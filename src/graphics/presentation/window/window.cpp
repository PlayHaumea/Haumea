#include "graphics/presentation/window.h"

#include "SDL.h"
#include "SDL_error.h"
#include "SDL_events.h"
#include "SDL_gamecontroller.h"
#include "SDL_hints.h"
#include "SDL_joystick.h"
#include "SDL_keyboard.h"
#include "SDL_keycode.h"
#include "SDL_mouse.h"
#include "SDL_pixels.h"
#include "SDL_rwops.h"
#include "SDL_stdinc.h"
#include "SDL_surface.h"
#include "SDL_thread.h"
#include "SDL_touch.h"
#include "SDL_video.h"
#include "SDL_vulkan.h"
#include "common/assert.h"
#include "common/common.h"
#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/logging/log.h"
#include "common/profiler.h"
#include "common/stringUtils.h"
#include "common/systemInfo.h"
#include "common/threads.h"
#include "common/timer.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/render.h"
#include "graphics/host_gpu/vma.h"
#include "graphics/host_gpu/vulkanCommon.h"
#include "graphics/presentation/imeOverlay.h"
#include "graphics/presentation/renderDoc.h"
#include "graphics/presentation/window/hostInput.h"
#include "graphics/presentation/window/windowInternal.h"
#if defined(__IPHONEOS__)
#include "graphics/presentation/window/loadingStatus.h"
#include <CoreFoundation/CoreFoundation.h>
#endif
#include "graphics/shader/shader.h"
#include "kytyGitVersion.h"
#include "libs/controller.h"
#include "loader/systemContent.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <vulkan/vk_platform.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_SIMD
#include "stb_image.h"

#include <fmt/format.h>

// IWYU pragma: no_include <intrin.h>

#define KYTY_ENABLE_DEBUG_PRINTF
#define KYTY_DBG_INPUT

namespace Libs::Graphics {

struct EventKeyboard {
	bool     down;
	bool     up;
	bool     pressed;
	bool     released;
	bool     repeat;
	int      scan_code;
	int      key_code;
	uint16_t mod;
	double   timestamp_seconds;
};

static uint32_t ControllerButtonToPadButton(int button) {
	switch (button) {
		case SDL_CONTROLLER_BUTTON_A: return Controller::PAD_BUTTON_CROSS;
		case SDL_CONTROLLER_BUTTON_B: return Controller::PAD_BUTTON_CIRCLE;
		case SDL_CONTROLLER_BUTTON_X: return Controller::PAD_BUTTON_SQUARE;
		case SDL_CONTROLLER_BUTTON_Y: return Controller::PAD_BUTTON_TRIANGLE;
		case SDL_CONTROLLER_BUTTON_BACK: return Controller::PAD_BUTTON_SHARE;
		case SDL_CONTROLLER_BUTTON_START: return Controller::PAD_BUTTON_OPTIONS;
		case SDL_CONTROLLER_BUTTON_LEFTSTICK: return Controller::PAD_BUTTON_L3;
		case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return Controller::PAD_BUTTON_R3;
		case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return Controller::PAD_BUTTON_L1;
		case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return Controller::PAD_BUTTON_R1;
		case SDL_CONTROLLER_BUTTON_DPAD_UP: return Controller::PAD_BUTTON_UP;
		case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Controller::PAD_BUTTON_DOWN;
		case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Controller::PAD_BUTTON_LEFT;
		case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Controller::PAD_BUTTON_RIGHT;
		case SDL_CONTROLLER_BUTTON_TOUCHPAD: return Controller::PAD_BUTTON_TOUCH_PAD;
		default: return 0;
	}
}

static Controller::Axis ControllerAxisFromSdl(int axis_id) {
	switch (axis_id) {
		case SDL_CONTROLLER_AXIS_LEFTX: return Controller::Axis::LeftX;
		case SDL_CONTROLLER_AXIS_LEFTY: return Controller::Axis::LeftY;
		case SDL_CONTROLLER_AXIS_RIGHTX: return Controller::Axis::RightX;
		case SDL_CONTROLLER_AXIS_RIGHTY: return Controller::Axis::RightY;
		case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return Controller::Axis::TriggerLeft;
		case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return Controller::Axis::TriggerRight;
		default: return Controller::Axis::AxisMax;
	}
}

static bool ControllerAxisIsTrigger(int axis_id) {
	return axis_id == SDL_CONTROLLER_AXIS_TRIGGERLEFT ||
	       axis_id == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
}

static int ControllerAxisValueFromSdl(int axis_id, int axis_value) {
	return ControllerAxisIsTrigger(axis_id)
	           ? Controller::controller_get_axis(0, SDL_JOYSTICK_AXIS_MAX, axis_value)
	           : Controller::controller_get_axis(SDL_JOYSTICK_AXIS_MIN, SDL_JOYSTICK_AXIS_MAX,
	                                             axis_value);
}

static int g_device_for_port[Controller::PAD_PORT_MAX] = {-1, -1, -1, -1};

static int PortForDevice(int device_id) {
	for (int port = 0; port < Controller::PAD_PORT_MAX; port++) {
		if (g_device_for_port[port] == device_id) {
			return port;
		}
	}
	return -1;
}

static int ClaimPortForDevice(int device_id) {
	if (const int existing = PortForDevice(device_id); existing >= 0) {
		return existing;
	}
	for (int port = 0; port < Controller::PAD_PORT_MAX; port++) {
		if (g_device_for_port[port] < 0) {
			g_device_for_port[port] = device_id;
			return port;
		}
	}
	return -1;
}

static void ReleasePortForDevice(int device_id) {
	if (const int port = PortForDevice(device_id); port >= 0) {
		g_device_for_port[port] = -1;
		Controller::ControllerSetPortConnected(port, false);
	}
}

static void OpenController(int device_index) {
	if (SDL_IsGameController(device_index) != SDL_TRUE) {
		return;
	}

	const auto instance_id = SDL_JoystickGetDeviceInstanceID(device_index);
	if (instance_id >= 0 && SDL_GameControllerFromInstanceID(instance_id) != nullptr) {
		return;
	}

	auto* pad = SDL_GameControllerOpen(device_index);
	if (pad == nullptr) {
		LOGF("Controller open failed: device = %d, error = %s\n", device_index, SDL_GetError());
		return;
	}

	const int id = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad));
	if (id < 0) {
		LOGF("Controller instance failed: device = %d, error = %s\n", device_index,
		     SDL_GetError());
		SDL_GameControllerClose(pad);
		return;
	}

	const char* name = SDL_GameControllerName(pad);
	LOGF("Controller opened: device = %d, instance = %d, name = %s\n", device_index, id,
	     name != nullptr ? name : "Unknown");
	const int port = ClaimPortForDevice(id);
	if (port < 0) {
		SDL_GameControllerClose(pad);
		return;
	}
	Controller::ControllerConnect(port, id);
	Controller::ControllerSetPortConnected(port, true);
}

struct EventMouse {
	bool   down;
	bool   up;
	bool   left;
	bool   middle;
	bool   right;
	bool   x1;
	bool   x2;
	bool   touch;
	bool   pressed;
	bool   released;
	int    num_of_clicks;
	bool   wheel;
	int    x;
	int    y;
	bool   motion;
	int    motion_x;
	int    motion_y;
	double timestamp_seconds;
};

struct EventFinger {
	bool   down;
	bool   up;
	bool   motion;
	int    touch_id;
	int    finger_id;
	float  x;
	float  y;
	float  dx;
	float  dy;
	float  pressure;
	double timestamp_seconds;
};

struct EventController {
	int    id;
	int    button;
	int    axis_id;
	int    axis_value;
	bool   down;
	bool   up;
	bool   added;
	bool   removed;
	bool   remapped;
	bool   axis;
	bool   pressed;
	bool   released;
	double timestamp_seconds;
};

enum class DisplayOrientation {
	Unknown,   /* The display orientation can't be determined */
	Landscape, /* The display is in landscape mode, with the right side up, relative to portrait
	              mode */
	LandscapeFlipped, /* The display is in landscape mode, with the left side up, relative to
	                     portrait mode */
	Portrait,         /* The display is in portrait mode */
	PortraitFlipped,  /* The display is in portrait mode, upside down */

	DisplayEventOrientation = 0xF0
};

struct EventDisplay {
	DisplayOrientation orientation;
};

constexpr uint32_t KYTY_SDL_BUTTON_LMASK  = SDL_BUTTON_LMASK;  // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t KYTY_SDL_BUTTON_MMASK  = SDL_BUTTON_MMASK;  // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t KYTY_SDL_BUTTON_RMASK  = SDL_BUTTON_RMASK;  // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t KYTY_SDL_BUTTON_X1MASK = SDL_BUTTON_X1MASK; // NOLINT(hicpp-signed-bitwise)
constexpr uint32_t KYTY_SDL_BUTTON_X2MASK = SDL_BUTTON_X2MASK; // NOLINT(hicpp-signed-bitwise)

namespace {

std::unique_ptr<WindowContext> g_window;

void*    g_external_surface = nullptr;
uint32_t g_external_width   = 0;
uint32_t g_external_height  = 0;
std::atomic_bool g_app_paused {false};

} // namespace

void SetExternalSurface(void* metal_layer, uint32_t width, uint32_t height) {
	g_external_surface = metal_layer;
	g_external_width   = width;
	g_external_height  = height;
}

void* ExternalSurface() {
	return g_external_surface;
}

void ExternalSurfaceSize(uint32_t* width, uint32_t* height) {
	*width  = g_external_width;
	*height = g_external_height;
}

void SetAppPaused(bool paused) {
	g_app_paused.store(paused, std::memory_order_release);
	std::printf("Magnus:Lifecycle:Info: renderer paused=%d\n", paused ? 1 : 0);
}

bool IsAppPaused() {
	return g_app_paused.load(std::memory_order_acquire);
}

uint64_t GuestFrameCount() {
	return g_window == nullptr ? 0 : g_window->guest_frames.load(std::memory_order_relaxed);
}

constexpr const char* KYTY_SDL_WINDOW_CAPTION = "Game";
constexpr uint32_t    KYTY_SDL_WINDOW_FLAGS =
    (static_cast<uint32_t>(SDL_WINDOW_HIDDEN) | static_cast<uint32_t>(SDL_WINDOW_VULKAN));
constexpr int KYTY_SDL_WINDOWPOS_CENTERED = SDL_WINDOWPOS_CENTERED; /*NOLINT(hicpp-signed-bitwise)*/

static void SetPause(WindowLoopState& game, bool flag) {
	LOGF("Pause: %s\n", flag ? "true" : "false");

	game.paused.store(flag, std::memory_order_release);
}

static void GameEventQuit(WindowLoopState& game) {
	LOGF("Event: quit\n");

	game.need_exit = true;
}

static void GameEventTerminate(WindowLoopState& game) {
	LOGF("Event: terminate\n");

	game.need_exit = true;
}

static void ToggleDesktopFullscreen() {
	if (g_window == nullptr || g_window->window == nullptr) {
		return;
	}

	const auto flags = static_cast<uint32_t>(SDL_GetWindowFlags(g_window->window));
	const bool fullscreen =
	    (flags & static_cast<uint32_t>(SDL_WINDOW_FULLSCREEN_DESKTOP)) != 0u;
	const auto mode =
	    fullscreen ? 0u : static_cast<uint32_t>(SDL_WINDOW_FULLSCREEN_DESKTOP);
	if (SDL_SetWindowFullscreen(g_window->window, mode) != 0) {
		LOGF("Toggle fullscreen failed: %s\n", SDL_GetError());
	}
}

static void GameEventKeyboard(WindowLoopState& game, const EventKeyboard& key) {
	static SDL_Keycode fullscreen_key = SDLK_UNKNOWN;

#ifdef KYTY_DBG_INPUT
	LOGF("Key: time = %.04f, %s%s, %s%s, %s, scan = %d, key = %d, mod = %04" PRIx16 "\n",
	     key.timestamp_seconds, (key.down ? "down" : ""), (key.up ? "up" : ""),
	     (key.pressed ? "pressed" : ""), (key.released ? "released" : ""),
	     (key.repeat ? "repeat" : ""), key.scan_code, key.key_code, key.mod);
#endif

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS || KYTY_PLATFORM == KYTY_PLATFORM_LINUX
	if (key.down) {
		switch (key.key_code) {
			case SDLK_ESCAPE: game.need_exit = true; break;
			case SDLK_SPACE: SetPause(game, !game.paused.load(std::memory_order_acquire)); break;
			case SDLK_F1:
				if (!key.repeat) {
					RenderDocRequestCapture();
				}
				break;
			case SDLK_F11:
				if (!key.repeat) {
					ToggleDesktopFullscreen();
				}
				break;
			case SDLK_RETURN:
			case SDLK_KP_ENTER:
				if (!key.repeat && (key.mod & KMOD_ALT) != 0) {
					ToggleDesktopFullscreen();
					fullscreen_key = key.key_code;
				}
				break;
			default: break;
		}
	}

#endif

	const bool fullscreen_key_event =
	    fullscreen_key != SDLK_UNKNOWN && key.key_code == fullscreen_key;
	if (fullscreen_key_event && key.up) {
		fullscreen_key = SDLK_UNKNOWN;
	}
	if ((key.down || key.up) && !key.repeat && !fullscreen_key_event) {
		HostInputKey(key.key_code, key.down);
	}
}

static void GameEventMouse([[maybe_unused]] const EventMouse& mb) {
#ifdef KYTY_DBG_INPUT
	if (mb.wheel) {
		LOGF("Mouse wheel: time = %.04f, %s[%d, %d]\n", mb.timestamp_seconds,
		     (mb.touch ? "touch, " : ""), mb.x, mb.y);
	} else if (mb.motion) {
		LOGF("Mouse motion: time = %.04f, %s%s%s%s%s%s, [%d, %d], (%d, %d)\n", mb.timestamp_seconds,
		     (mb.left ? "left" : ""), (mb.middle ? "middle" : ""), (mb.right ? "right" : ""),
		     (mb.x1 ? "x1" : ""), (mb.x2 ? "x2" : ""), (mb.touch ? "_touch" : ""), mb.x, mb.y,
		     mb.motion_x, mb.motion_y);
	} else {
		LOGF("Mouse click: time = %.04f, %d, %s%s%s%s%s%s, %s%s, %s%s, [%d, %d]\n",
		     mb.timestamp_seconds, mb.num_of_clicks, (mb.left ? "left" : ""),
		     (mb.middle ? "middle" : ""), (mb.right ? "right" : ""), (mb.x1 ? "x1" : ""),
		     (mb.x2 ? "x2" : ""), (mb.touch ? "_touch" : ""), (mb.down ? "down" : ""),
		     (mb.up ? "up" : ""), (mb.pressed ? "pressed" : ""), (mb.released ? "released" : ""),
		     mb.x, mb.y);
	}
#endif

	if (mb.down || mb.up) {
		uint8_t mouse_button = 0;
		if (mb.left) {
			mouse_button = SDL_BUTTON_LEFT;
		} else if (mb.middle) {
			mouse_button = SDL_BUTTON_MIDDLE;
		} else if (mb.right) {
			mouse_button = SDL_BUTTON_RIGHT;
		} else if (mb.x1) {
			mouse_button = SDL_BUTTON_X1;
		} else if (mb.x2) {
			mouse_button = SDL_BUTTON_X2;
		}

		HostInputMouseButton(mouse_button, mb.down);
	}
}

static void GameEventFinger([[maybe_unused]] const EventFinger& f) {
#ifdef KYTY_DBG_INPUT
	if (f.motion) {
		LOGF("Finger motion: time = %.04f, %d, %d, (x,y) = [%f, %f], (dx,dy) = [%f, %f], pressure "
		     "= %f\n",
		     f.timestamp_seconds, f.touch_id, f.finger_id, f.x, f.y, f.dx, f.dy, f.pressure);
	} else {
		LOGF("Finger press: time = %.04f, %d, %d, %s%s, (x,y) = [%f, %f], (dx,dy) = [%f, %f], "
		     "pressure = %f\n",
		     f.timestamp_seconds, f.touch_id, f.finger_id, (f.down ? "down" : ""),
		     (f.up ? "up" : ""), f.x, f.y, f.dx, f.dy, f.pressure);
	}
#endif
}

struct PadEvent {
	uint64_t frame;
	uint64_t micros;
	char     kind;
	int      id;
	int      a;
	int      b;
};

static FILE*                 g_pad_record_file    = nullptr;
static bool                  g_pad_record_checked = false;
static std::vector<PadEvent> g_pad_replay_events;
static size_t                g_pad_replay_next    = 0;
static bool                  g_pad_replay_by_time = false;
static uint64_t              g_pad_origin         = 0;

static uint64_t PadMicros() {
	const auto frequency = Common::Timer::QueryPerformanceFrequency();
	const auto now       = Common::Timer::QueryPerformanceCounter();
	if (g_pad_origin == 0) {
		g_pad_origin = now;
	}
	return frequency == 0 ? 0 : (now - g_pad_origin) * 1000000ull / frequency;
}

static void PadRecordOpen() {
	if (g_pad_record_checked) {
		return;
	}
	g_pad_record_checked = true;
	const char* path     = std::getenv("MAGNUS_PAD_RECORD");
	if (path == nullptr || path[0] == '\0') {
		return;
	}
	g_pad_record_file = std::fopen(path, "w");
	std::printf("Magnus:Pad:Info: record %s path=%s\n",
	            g_pad_record_file != nullptr ? "open" : "FAILED", path);
	std::fflush(stdout);
}

static void PadRecordEvent(const EventController& f) {
	PadRecordOpen();
	if (g_pad_record_file == nullptr) {
		return;
	}

	char kind = 0;
	int  a    = 0;
	int  b    = 0;
	if (f.added) {
		kind = '+';
	} else if (f.removed) {
		kind = '-';
	} else if (f.axis) {
		kind = 'x';
		a    = f.axis_id;
		b    = f.axis_value;
	} else if (f.down || f.up) {
		kind = 'b';
		a    = f.button;
		b    = f.down ? 1 : 0;
	} else {
		return;
	}

	std::fprintf(g_pad_record_file, "%llu %llu %c %d %d %d\n",
	             static_cast<unsigned long long>(GuestFrameCount()),
	             static_cast<unsigned long long>(PadMicros()), kind, f.id, a, b);
	std::fflush(g_pad_record_file);
}

static void GameEventController([[maybe_unused]] const EventController& f) {
	PadRecordEvent(f);
#ifdef KYTY_DBG_INPUT
	if (f.added || f.removed || f.remapped) {
		const char* action = f.added ? "added" : (f.removed ? "removed" : "remapped");
		LOGF("Controller %s: %d, time = %.04f\n", action, f.id, f.timestamp_seconds);
	} else if (f.axis) {
		LOGF("Controller axis: %d, axis = %d, value = %d, time = %.04f\n", f.id, f.axis_id,
		     f.axis_value, f.timestamp_seconds);
	} else {
		LOGF("Controller button: "
		     "%d, %s%s, %s%s, button = %d, time = %.04f\n",
		     f.id, (f.down ? "down" : ""), (f.up ? "up" : ""), (f.pressed ? "pressed" : ""),
		     (f.released ? "released" : ""), f.button, f.timestamp_seconds);
	}
#endif

	if (f.added) {
		OpenController(f.id);
	}

	if (f.removed) {
		if (const int port = PortForDevice(f.id); port >= 0) {
			Controller::ControllerDisconnect(port, f.id);
		}
		ReleasePortForDevice(f.id);
		if (auto* pad = SDL_GameControllerFromInstanceID(f.id); pad != nullptr) {
			SDL_GameControllerClose(pad);
		}
	}

	const int port = PortForDevice(f.id);

	if (port >= 0 && (f.down || f.up)) {
		const auto button = ControllerButtonToPadButton(f.button);
		if (button != 0) {
			Controller::ControllerButton(port, f.id, button, f.down);
		}
	}

	if (port >= 0 && f.axis) {
		const auto axis = ControllerAxisFromSdl(f.axis_id);
		if (axis != Controller::Axis::AxisMax) {
			Controller::ControllerAxis(port, f.id, axis,
			                           ControllerAxisValueFromSdl(f.axis_id, f.axis_value));
		}
	}
}

static void PadReplayConnect(int id) {
	const int port = ClaimPortForDevice(id);
	if (port < 0) {
		return;
	}
	Controller::ControllerConnect(port, id);
	Controller::ControllerSetPortConnected(port, true);
	std::printf("Magnus:Pad:Info: replay pad connected port=%d id=%d\n", port, id);
}

static void PadReplayLoad() {
	const char* path = std::getenv("MAGNUS_PAD_REPLAY");
	if (path == nullptr || path[0] == '\0') {
		return;
	}
	FILE* file = std::fopen(path, "r");
	if (file == nullptr) {
		std::printf("Magnus:Pad:Error: replay open failed path=%s\n", path);
		return;
	}
	char line[256];
	while (std::fgets(line, sizeof(line), file) != nullptr) {
		unsigned long long frame  = 0;
		unsigned long long micros = 0;
		char               kind   = 0;
		int                id     = 0;
		int                a      = 0;
		int                b      = 0;
		if (std::sscanf(line, "%llu %llu %c %d %d %d", &frame, &micros, &kind, &id, &a, &b) == 6) {
			g_pad_replay_events.push_back(PadEvent {frame, micros, kind, id, a, b});
		}
	}
	std::fclose(file);
	const char* clock    = std::getenv("MAGNUS_PAD_REPLAY_CLOCK");
	g_pad_replay_by_time = clock != nullptr && (clock[0] == 't' || clock[0] == 'T');
	std::printf("Magnus:Pad:Info: replay loaded events=%zu clock=%s path=%s\n",
	            g_pad_replay_events.size(), g_pad_replay_by_time ? "time" : "frame", path);
}

static bool PadReplayArmed() {
	return !g_pad_replay_events.empty();
}

static void PadReplayPump() {
	if (g_pad_replay_next >= g_pad_replay_events.size()) {
		return;
	}
	const uint64_t frame  = GuestFrameCount();
	const uint64_t micros = PadMicros();
	while (g_pad_replay_next < g_pad_replay_events.size()) {
		const auto& e   = g_pad_replay_events[g_pad_replay_next];
		const bool  due = g_pad_replay_by_time ? micros >= e.micros : frame >= e.frame;
		if (!due) {
			break;
		}
		g_pad_replay_next++;
		if (e.kind == '+') {
			PadReplayConnect(e.id);
			continue;
		}
		EventController c {};
		c.id = e.id;
		switch (e.kind) {
			case '-':
				c.removed = true;
				break;
			case 'x':
				c.axis       = true;
				c.axis_id    = e.a;
				c.axis_value = e.b;
				break;
			case 'b':
				c.button = e.a;
				c.down   = e.b != 0;
				c.up     = e.b == 0;
				break;
			default: continue;
		}
		GameEventController(c);
	}
}

static void GameEventLowMemory() {
	LOGF("Event: low_memory\n");
}

static void GameEventWillEnterBackground(WindowLoopState& game) {
	LOGF("Event: will_enter_background\n");

	SetPause(game, true);
}

static void GameEventDidEnterBackground() {
	LOGF("Event: did_enter_background\n");
}

static void GameEventWillEnterForeground() {
	LOGF("Event: will_enter_foreground\n");
}

static void GameEventDidEnterForeground(WindowLoopState& game) {
	LOGF("Event: did_enter_foreground\n");

	SetPause(game, false);
}

void WindowContext::Resize(uint32_t new_width, uint32_t new_height) {
	EXIT_IF(new_width == 0 || new_height == 0);
	graphic_ctx.screen_width  = new_width;
	graphic_ctx.screen_height = new_height;
}

void WindowContext::ProcessWindowEvent(const SDL_WindowEvent& event) {
	const auto& window_event = event;
	switch (window_event.event) {
		case SDL_WINDOWEVENT_SHOWN:
			LOGF("Window %" PRIu32 " shown\n", window_event.windowID);
			break;

		case SDL_WINDOWEVENT_HIDDEN:
			LOGF("Window %" PRIu32 " hidden\n", window_event.windowID);
			break;

		case SDL_WINDOWEVENT_EXPOSED:
			LOGF("Window %" PRIu32 " exposed\n", window_event.windowID);
			break;

		case SDL_WINDOWEVENT_MOVED:
			LOGF("Window %" PRIu32 " moved to %" PRId32 ",%" PRId32 "\n", window_event.windowID,
			     window_event.data1, window_event.data2);
			break;

		case SDL_WINDOWEVENT_RESIZED:
			LOGF("Window %" PRIu32 " resized to %" PRId32 "x%" PRId32 "\n", window_event.windowID,
			     window_event.data1, window_event.data2);

			LOGF("m: %d\n", static_cast<int>(SDL_ThreadID()));
			Resize(window_event.data1, window_event.data2);

			break;

		case SDL_WINDOWEVENT_SIZE_CHANGED:
			LOGF("Window %" PRIu32 " size changed to %" PRId32 "x%" PRId32 "\n",
			     window_event.windowID, window_event.data1, window_event.data2);

			LOGF("m: %d\n", static_cast<int>(SDL_ThreadID()));
			Resize(window_event.data1, window_event.data2);

			break;

		case SDL_WINDOWEVENT_MINIMIZED:
			LOGF("Window %" PRIu32 " minimized\n", window_event.windowID);
			break;
		case SDL_WINDOWEVENT_MAXIMIZED:
			LOGF("Window %" PRIu32 " maximized\n", window_event.windowID);
			break;
		case SDL_WINDOWEVENT_RESTORED:
			LOGF("Window %" PRIu32 " restored\n", window_event.windowID);
			break;
		case SDL_WINDOWEVENT_ENTER:
			LOGF("Mouse entered window %" PRIu32 "\n", window_event.windowID);
			break;
		case SDL_WINDOWEVENT_LEAVE:
			LOGF("Mouse left window %" PRIu32 "\n", window_event.windowID);
			break;
		case SDL_WINDOWEVENT_FOCUS_GAINED:
			LOGF("Window %" PRIu32 " gained keyboard focus\n", window_event.windowID);
			break;
		case SDL_WINDOWEVENT_FOCUS_LOST:
			LOGF("Window %" PRIu32 " lost keyboard focus\n", window_event.windowID);
			break;
		case SDL_WINDOWEVENT_CLOSE:
			LOGF("Window %" PRIu32 " closed\n", window_event.windowID);
			break;
		default:
			LOGF("Window %" PRIu32 " got unknown event %" PRIu8 "\n", window_event.windowID,
			     window_event.event);
			break;
	}
}

void WindowContext::ProcessDisplayEvent(const SDL_DisplayEvent& display) {
	bool sdl = false;

	switch (display.event) {
		case SDL_DISPLAYEVENT_ORIENTATION: sdl = true; [[fallthrough]];
		case static_cast<Uint8>(DisplayOrientation::DisplayEventOrientation): {
			LOGF("Display %" PRIu32 "[%s] changed orientation to %d - ", display.display,
			     sdl ? "SDL" : "Magnus", static_cast<int>(display.data1));

			switch (display.data1) {
				case SDL_ORIENTATION_UNKNOWN: LOGF("UNKNOWN\n"); break;
				case SDL_ORIENTATION_LANDSCAPE: LOGF("LANDSCAPE\n"); break;
				case SDL_ORIENTATION_LANDSCAPE_FLIPPED: LOGF("LANDSCAPE_FLIPPED\n"); break;
				case SDL_ORIENTATION_PORTRAIT: LOGF("PORTRAIT\n"); break;
				case SDL_ORIENTATION_PORTRAIT_FLIPPED: LOGF("PORTRAIT_FLIPPED\n"); break;
				default: LOGF("???\n");
			}

			break;
		}
		default:
			LOGF("Display %" PRIu32 " got unknown event 0x%" PRIx8 "\n", display.display,
			     display.event);
			break;
	}
}

void WindowContext::ProcessEvent(double time_s) {
	auto& game  = loop;
	auto* event = &game.event;
	EXIT_IF(SDL_GetEventState(SDL_DISPLAYEVENT) != SDL_ENABLE);
	if (ProcessImeInput(*event)) {
		return;
	}

	switch (event->type) {
		case SDL_QUIT: GameEventQuit(game); break;

		case SDL_APP_TERMINATING: GameEventTerminate(game); break;

		case SDL_APP_LOWMEMORY: GameEventLowMemory(); break;

		case SDL_APP_WILLENTERBACKGROUND: GameEventWillEnterBackground(game); break;

		case SDL_APP_DIDENTERBACKGROUND: GameEventDidEnterBackground(); break;

		case SDL_APP_WILLENTERFOREGROUND: GameEventWillEnterForeground(); break;

		case SDL_APP_DIDENTERFOREGROUND: GameEventDidEnterForeground(game); break;

		case SDL_KEYDOWN:
		case SDL_KEYUP: {
			EventKeyboard key {};

			key.down              = (event->type == SDL_KEYDOWN);
			key.up                = (event->type == SDL_KEYUP);
			key.pressed           = (event->key.state == SDL_PRESSED);
			key.released          = (event->key.state == SDL_RELEASED);
			key.repeat            = (event->key.repeat != 0u);
			key.scan_code         = event->key.keysym.scancode;
			key.key_code          = event->key.keysym.sym;
			key.mod               = event->key.keysym.mod;
			key.timestamp_seconds = time_s;

			GameEventKeyboard(game, key);

			break;
		}

		case SDL_WINDOWEVENT: ProcessWindowEvent(event->window); break;

		case SDL_DISPLAYEVENT: ProcessDisplayEvent(event->display); break;

		case SDL_MOUSEBUTTONDOWN:
		case SDL_MOUSEBUTTONUP: {
			EventMouse mb {};

			mb.down              = (event->button.type == SDL_MOUSEBUTTONDOWN);
			mb.up                = (event->button.type == SDL_MOUSEBUTTONUP);
			mb.left              = (event->button.button == SDL_BUTTON_LEFT);
			mb.middle            = (event->button.button == SDL_BUTTON_MIDDLE);
			mb.right             = (event->button.button == SDL_BUTTON_RIGHT);
			mb.x1                = (event->button.button == SDL_BUTTON_X1);
			mb.x2                = (event->button.button == SDL_BUTTON_X2);
			mb.touch             = (event->button.which == SDL_TOUCH_MOUSEID);
			mb.pressed           = (event->button.state == SDL_PRESSED);
			mb.released          = (event->button.state == SDL_RELEASED);
			mb.num_of_clicks     = event->button.clicks;
			mb.wheel             = false;
			mb.x                 = event->button.x;
			mb.y                 = event->button.y;
			mb.motion            = false;
			mb.motion_x          = 0;
			mb.motion_y          = 0;
			mb.timestamp_seconds = time_s;

			GameEventMouse(mb);

			break;
		}

		case SDL_MOUSEWHEEL: {
			EventMouse mb {};

			mb.down              = false;
			mb.up                = false;
			mb.left              = false;
			mb.middle            = false;
			mb.right             = false;
			mb.x1                = false;
			mb.x2                = false;
			mb.touch             = (event->wheel.which == SDL_TOUCH_MOUSEID);
			mb.pressed           = false;
			mb.released          = false;
			mb.num_of_clicks     = 0;
			mb.wheel             = true;
			mb.x                 = event->wheel.x;
			mb.y                 = event->wheel.y;
			mb.motion            = false;
			mb.motion_x          = 0;
			mb.motion_y          = 0;
			mb.timestamp_seconds = time_s;

			GameEventMouse(mb);

			break;
		}

		case SDL_MOUSEMOTION: {
			EventMouse mb {};

			mb.down              = false;
			mb.up                = false;
			mb.left              = ((event->motion.state & KYTY_SDL_BUTTON_LMASK) != 0u);
			mb.middle            = ((event->motion.state & KYTY_SDL_BUTTON_MMASK) != 0u);
			mb.right             = ((event->motion.state & KYTY_SDL_BUTTON_RMASK) != 0u);
			mb.x1                = ((event->motion.state & KYTY_SDL_BUTTON_X1MASK) != 0u);
			mb.x2                = ((event->motion.state & KYTY_SDL_BUTTON_X2MASK) != 0u);
			mb.touch             = (event->motion.which == SDL_TOUCH_MOUSEID);
			mb.pressed           = false;
			mb.released          = false;
			mb.num_of_clicks     = 0;
			mb.wheel             = false;
			mb.x                 = event->motion.x;
			mb.y                 = event->motion.y;
			mb.motion            = true;
			mb.motion_x          = event->motion.xrel;
			mb.motion_y          = event->motion.yrel;
			mb.timestamp_seconds = time_s;

			GameEventMouse(mb);

			break;
		}

		case SDL_FINGERMOTION:
		case SDL_FINGERDOWN:
		case SDL_FINGERUP: {
			EventFinger f {};

			f.down              = (event->tfinger.type == SDL_FINGERDOWN);
			f.up                = (event->tfinger.type == SDL_FINGERUP);
			f.motion            = (event->tfinger.type == SDL_FINGERMOTION);
			f.finger_id         = static_cast<int>(event->tfinger.fingerId);
			f.touch_id          = static_cast<int>(event->tfinger.touchId);
			f.x                 = event->tfinger.x;
			f.y                 = event->tfinger.y;
			f.dx                = event->tfinger.dx;
			f.dy                = event->tfinger.dy;
			f.pressure          = event->tfinger.pressure;
			f.timestamp_seconds = time_s;

			GameEventFinger(f);

			break;
		}

		case SDL_CONTROLLERAXISMOTION: {
			EventController c {};

			c.id                = event->caxis.which;
			c.button            = SDL_CONTROLLER_BUTTON_INVALID;
			c.axis_id           = event->caxis.axis;
			c.axis_value        = event->caxis.value;
			c.down              = false;
			c.up                = false;
			c.added             = false;
			c.removed           = false;
			c.remapped          = false;
			c.axis              = true;
			c.pressed           = false;
			c.released          = false;
			c.timestamp_seconds = time_s;

			GameEventController(c);

			break;
		}

		case SDL_CONTROLLERBUTTONDOWN:
		case SDL_CONTROLLERBUTTONUP: {
			EventController c {};

			c.id                = event->cbutton.which;
			c.button            = event->cbutton.button;
			c.axis_id           = SDL_CONTROLLER_AXIS_INVALID;
			c.axis_value        = 0;
			c.down              = (event->cbutton.type == SDL_CONTROLLERBUTTONDOWN);
			c.up                = (event->cbutton.type == SDL_CONTROLLERBUTTONUP);
			c.added             = false;
			c.removed           = false;
			c.remapped          = false;
			c.axis              = false;
			c.pressed           = (event->cbutton.state == SDL_PRESSED);
			c.released          = (event->cbutton.state == SDL_RELEASED);
			c.timestamp_seconds = time_s;

			GameEventController(c);

			break;
		}

		case SDL_CONTROLLERDEVICEADDED:
		case SDL_CONTROLLERDEVICEREMOVED:
		case SDL_CONTROLLERDEVICEREMAPPED: {
			EventController c {};

			c.id                = event->cdevice.which;
			c.button            = SDL_CONTROLLER_BUTTON_INVALID;
			c.axis_id           = SDL_CONTROLLER_AXIS_INVALID;
			c.axis_value        = 0;
			c.down              = false;
			c.up                = false;
			c.added             = (event->cdevice.type == SDL_CONTROLLERDEVICEADDED);
			c.removed           = (event->cdevice.type == SDL_CONTROLLERDEVICEREMOVED);
			c.remapped          = (event->cdevice.type == SDL_CONTROLLERDEVICEREMAPPED);
			c.axis              = false;
			c.pressed           = false;
			c.released          = false;
			c.timestamp_seconds = time_s;

			GameEventController(c);

			break;
		}
	}
}

#if defined(__APPLE__)
void WindowContext::RunOnMainThread(std::function<void()> task, bool wait) {
	if (Common::Thread::IsMainThread()) {
		task();
		return;
	}

	uint64_t ticket = 0;
	{
		Common::LockGuard lock(main_task_mutex);
		main_tasks.push_back(std::move(task));
		ticket = ++main_tasks_queued;
	}

	SDL_Event event {};
	event.type = SDL_USEREVENT;
	SDL_PushEvent(&event);

	if (!wait) {
		return;
	}
	Common::LockGuard lock(main_task_mutex);
	while (main_tasks_run < ticket) {
		main_task_done.Wait(&main_task_mutex);
	}
}

void WindowContext::DrainMainThreadTasks() {
	std::vector<std::function<void()>> tasks;
	{
		Common::LockGuard lock(main_task_mutex);
		tasks.swap(main_tasks);
	}
	if (tasks.empty()) {
		return;
	}
	for (auto& task: tasks) {
		task();
	}
	Common::LockGuard lock(main_task_mutex);
	main_tasks_run += tasks.size();
	main_task_done.SignalAll();
}
#endif

static Uint32 TitleTimer(Uint32 interval, void*) {
	SDL_Event event {};
	event.type = SDL_USEREVENT;
	return SDL_PushEvent(&event) >= 0 ? interval : 0;
}

void WindowContext::Run() {
	Common::Timer timer;
	timer.Start();

	loop.event     = {};
	loop.need_exit = false;
	loop.paused.store(false, std::memory_order_release);

	PadRecordOpen();
	PadReplayLoad();

	if (ExternalSurface() != nullptr) {
		while (!loop.need_exit) {
#if defined(__APPLE__)
			DrainMainThreadTasks();
#endif
			SDL_PumpEvents();
			while (SDL_PollEvent(&loop.event) != 0) {
				ProcessEvent(timer.GetTimeS());
			}
			PadReplayPump();
#if defined(__IPHONEOS__)
			if (Common::Thread::IsMainThread()) {
				CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.002, true);
			} else {
				Common::Thread::SleepMicro(2000);
			}
#else
			Common::Thread::SleepMicro(2000);
#endif
		}
		return;
	}

	const SDL_TimerID title_timer = SDL_AddTimer(100, TitleTimer, nullptr);
	EXIT_IF(title_timer == 0);

	while (!loop.need_exit) {
#if defined(__APPLE__)
		DrainMainThreadTasks();
#endif
		UpdateTitle();
		if (loop.paused.load(std::memory_order_acquire)) {
			if (!timer.IsPaused()) {
				timer.Pause();
			}
		} else if (timer.IsPaused()) {
			timer.Resume();
		}

		if (PadReplayArmed()) {
			if (SDL_WaitEventTimeout(&loop.event, 2) != 0) {
				ProcessEvent(timer.GetTimeS());
			}
			PadReplayPump();
			continue;
		}

		if (SDL_WaitEvent(&loop.event) == 0) {
			EXIT("%s\n", SDL_GetError());
		}
		ProcessEvent(timer.GetTimeS());
	}
	SDL_RemoveTimer(title_timer);
}

static void WindowCreate(WindowContext& context) {
	EXIT_IF(context.window != nullptr);
	EXIT_IF(context.graphic_ctx.screen_width == 0);
	EXIT_IF(context.graphic_ctx.screen_height == 0);

	int width  = static_cast<int>(context.graphic_ctx.screen_width);
	int height = static_cast<int>(context.graphic_ctx.screen_height);

#if KYTY_PLATFORM == KYTY_PLATFORM_WINDOWS
	SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "0");
#endif

	if (ExternalSurface() != nullptr) {
		SDL_SetMainReady();
		if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) < 0) {
			LOGF("No game controller support: %s\n", SDL_GetError());
		}
	} else if (SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) < 0) {
		EXIT("%s\n", SDL_GetError());
	}
	for (int device_index = 0; device_index < SDL_NumJoysticks(); device_index++) {
		OpenController(device_index);
	}
	HostInputInit();
	InitializeImeInput();

	LOGF("WindowCreate(): width = %d, height = %d\n", width, height);

	if (ExternalSurface() != nullptr) {
		LOGF("WindowCreate(): external surface %p\n", ExternalSurface());
		context.window_hidden = false;
		return;
	}

	uint32_t window_flags = KYTY_SDL_WINDOW_FLAGS;
	if (Config::FullscreenEnabled()) {
		window_flags |= static_cast<uint32_t>(SDL_WINDOW_FULLSCREEN_DESKTOP);
	}
#if defined(__APPLE__)
	if (std::getenv("KYTY_BORDERLESS") != nullptr) {
		window_flags |= static_cast<uint32_t>(SDL_WINDOW_BORDERLESS);
	}
#endif
	context.window = SDL_CreateWindow(KYTY_SDL_WINDOW_CAPTION, KYTY_SDL_WINDOWPOS_CENTERED,
	                                  KYTY_SDL_WINDOWPOS_CENTERED, width, height, window_flags);

	context.window_hidden = true;

	if (context.window == nullptr) {
		EXIT("%s\n", SDL_GetError());
	}

	SDL_SetWindowResizable(context.window, SDL_FALSE);
}

Presenter& WindowInit(uint32_t width, uint32_t height) {
	EXIT_NOT_IMPLEMENTED(!Common::Thread::IsMainThread());
	EXIT_IF(g_window != nullptr);

	auto window = std::make_unique<WindowContext>();

	window->graphic_ctx.screen_width  = width;
	window->graphic_ctx.screen_height = height;

	WindowCreate(*window);
	window->CreateVulkan();
	auto& presenter = *window->presenter;
	g_window        = std::move(window);
	return presenter;
}

void WindowRun() {
	KYTY_PROFILER_THREAD("Thread_Window");
	EXIT_IF(g_window == nullptr);

	g_window->Run();
}

void WindowShutdown() {
	if (g_window != nullptr) {
		g_window.reset();
	}
}

static int WindowIconRead(void* user, char* data, int size) {
	auto*    src        = static_cast<Common::File*>(user);
	uint32_t bytes_read = 0;
	src->Read(data, static_cast<uint32_t>(size), &bytes_read);
	return static_cast<int>(bytes_read);
}

static void WindowIconSkip(void* user, int n) {
	auto*          src      = static_cast<Common::File*>(user);
	const uint64_t position = src->Tell();

	if (n >= 0) {
		src->Seek(position + static_cast<uint64_t>(n));
	} else {
		const uint64_t distance = static_cast<uint64_t>(-static_cast<int64_t>(n));
		EXIT_IF(distance > position);
		src->Seek(position - distance);
	}
}

static int WindowIconEof(void* user) {
	auto* src = static_cast<Common::File*>(user);
	return src->IsEOF() ? 1 : 0;
}

struct WindowIcon {
	SDL_Surface* surface = nullptr;
	void*        pixels  = nullptr;

	~WindowIcon() {
		SDL_FreeSurface(surface);
		stbi_image_free(pixels);
	}
};

static void WindowLoadPngIcon(const std::string& path, WindowIcon* icon) {
	Common::File f;
	if (!f.Open(path, Common::File::Mode::Read)) {
		EXIT("Can't open icon file %s\n", path.c_str());
	}

	int width  = 0;
	int height = 0;

	stbi_io_callbacks cb {};
	cb.read = WindowIconRead;
	cb.skip = WindowIconSkip;
	cb.eof  = WindowIconEof;

	icon->pixels = stbi_load_from_callbacks(&cb, &f, &width, &height, nullptr, 4);
	f.Close();

	EXIT_IF(icon->pixels == nullptr);

	icon->surface = SDL_CreateRGBSurfaceWithFormatFrom(icon->pixels, width, height, 32, width * 4,
	                                                   SDL_PIXELFORMAT_RGBA32);
	EXIT_NOT_IMPLEMENTED(icon->surface == nullptr);
}

void WindowContext::UpdateIcon() {
	static WindowIcon icon;
	static bool       icon_loaded = false;

	if (!icon_loaded) {
		std::string icon_path;
		if (Loader::SystemContentGetIconPath(&icon_path)) {
			WindowLoadPngIcon(icon_path, &icon);
		}
		icon_loaded = true;
	}

	if (icon.surface != nullptr && window != nullptr) {
		SDL_SetWindowIcon(window, icon.surface);
	}
}

void WindowContext::NotePresent() {
	presented_frames.fetch_add(1, std::memory_order_relaxed);
}

void WindowContext::UpdateTitle() {
	static char title[128];
	static char title_id[12];
	static char app_ver[12];
	static bool has_title = Loader::SystemContentParamSfoGetString("TITLE", title, sizeof(title));
	static bool has_title_id =
	    Loader::SystemContentParamSfoGetString("TITLE_ID", title_id, sizeof(title_id));
	static bool has_app_ver =
	    Loader::SystemContentParamSfoGetString("APP_VER", app_ver, sizeof(app_ver));
	static std::string artwork_path;
	static bool has_artwork = Loader::SystemContentGetIconPath(&artwork_path);
	static uint64_t fps_start     = Common::Timer::QueryPerformanceCounter();
	static uint64_t elapsed_start = fps_start;
	static uint64_t title_update  = 0;
	static uint64_t fps_frames    = 0;
	static double   current_fps   = 0.0;

#if KYTY_BUILD == KYTY_BUILD_DEBUG
	static constexpr auto build_type = "Debug";
#elif KYTY_BUILD == KYTY_BUILD_RELEASE
	static constexpr auto build_type = "Release";
#else
	static constexpr auto build_type = "Unknown";
#endif

	const auto now       = Common::Timer::QueryPerformanceCounter();
	const auto frequency = Common::Timer::QueryPerformanceFrequency();
	if (title_update != 0 && now - title_update < frequency / 10) {
		return;
	}
	title_update = now;
	const auto frame_num = presented_frames.load(std::memory_order_relaxed);
	const double elapsed_seconds =
	    static_cast<double>(now - elapsed_start) / static_cast<double>(frequency);
	if (now - fps_start >= frequency) {
		current_fps = static_cast<double>(frame_num - fps_frames) * static_cast<double>(frequency) /
		              static_cast<double>(now - fps_start);
		fps_start  = now;
		fps_frames = frame_num;
	}

	auto fps = fmt::format(
	    "elapsed: {:.1f}s | shaders: {} | [{} | {}] {}{}{}{}{}{}[{}] [{}], frame: {}, fps: {:f}",
	    elapsed_seconds, ShaderGetCompileCount(), KYTY_BUILD_LABEL, build_type,
	    (has_title ? title : ""), (has_title ? ", " : ""), (has_title_id ? title_id : ""),
	    (has_title_id ? ", " : ""), (has_app_ver ? app_ver : ""), (has_app_ver ? " " : ""),
	    device_name, processor_name, frame_num, current_fps);

	if (window == nullptr) {
		return;
	}

#if defined(__IPHONEOS__)
	if (guest_visible.load(std::memory_order_acquire)) {
		LoadingStatusFinish(window, has_title_id ? title_id : "unknown");
	} else {
		LoadingStatusUpdate(window, has_title ? title : "PS5 game",
		                    has_title_id ? title_id : "unknown", has_artwork ? artwork_path : "");
	}
#endif

	SDL_SetWindowTitle(window, fps.c_str());
}

} // namespace Libs::Graphics
