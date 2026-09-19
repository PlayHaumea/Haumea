// SPDX-FileCopyrightText: Copyright 2026 BaconMakin
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef EMULATOR_SRC_IOS_EMBED_H_
#define EMULATOR_SRC_IOS_EMBED_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum HaumeaState {
	HAUMEA_STOPPED = 0,
	HAUMEA_LOADING = 1,
	HAUMEA_RUNNING = 2,
};

enum HaumeaPadButton {
	HAUMEA_PAD_UP = 0,
	HAUMEA_PAD_DOWN,
	HAUMEA_PAD_LEFT,
	HAUMEA_PAD_RIGHT,
	HAUMEA_PAD_CROSS,
	HAUMEA_PAD_CIRCLE,
	HAUMEA_PAD_SQUARE,
	HAUMEA_PAD_TRIANGLE,
	HAUMEA_PAD_L1,
	HAUMEA_PAD_R1,
	HAUMEA_PAD_L2,
	HAUMEA_PAD_R2,
	HAUMEA_PAD_OPTIONS,
	HAUMEA_PAD_SHARE,
	HAUMEA_PAD_L3,
	HAUMEA_PAD_R3,
	HAUMEA_PAD_TOUCH_PAD,
};

enum HaumeaPadAxis {
	HAUMEA_AXIS_LEFT_X = 0,
	HAUMEA_AXIS_LEFT_Y,
	HAUMEA_AXIS_RIGHT_X,
	HAUMEA_AXIS_RIGHT_Y,
};

struct HaumeaStats {
	uint64_t guest_frames;
	uint64_t unaligned;
	uint64_t segv;
	uint64_t bus;
	uint64_t compiled_blocks;
	uint64_t frontend_compile_us;
	uint64_t backend_compile_us;
	uint64_t shader_compile_us;
	uint64_t pipelines;
	uint64_t pipeline_create_us;
};

void haumea_stats(struct HaumeaStats* out);

void haumea_set_shader_cache_dir(const char* path);

void haumea_set_network_enabled(bool enabled);

void haumea_set_surface(void* metal_layer, uint32_t width, uint32_t height);
void haumea_set_paused(bool paused);

void haumea_set_metal_fx(bool spatial, bool temporal, bool frame_interpolation);
void haumea_set_vblank_frequency(uint32_t hz);

void haumea_set_screen_size(int mode);
void haumea_set_volume(int percent);

bool haumea_boot_game(const char* path);

int      haumea_state(void);
uint64_t haumea_guest_frames(void);

const char* haumea_boot_failure(void);

int  haumea_pad_port_count(void);
void haumea_pad_connected(int port, bool connected);
void haumea_pad_button(int port, int button, bool down);
void haumea_pad_axis(int port, int axis, int value);
void haumea_pad_touch(int port, bool down, float x, float y);

void haumea_mic_push(const int16_t* frames, uint32_t count);
void haumea_mic_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* EMULATOR_SRC_IOS_EMBED_H_ */
