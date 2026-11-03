/* Internal definitions shared by the core modules. */
#ifndef NESCORE_CORE_H
#define NESCORE_CORE_H

#include "nes.h"
#include <string.h>

#if defined(__GNUC__)
#define LIKELY(x) __builtin_expect(!!(x), 1)
#define UNLIKELY(x) __builtin_expect(!!(x), 0)
#define INLINE static inline __attribute__((always_inline))
#define NOINLINE __attribute__((noinline))
#else
#define LIKELY(x) (x)
#define UNLIKELY(x) (x)
#define INLINE static inline
#define NOINLINE
#endif

/* NTSC timing in master clocks (21.477272 MHz). */
#define CPU_DIV 12
#define PPU_DIV 4
#define PPU_OFFSET 1
#define CPU_HZ 1789773.0

enum { IRQ_FRAME = 1, IRQ_DMC = 2, IRQ_MAPPER = 4 };
enum { FC = 0x01, FZ = 0x02, FI = 0x04, FD = 0x08, FB = 0x10, FU = 0x20, FV = 0x40, FN = 0x80 };

/*
 * Everything that makes up the machine state lives in state_t, which holds
 * no pointers: a savestate is a plain copy of it. That keeps run-ahead
 * cheap (one memcpy) on the Wii.
 */
typedef struct {
    uint16_t pc;
    uint8_t a, x, y, s, p;
    uint64_t cycles;      /* CPU cycles since power */
    uint8_t phase;        /* master clocks into the current cycle at the bus access (5 read, 7 write) */
    uint32_t sync_at;     /* low 32 bits of the cycle at whose end PPU/APU must be caught up */
    uint8_t irq;          /* active IRQ sources */
    uint8_t nmi_line, prev_nmi_line, need_nmi, prev_need_nmi;
    uint8_t run_irq, prev_run_irq;
    uint8_t need_halt, need_dummy, dmc_running, spr_running, spr_page;
    uint8_t jam;
} cpu_t;

typedef struct {
    int16_t scanline;
    uint16_t dot;
    uint64_t frame;
    uint64_t master;
    uint64_t dots;        /* PPU dots since power (mapper A12 filters) */
    uint16_t v, t;
    uint8_t x, w;
    uint8_t ctrl, mask, status;
    uint8_t oamaddr;
    uint8_t oam[256];
    uint8_t oam2[32];
    uint8_t pal[32];
    uint8_t buf;          /* $2007 read buffer */
    uint8_t latch;        /* I/O bus latch (open bus) */
    uint32_t latch_frame[8];
    uint8_t prevent_vbl, render_en, warmup, frame_done, skip_dot;
    /* background pipeline */
    uint8_t nt, at, plo, phi;
    uint16_t shl, shh, sal, sah;
    /* sprite evaluation */
    uint8_t ev_buf, ev_h, ev_l, ev_o2, ev_inrange, ev_done, ev_s0, ev_bug, ev_cnt;
    /* sprites for the line being drawn */
    uint8_t sp_cnt, sp_s0;
    uint8_t sp_x[8], sp_a[8], sp_lo[8], sp_hi[8];
    uint8_t sp_line[256];
    uint16_t ev_pos;
    uint16_t cpal[32];    /* palette with greyscale/emphasis applied */
    uint8_t cpal_dirty;
    uint64_t ev_master;   /* next PPU event the CPU must not run past */
    uint16_t vdelay_val;
    uint8_t vdelay;
} ppu_t;

typedef struct {
    uint8_t len, halt, new_halt, reload, prev;
    uint8_t enabled;
} lenc_t;

typedef struct {
    uint8_t start, div, decay, loop, constant, vol;
} env_t;

typedef struct {
    lenc_t lc;
    env_t env;
    uint8_t duty, pos;
    uint16_t period, timer;
    uint8_t sw_en, sw_period, sw_div, sw_neg, sw_shift, sw_reload;
    uint8_t out;
} pulse_t;

typedef struct {
    lenc_t lc;
    uint16_t period, timer;
    uint8_t pos, lin, lin_reload, lin_flag, ctrl;
    uint8_t out;
} tri_t;

typedef struct {
    lenc_t lc;
    env_t env;
    uint8_t mode;
    uint16_t period, timer, lfsr;
    uint8_t out;
} noise_t;

typedef struct {
    uint8_t irq_en, loop, rate;
    uint16_t timer, period;
    uint8_t level;
    uint16_t addr, len, cur, remain;
    uint8_t buf, buf_empty, shift, bits, silence;
    uint8_t start_delay, dma_pending;
} dmc_t;

typedef struct {
    pulse_t p[2];
    tri_t t;
    noise_t n;
    dmc_t d;
    uint8_t odd;
    /* frame counter */
    uint8_t fc_mode, fc_step, fc_inhibit, fc_block, fc_delay;
    int16_t fc_new;
    uint32_t fc_count;
    /* mixer */
    int32_t amp;
    uint8_t dirty, lc_pending;
    uint64_t cycle;       /* next CPU cycle the APU will process */
    uint64_t ev_cycle;    /* CPU must sync by this cycle (IRQ / DMA) */
} apu_t;

typedef struct {
    uint32_t prg_off[4];  /* 8 KB banks at $8000/$A000/$C000/$E000 */
    uint32_t chr_off[8];  /* 1 KB banks */
    uint8_t nt_map[4];
    uint8_t ram_enable, ram_write;
    uint8_t r[16];        /* mapper registers */
    uint8_t shift, shift_count;
    uint8_t irq_counter, irq_latch, irq_enable, irq_reload;
    uint8_t a12;
    uint8_t fine;         /* mapper IRQ armed: keep the PPU in lockstep */
    uint64_t safe_until;  /* PPU dot until which the mapper IRQ cannot fire */
    uint64_t a12_low_at;
    uint64_t last_write;
} mapper_t;

typedef struct {
    uint32_t magic;
    cpu_t cpu;
    ppu_t ppu;
    apu_t apu;
    mapper_t m;
    uint8_t ram[2048];
    uint8_t vram[4096];
    uint8_t prg_ram[8192];
    uint8_t chr_ram[8192];
    uint8_t open_bus;
    uint8_t strobe;
    uint8_t pad_shift[2];
} state_t;

#define AUDIO_RING 8192

struct nes {
    state_t s;

    /* cartridge (not part of the state) */
    uint8_t *rom;
    const uint8_t *prg;
    uint32_t prg_size;
    uint8_t *chr;
    uint32_t chr_size;
    int chr_is_ram;
    int mapper, submapper;
    int battery, four_screen, vertical;
    void (*map_write)(nes_t *n, uint16_t a, uint8_t v);
    void (*map_bus)(nes_t *n, uint16_t a);
    void (*map_reset)(nes_t *n);

    uint8_t buttons[2];
    uint16_t fb[NES_WIDTH * NES_HEIGHT];
    int video_on, audio_on;

    /* audio output */
    double rate;
    uint32_t rate_phase, rate_step;
    int32_t acc; /* fits: <= ~40 cycles x 40000 per output sample */
    int32_t acc_n;
    float hp1, hp1_prev, hp2, hp2_prev, lp;
    float k_hp1, k_hp2, k_lp;
    int16_t ring[AUDIO_RING];
    int ring_r, ring_w;
};

/* cpu.c */
void cpu_power(nes_t *n);
void cpu_reset(nes_t *n);
void cpu_step(nes_t *n);
void cpu_set_irq(nes_t *n, uint8_t src);
void cpu_clear_irq(nes_t *n, uint8_t src);

/* ppu.c */
void ppu_power(nes_t *n);
void ppu_reset(nes_t *n);
void ppu_run(nes_t *n, uint64_t target);
uint8_t ppu_read_reg(nes_t *n, uint16_t a);
void ppu_write_reg(nes_t *n, uint16_t a, uint8_t v);

/* apu.c */
void apu_power(nes_t *n);
void apu_reset(nes_t *n);
void apu_sync(nes_t *n);
uint8_t apu_read_status(nes_t *n);
void apu_write(nes_t *n, uint16_t a, uint8_t v);
void apu_dmc_dma_done(nes_t *n, uint8_t v);
uint16_t apu_dmc_addr(nes_t *n);
void apu_init_output(nes_t *n);

/* cart.c */
int cart_load(nes_t *n, const uint8_t *data, size_t size);
void cart_power(nes_t *n);
void cart_reset(nes_t *n);

/* nes.c */
uint8_t bus_read(nes_t *n, uint16_t a);
void bus_write(nes_t *n, uint16_t a, uint8_t v);

/* The master clock is implicit: a CPU cycle N spans master clocks
 * 12N .. 12N+12, with the bus access 5 (read) or 7 (write) clocks in. */
INLINE void ppu_sync(nes_t *n)
{
    ppu_run(n, n->s.cpu.cycles * CPU_DIV + n->s.cpu.phase - PPU_OFFSET);
}

void cpu_resched(nes_t *n);

INLINE uint8_t prg_read(nes_t *n, uint16_t a)
{
    return n->prg[n->s.m.prg_off[(a >> 13) & 3] + (a & 0x1FFF)];
}

INLINE uint8_t chr_read(nes_t *n, uint16_t a)
{
    return n->chr[n->s.m.chr_off[(a >> 10) & 7] + (a & 0x3FF)];
}

INLINE uint8_t *nt_ptr(nes_t *n, uint16_t a)
{
    return &n->s.vram[((uint32_t)n->s.m.nt_map[(a >> 10) & 3] << 10) | (a & 0x3FF)];
}

#endif
