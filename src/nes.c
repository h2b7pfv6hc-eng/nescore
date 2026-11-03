/* CPU memory map, frame loop, input, savestates and the public API. */
#include "core.h"
#include <math.h>
#include <stdlib.h>

#define STATE_MAGIC 0x4E534331u /* "NSC1" */

uint8_t bus_read(nes_t *n, uint16_t a)
{
    uint8_t v;
    if (a >= 0x8000) {
        v = prg_read(n, a); /* most reads are code from ROM */
    } else if (a < 0x2000) {
        v = n->s.ram[a & 0x7FF];
    } else if (a < 0x4000) {
        ppu_sync(n);
        v = ppu_read_reg(n, a);
    } else if (a < 0x4020) {
        if (a == 0x4015) {
            /* internal register: does not drive the external data bus */
            return (uint8_t)(apu_read_status(n) | (n->s.open_bus & 0x20));
        } else if (a == 0x4016 || a == 0x4017) {
            int port = a & 1;
            uint8_t bit;
            if (n->s.strobe) {
                bit = n->buttons[port] & 1;
            } else {
                bit = n->s.pad_shift[port] & 1;
                n->s.pad_shift[port] = (uint8_t)((n->s.pad_shift[port] >> 1) | 0x80);
            }
            v = (uint8_t)((n->s.open_bus & 0xE0) | bit);
        } else {
            v = n->s.open_bus;
        }
    } else if (a < 0x6000) {
        v = n->s.open_bus;
    } else if (a < 0x8000) {
        v = n->s.m.ram_enable ? n->s.prg_ram[a & 0x1FFF] : n->s.open_bus;
    } else {
        v = prg_read(n, a);
    }
    n->s.open_bus = v;
    return v;
}

void bus_write(nes_t *n, uint16_t a, uint8_t v)
{
    n->s.open_bus = v;
    if (a < 0x2000) {
        n->s.ram[a & 0x7FF] = v;
    } else if (a < 0x4000) {
        ppu_sync(n);
        ppu_write_reg(n, a, v);
    } else if (a < 0x4020) {
        if (a == 0x4014) {
            n->s.cpu.spr_page = v;
            n->s.cpu.spr_running = 1;
            n->s.cpu.need_halt = 1;
        } else if (a == 0x4016) {
            n->s.strobe = v & 1;
            if (n->s.strobe) {
                n->s.pad_shift[0] = n->buttons[0];
                n->s.pad_shift[1] = n->buttons[1];
            }
        } else {
            apu_write(n, a, v);
        }
    } else {
        if (a >= 0x6000 && a < 0x8000 && n->s.m.ram_enable && n->s.m.ram_write)
            n->s.prg_ram[a & 0x1FFF] = v;
        if (n->map_write) {
            if (a >= 0x8000)
                ppu_sync(n); /* bank/mirroring changes affect rendering */
            n->map_write(n, a, v);
        }
    }
}

nes_t *nes_create(int sample_rate)
{
    nes_t *n = (nes_t *)calloc(1, sizeof(nes_t));
    if (!n)
        return NULL;
    n->rate = sample_rate > 0 ? sample_rate : 48000;
    n->video_on = 1;
    n->audio_on = 1;
    apu_init_output(n);
    return n;
}

void nes_destroy(nes_t *n)
{
    if (!n) return;
    free(n->rom);
    free(n);
}

const char *nes_error_string(int err)
{
    switch (err) {
    case NES_OK: return "ok";
    case NES_ERR_FORMAT: return "not a valid NES ROM";
    case NES_ERR_MAPPER: return "mapper not supported";
    case NES_ERR_MEMORY: return "out of memory";
    case NES_ERR_STATE: return "savestate does not match";
    }
    return "unknown error";
}

int nes_load_rom(nes_t *n, const uint8_t *data, size_t size)
{
    int r = cart_load(n, data, size);
    if (r != NES_OK)
        return r;
    nes_power(n);
    return NES_OK;
}

int nes_mapper_number(const nes_t *n) { return n->mapper; }

void nes_power(nes_t *n)
{
    n->s.magic = STATE_MAGIC;
    memset(n->s.ram, 0, sizeof(n->s.ram));
    memset(n->s.vram, 0, sizeof(n->s.vram));
    if (n->chr_is_ram)
        memset(n->s.chr_ram, 0, sizeof(n->s.chr_ram));
    n->s.open_bus = 0;
    n->s.strobe = 0;
    cart_power(n);
    ppu_power(n);
    apu_power(n);
    cpu_power(n);
}

void nes_reset(nes_t *n)
{
    cart_reset(n);
    ppu_reset(n);
    apu_reset(n);
    cpu_reset(n);
}

void nes_run_frame(nes_t *n)
{
    n->s.ppu.frame_done = 0;
    while (!n->s.ppu.frame_done)
        cpu_step(n);
    apu_sync(n); /* produce this frame's audio */
}

void nes_run_frame_runahead(nes_t *n, int frames)
{
    if (frames <= 0) {
        nes_run_frame(n);
        return;
    }
    static state_t saved; /* run-ahead scratch; one instance per process */
    n->video_on = 0;
    nes_run_frame(n);            /* the real timeline, with sound */
    memcpy(&saved, &n->s, sizeof(state_t));
    n->audio_on = 0;
    n->video_on = 1;
    for (int i = 0; i < frames; i++)
        nes_run_frame(n);        /* speculative frames, shown but silent */
    memcpy(&n->s, &saved, sizeof(state_t));
    n->audio_on = 1;
}

const uint16_t *nes_framebuffer(const nes_t *n) { return n->fb; }

void nes_set_buttons(nes_t *n, int port, uint8_t buttons)
{
    /* the D-pad cannot press opposite directions on real hardware */
    if ((buttons & (NES_UP | NES_DOWN)) == (NES_UP | NES_DOWN))
        buttons &= (uint8_t)~(NES_UP | NES_DOWN);
    if ((buttons & (NES_LEFT | NES_RIGHT)) == (NES_LEFT | NES_RIGHT))
        buttons &= (uint8_t)~(NES_LEFT | NES_RIGHT);
    n->buttons[port & 1] = buttons;
}

int nes_audio_available(const nes_t *n)
{
    return (n->ring_w - n->ring_r) & (AUDIO_RING - 1);
}

int nes_audio_read(nes_t *n, int16_t *out, int max)
{
    int c = 0;
    while (c < max && n->ring_r != n->ring_w) {
        out[c++] = n->ring[n->ring_r];
        n->ring_r = (n->ring_r + 1) & (AUDIO_RING - 1);
    }
    return c;
}

void nes_set_audio_rate(nes_t *n, double rate)
{
    n->rate = rate;
    n->rate_step = (uint32_t)(rate / CPU_HZ * 4294967296.0);
}

uint8_t *nes_battery_ram(nes_t *n, size_t *size)
{
    if (!n->battery)
        return NULL;
    if (size)
        *size = sizeof(n->s.prg_ram);
    return n->s.prg_ram;
}

size_t nes_state_size(const nes_t *n)
{
    (void)n;
    return sizeof(state_t);
}

int nes_save_state(const nes_t *n, void *buf, size_t size)
{
    if (size < sizeof(state_t))
        return NES_ERR_STATE;
    memcpy(buf, &n->s, sizeof(state_t));
    return NES_OK;
}

int nes_load_state(nes_t *n, const void *buf, size_t size)
{
    if (size < sizeof(state_t) || ((const state_t *)buf)->magic != STATE_MAGIC)
        return NES_ERR_STATE;
    memcpy(&n->s, buf, sizeof(state_t));
    return NES_OK;
}

uint8_t nes_peek(nes_t *n, uint16_t a)
{
    if (a < 0x2000) return n->s.ram[a & 0x7FF];
    if (a >= 0x6000 && a < 0x8000) return n->s.prg_ram[a & 0x1FFF];
    if (a >= 0x8000) return prg_read(n, a);
    return 0;
}

uint64_t nes_cpu_cycles(const nes_t *n) { return n->s.cpu.cycles; }
uint64_t nes_frame_count(const nes_t *n) { return n->s.ppu.frame; }

/*
 * Palette: generated from a model of the 2C02 composite video signal
 * (square wave between two voltage levels per hue), decoded as YUV.
 * The colour-emphasis bits attenuate the signal during their phases.
 */
void nes_palette_rgb(uint32_t rgb[512])
{
    static const float lo[4] = {0.228f, 0.312f, 0.552f, 0.880f};
    static const float hi[4] = {0.616f, 0.840f, 1.100f, 1.100f};
    const float black = 0.312f, white = 1.100f;
    for (int i = 0; i < 512; i++) {
        int color = i & 0x0F, level = (i >> 4) & 3, emph = i >> 6;
        float y = 0, u = 0, v = 0;
        for (int p = 0; p < 12; p++) {
            float sig;
            if (color == 0) sig = hi[level];
            else if (color == 13) sig = lo[level];
            else if (color > 13) sig = black;
            else sig = ((p - color + 30) % 12) < 6 ? hi[level] : lo[level];
            if (emph && color < 14) {
                int att = ((emph & 1) && ((p + 4) % 12) < 6) ||
                          ((emph & 2) && (p % 12) < 6) ||
                          ((emph & 4) && ((p + 8) % 12) < 6);
                if (att) sig *= 0.746f;
            }
            float s = (sig - black) / (white - black);
            float ang = 3.14159265f * (p + 0.5f) / 6.0f;
            y += s;
            u += s * cosf(ang);
            v += s * sinf(ang);
        }
        y /= 12.0f; u /= 12.0f; v /= 12.0f;
        float hue = 20.0f * 3.14159265f / 180.0f;
        float uu = u * cosf(hue) - v * sinf(hue);
        float vv = u * sinf(hue) + v * cosf(hue);
        float c[3] = {y + 1.139883f * vv, y - 0.394642f * uu - 0.580622f * vv, y + 2.032062f * uu};
        uint32_t out = 0;
        for (int k = 0; k < 3; k++) {
            float x = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
            x = powf(x, 1.0f / 1.1f);
            out = out << 8 | (uint32_t)(x * 255.0f + 0.5f);
        }
        rgb[i] = out;
    }
}
