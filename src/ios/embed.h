// SPDX-FileCopyrightText: Copyright 2026 BaconMakin
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef EMULATOR_SRC_IOS_EMBED_H_
#define EMULATOR_SRC_IOS_EMBED_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum MagnusState {
	MAGNUS_STOPPED = 0,
	MAGNUS_LOADING = 1,
	MAGNUS_RUNNING = 2,
};

enum MagnusPadButton {
	MAGNUS_PAD_UP = 0,
	MAGNUS_PAD_DOWN,
	MAGNUS_PAD_LEFT,
	MAGNUS_PAD_RIGHT,
	MAGNUS_PAD_CROSS,
	MAGNUS_PAD_CIRCLE,
	MAGNUS_PAD_SQUARE,
	MAGNUS_PAD_TRIANGLE,
	MAGNUS_PAD_L1,
	MAGNUS_PAD_R1,
	MAGNUS_PAD_L2,
	MAGNUS_PAD_R2,
	MAGNUS_PAD_OPTIONS,
	MAGNUS_PAD_SHARE,
	MAGNUS_PAD_L3,
	MAGNUS_PAD_R3,
	MAGNUS_PAD_TOUCH_PAD,
};

enum MagnusPadAxis {
	MAGNUS_AXIS_LEFT_X = 0,
	MAGNUS_AXIS_LEFT_Y,
	MAGNUS_AXIS_RIGHT_X,
	MAGNUS_AXIS_RIGHT_Y,
};

struct MagnusStats {
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

void magnus_stats(struct MagnusStats* out);

void magnus_set_shader_cache_dir(const char* path);

void magnus_set_network_enabled(bool enabled);

void magnus_set_surface(void* metal_layer, uint32_t width, uint32_t height);
void magnus_set_paused(bool paused);

void magnus_set_metal_fx(bool spatial, bool temporal, bool frame_interpolation);
void magnus_set_vblank_frequency(uint32_t hz);

void magnus_set_screen_size(int mode);
void magnus_set_volume(int percent);

bool magnus_boot_game(const char* path);

int      magnus_state(void);
uint64_t magnus_guest_frames(void);

const char* magnus_boot_failure(void);

int  magnus_pad_port_count(void);
void magnus_pad_connected(int port, bool connected);
void magnus_pad_button(int port, int button, bool down);
void magnus_pad_axis(int port, int axis, int value);
void magnus_pad_touch(int port, bool down, float x, float y);

void magnus_mic_push(const int16_t* frames, uint32_t count);
void magnus_mic_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* EMULATOR_SRC_IOS_EMBED_H_ */
