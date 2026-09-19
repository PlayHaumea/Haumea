// SPDX-FileCopyrightText: Copyright 2026 BaconMakin
// SPDX-License-Identifier: GPL-3.0-or-later

#include "ios/embed.h"

#include "common/emulatorConfig.h"
#include "common/file.h"
#include "common/threads.h"
#include "libs/audio.h"
#include "common/virtualMemory.h"
#include "emulator.h"
#include "graphics/presentation/window.h"
#include "graphics/host_gpu/graphicContext.h"
#include "graphics/host_gpu/renderer/pipeline/pipelineCache.h"
#include "graphics/shader/shader.h"
#include "libs/controller.h"
#include "libs/network.h"

#include <csignal>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <thread>

namespace Haumea {
bool InstallStinger(const char* title_path);
bool ReserveStingerRuntime();
const char* StingerRuntimeFailure();
uint64_t UnalignedAccessCount();
uint64_t GuestFaultCount(uint32_t signal_number);
}

extern "C" uint64_t FEXCompiledBlockCount();
extern "C" uint64_t FEXFrontendCompileMicroseconds();
extern "C" uint64_t FEXBackendCompileMicroseconds();

namespace {

std::atomic<int> g_state {HAUMEA_STOPPED};
std::atomic<const char*> g_boot_failure {nullptr};
std::atomic_bool g_booted_once {false};

uint32_t PadBit(int button) {
	using namespace Libs::Controller;
	switch (button) {
		case HAUMEA_PAD_UP: return PAD_BUTTON_UP;
		case HAUMEA_PAD_DOWN: return PAD_BUTTON_DOWN;
		case HAUMEA_PAD_LEFT: return PAD_BUTTON_LEFT;
		case HAUMEA_PAD_RIGHT: return PAD_BUTTON_RIGHT;
		case HAUMEA_PAD_CROSS: return PAD_BUTTON_CROSS;
		case HAUMEA_PAD_CIRCLE: return PAD_BUTTON_CIRCLE;
		case HAUMEA_PAD_SQUARE: return PAD_BUTTON_SQUARE;
		case HAUMEA_PAD_TRIANGLE: return PAD_BUTTON_TRIANGLE;
		case HAUMEA_PAD_L1: return PAD_BUTTON_L1;
		case HAUMEA_PAD_R1: return PAD_BUTTON_R1;
		case HAUMEA_PAD_L2: return PAD_BUTTON_L2;
		case HAUMEA_PAD_R2: return PAD_BUTTON_R2;
		case HAUMEA_PAD_OPTIONS: return PAD_BUTTON_OPTIONS;
		case HAUMEA_PAD_SHARE: return PAD_BUTTON_SHARE;
		case HAUMEA_PAD_L3: return PAD_BUTTON_L3;
		case HAUMEA_PAD_R3: return PAD_BUTTON_R3;
		case HAUMEA_PAD_TOUCH_PAD: return PAD_BUTTON_TOUCH_PAD;
		default: return 0;
	}
}

bool SetDataDirectory() {
	const char* home = std::getenv("HOME");
	if (home == nullptr) {
		return false;
	}

	const auto data =
	    std::filesystem::path(home) / "Library" / "Application Support" / "Haumea";
	Common::File::CreateDirectories(data);
	std::error_code error;
	std::filesystem::current_path(data, error);
	return !error;
}

void BootThread(std::filesystem::path app0) {
	if (!Haumea::ReserveStingerRuntime()) {
		const char* reason = Haumea::StingerRuntimeFailure();
		g_boot_failure = reason != nullptr ? reason : "the recompiler runtime is not available";
		::printf("Haumea:Stinger:Error: runtime reservation failed, refusing to boot: %s\n",
		         g_boot_failure.load());
		g_booted_once = false;
		g_state = HAUMEA_STOPPED;
		return;
	}

	Common::VirtualMemory::Init();
	Common::InitializeThreads();

	if (!Haumea::InstallStinger(app0.string().c_str())) {
		g_boot_failure = "the recompiler could not load this game";
		::printf("Haumea:Stinger:Error: install failed, refusing to boot\n");
		g_state = HAUMEA_STOPPED;
		return;
	}

	Emulator::RunOptions options;
	options.config.printf_direction = ::getenv("HAUMEA_VERBOSE") != nullptr
	                                      ? Config::OutputDirection::Console
	                                      : Config::OutputDirection::Silent;

	uint32_t width  = 0;
	uint32_t height = 0;
	Libs::Graphics::ExternalSurfaceSize(&width, &height);
	if (width > 0 && height > 0) {
		options.config.screen_width  = width;
		options.config.screen_height = height;
	}

	options.app0_dir = app0;
	options.elf      = "/app0/eboot.bin";

	g_state = HAUMEA_RUNNING;
	Emulator::Run(options);
	g_boot_failure = "the game stopped running";
	::printf("Haumea:Boot:Info: emulator returned, game stopped\n");
	g_state = HAUMEA_STOPPED;
}

}

void haumea_set_surface(void* metal_layer, uint32_t width, uint32_t height) {
	Libs::Graphics::SetExternalSurface(metal_layer, width, height);
}

void haumea_set_paused(bool paused) {
	Libs::Graphics::SetAppPaused(paused);
}

void haumea_set_vblank_frequency(uint32_t hz) {
	Config::SetVblankFrequency(hz);
}

void haumea_set_volume(int percent) {
	Libs::Audio::SetMasterVolume(percent);
}

bool haumea_boot_game(const char* path) {
	if (path == nullptr) {
		return false;
	}

	bool already = false;
	if (!g_booted_once.compare_exchange_strong(already, true)) {
		g_boot_failure = "Haumea runs one game per launch. Close Haumea and open it again to "
		                 "play another.";
		::printf("Haumea:Boot:Error: refused, this process already booted a game\n");
		return false;
	}

	int expected = HAUMEA_STOPPED;
	if (!g_state.compare_exchange_strong(expected, HAUMEA_LOADING)) {
		::printf("Haumea:Boot:Error: refused, a game is already loaded state=%d\n", expected);
		return false;
	}
	g_boot_failure = nullptr;

	std::filesystem::path app0(path);
	if (app0.filename() == "eboot.bin") {
		app0 = app0.parent_path();
	}

	if (!Common::File::IsFileExisting(app0 / "eboot.bin")) {
		g_boot_failure = "this game folder has no eboot.bin";
		::printf("Haumea:Boot:Error: no eboot file=eboot.bin\n");
		g_state = HAUMEA_STOPPED;
		return false;
	}
	if (!SetDataDirectory()) {
		g_boot_failure = "the emulator could not open its data folder";
		::printf("Haumea:Boot:Error: data directory unavailable\n");
		g_state = HAUMEA_STOPPED;
		return false;
	}

	std::thread(BootThread, app0).detach();
	return true;
}

int haumea_state() {
	return g_state.load();
}

const char* haumea_boot_failure() {
	return g_boot_failure.load();
}

uint64_t haumea_guest_frames() {
	return Libs::Graphics::GuestFrameCount();
}

void haumea_set_shader_cache_dir(const char* path) {
	Libs::Graphics::SetPipelineCacheFolder(path);
}

void haumea_set_screen_size(int mode) {
	Config::SetScreenSize(mode);
}

void haumea_set_network_enabled(bool enabled) {
	Libs::Network::NetCtl::SetNetworkEnabled(enabled);
}

void haumea_stats(struct HaumeaStats* out) {
	if (out == nullptr) {
		return;
	}
	out->guest_frames        = Libs::Graphics::GuestFrameCount();
	out->unaligned           = Haumea::UnalignedAccessCount();
	out->segv                = Haumea::GuestFaultCount(SIGSEGV);
	out->bus                 = Haumea::GuestFaultCount(SIGBUS);
	out->compiled_blocks     = FEXCompiledBlockCount();
	out->frontend_compile_us = FEXFrontendCompileMicroseconds();
	out->backend_compile_us  = FEXBackendCompileMicroseconds();
	out->shader_compile_us  = Libs::Graphics::ShaderGetCompileMicroseconds();
	out->pipelines          = Libs::Graphics::PipelineGetCreateCount();
	out->pipeline_create_us = Libs::Graphics::PipelineGetCreateMicroseconds();
}

int haumea_pad_port_count() {
	return Libs::Controller::PAD_PORT_MAX;
}

void haumea_pad_connected(int port, bool connected) {
	using namespace Libs::Controller;
	if (g_state.load() == HAUMEA_STOPPED || port < 0 || port >= PAD_PORT_MAX) {
		return;
	}

	ControllerSetPortConnected(port, connected);
}

void haumea_pad_button(int port, int button, bool down) {
	using namespace Libs::Controller;
	if (g_state.load() == HAUMEA_STOPPED || port < 0 || port >= PAD_PORT_MAX) {
		return;
	}

	const uint32_t bit = PadBit(button);
	if (bit == PAD_BUTTON_L2) {
		ControllerAxis(port, HOST_INPUT_CONTROLLER_ID, Axis::TriggerLeft, down ? 255 : 0);
	} else if (bit == PAD_BUTTON_R2) {
		ControllerAxis(port, HOST_INPUT_CONTROLLER_ID, Axis::TriggerRight, down ? 255 : 0);
	} else if (bit != 0) {
		ControllerButton(port, HOST_INPUT_CONTROLLER_ID, bit, down);
	}
}

void haumea_pad_touch(int port, bool down, float x, float y) {
	using namespace Libs::Controller;
	if (g_state.load() == HAUMEA_STOPPED || port < 0 || port >= PAD_PORT_MAX) {
		return;
	}

	constexpr float touch_width  = 1920.0f;
	constexpr float touch_height = 943.0f;
	const float     clamped_x    = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
	const float     clamped_y    = y < 0.0f ? 0.0f : (y > 1.0f ? 1.0f : y);

	ControllerTouch(port, HOST_INPUT_CONTROLLER_ID, down,
	                static_cast<uint16_t>(clamped_x * touch_width),
	                static_cast<uint16_t>(clamped_y * touch_height));
}

void haumea_pad_axis(int port, int axis, int value) {
	using namespace Libs::Controller;
	if (g_state.load() == HAUMEA_STOPPED || port < 0 || port >= PAD_PORT_MAX || axis < 0 ||
	    axis > HAUMEA_AXIS_RIGHT_Y) {
		return;
	}

	ControllerAxis(port, HOST_INPUT_CONTROLLER_ID, static_cast<Axis>(axis),
	               value < 0 ? 0 : (value > 255 ? 255 : value));
}

void haumea_mic_push(const int16_t* frames, uint32_t count) {
	if (g_state.load() == HAUMEA_STOPPED) {
		return;
	}
	Libs::Audio::AudioIn::CapturePush(frames, count);
}

void haumea_mic_stop(void) {
	Libs::Audio::AudioIn::CaptureReset();
}
