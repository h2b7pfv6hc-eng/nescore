/*
 * nescore - a cycle-accurate NES emulation core, written for slow
 * big-endian targets such as the Nintendo Wii (PowerPC 750CL).
 *
 * Public API. The core is frontend-agnostic: it produces a 256x240
 * buffer of 9-bit palette indices and 16-bit mono PCM; the frontend
 * converts colours and handles video, audio and input itself.
 */
#ifndef NESCORE_H
#define NESCORE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct nes nes_t;

enum {
    NES_A = 0x01, NES_B = 0x02, NES_SELECT = 0x04, NES_START = 0x08,
    NES_UP = 0x10, NES_DOWN = 0x20, NES_LEFT = 0x40, NES_RIGHT = 0x80
};

enum {
    NES_OK = 0,
    NES_ERR_FORMAT = -1,      /* not an iNES / NES 2.0 file */
    NES_ERR_MAPPER = -2,      /* mapper not supported */
    NES_ERR_MEMORY = -3,
    NES_ERR_STATE = -4        /* savestate does not match this ROM/version */
};

#define NES_WIDTH 256
#define NES_HEIGHT 240

/* Create an emulator instance producing audio at the given sample rate. */
nes_t *nes_create(int sample_rate);
void nes_destroy(nes_t *n);

/* Load an iNES/NES 2.0 image (the data is copied). Powers the console on. */
int nes_load_rom(nes_t *n, const uint8_t *data, size_t size);
int nes_mapper_number(const nes_t *n);
const char *nes_error_string(int err);

void nes_power(nes_t *n);
void nes_reset(nes_t *n);

/* Run until the PPU has produced one full frame. */
void nes_run_frame(nes_t *n);

/*
 * Run one frame with N frames of run-ahead (0 = normal). Run-ahead
 * removes the game's own internal input lag: the frame you see has been
 * simulated N frames into the future with the current input.
 * Costs (N+1)x the emulation time of a normal frame.
 */
void nes_run_frame_runahead(nes_t *n, int frames);

/* 256x240 entries: bits 0-5 NES colour, bits 6-8 colour emphasis. */
const uint16_t *nes_framebuffer(const nes_t *n);

/* Fill rgb[512] with 0x00RRGGBB for every (colour | emphasis) index. */
void nes_palette_rgb(uint32_t rgb[512]);

/* Pull up to max mono samples; returns the number written. */
int nes_audio_read(nes_t *n, int16_t *out, int max);
int nes_audio_available(const nes_t *n);
/* Fine-tune the audio rate for dynamic rate control (e.g. 48000.0f * 1.002f). */
void nes_set_audio_rate(nes_t *n, double rate);

void nes_set_buttons(nes_t *n, int port, uint8_t buttons);

/* Battery-backed PRG RAM (8 KB) or NULL when the cartridge has none. */
uint8_t *nes_battery_ram(nes_t *n, size_t *size);

size_t nes_state_size(const nes_t *n);
int nes_save_state(const nes_t *n, void *buf, size_t size);
int nes_load_state(nes_t *n, const void *buf, size_t size);

/* Debug helpers used by the test tools. */
uint8_t nes_peek(nes_t *n, uint16_t addr);
uint64_t nes_cpu_cycles(const nes_t *n);
uint64_t nes_frame_count(const nes_t *n);

#ifdef __cplusplus
}
#endif
#endif
