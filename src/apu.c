/*
 * 2A03 APU: two pulse channels, triangle, noise, DMC and the frame
 * counter, clocked every CPU cycle. The mixer uses the non-linear DAC
 * curves and the output is box-filtered down to the host sample rate,
 * followed by the NES's own analog high/low-pass filters.
 */
#include "core.h"
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define APU (&n->s.apu)
#ifndef DMC_PHASE
#define DMC_PHASE 1
#endif

static const uint8_t len_table[32] = {
    10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
    12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
static const uint8_t duty_table[4][8] = {
    {0, 1, 0, 0, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0, 0, 0},
    {0, 1, 1, 1, 1, 0, 0, 0}, {1, 0, 0, 1, 1, 1, 1, 1}};
static const uint8_t tri_table[32] = {
    15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0,
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
static const uint16_t noise_table[16] = {
    4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};
static const uint16_t dmc_table[16] = {
    428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54};
static const uint32_t fc_steps[2][6] = {
    {7457, 14913, 22371, 29828, 29829, 29830},
    {7457, 14913, 22371, 29829, 37281, 37282}};
/* 1 = quarter frame, 2 = half frame */
static const uint8_t fc_types[2][6] = {{1, 2, 1, 0, 2, 0}, {1, 2, 1, 0, 2, 0}};

static void update_event(nes_t *n);

static int32_t pulse_mix[31];
static int32_t tnd_mix[203];
static int mix_ready;

static void init_mix(void)
{
    if (mix_ready) return;
    for (int i = 0; i < 31; i++)
        pulse_mix[i] = i ? (int32_t)(95.52 / (8128.0 / i + 100.0) * 40000.0) : 0;
    for (int i = 0; i < 203; i++)
        tnd_mix[i] = i ? (int32_t)(163.67 / (24329.0 / i + 100.0) * 40000.0) : 0;
    mix_ready = 1;
}

INLINE void lc_clock(lenc_t *l)
{
    if (!l->halt && l->len)
        l->len--;
}

INLINE void lc_load(lenc_t *l, uint8_t idx)
{
    if (l->enabled) {
        l->reload = len_table[idx];
        l->prev = l->len;
    }
}

INLINE void lc_apply(lenc_t *l)
{
    if (l->reload) {
        if (l->len == l->prev)
            l->len = l->reload;
        l->reload = 0;
    }
    l->halt = l->new_halt;
}

static void env_clock(env_t *e)
{
    if (e->start) {
        e->start = 0;
        e->decay = 15;
        e->div = e->vol;
    } else if (e->div == 0) {
        e->div = e->vol;
        if (e->decay)
            e->decay--;
        else if (e->loop)
            e->decay = 15;
    } else {
        e->div--;
    }
}

INLINE uint8_t env_out(env_t *e) { return e->constant ? e->vol : e->decay; }

static uint16_t sweep_target(pulse_t *p, int ch)
{
    uint16_t c = p->period >> p->sw_shift;
    if (p->sw_neg)
        return (uint16_t)(p->period - c - (ch == 0 ? 1 : 0));
    return (uint16_t)(p->period + c);
}

INLINE int sweep_mute(pulse_t *p, int ch)
{
    return p->period < 8 || (!p->sw_neg && sweep_target(p, ch) > 0x7FF);
}

static void sweep_clock(pulse_t *p, int ch)
{
    if (p->sw_div == 0 && p->sw_en && p->sw_shift && !sweep_mute(p, ch))
        p->period = sweep_target(p, ch);
    if (p->sw_div == 0 || p->sw_reload) {
        p->sw_div = p->sw_period;
        p->sw_reload = 0;
    } else {
        p->sw_div--;
    }
}

INLINE void update_out(nes_t *n)
{
    apu_t *a = APU;
    a->dirty = 1;
}

static void quarter(nes_t *n)
{
    apu_t *a = APU;
    env_clock(&a->p[0].env);
    env_clock(&a->p[1].env);
    env_clock(&a->n.env);
    if (a->t.lin_flag)
        a->t.lin = a->t.lin_reload;
    else if (a->t.lin)
        a->t.lin--;
    if (!(a->t.ctrl))
        a->t.lin_flag = 0;
    update_out(n);
}

static void half(nes_t *n)
{
    apu_t *a = APU;
    quarter(n);
    lc_clock(&a->p[0].lc);
    lc_clock(&a->p[1].lc);
    lc_clock(&a->t.lc);
    lc_clock(&a->n.lc);
    sweep_clock(&a->p[0], 0);
    sweep_clock(&a->p[1], 1);
}

static void frame_tick(nes_t *n, int type)
{
    if (type == 1) quarter(n);
    else if (type == 2) half(n);
}

static void dmc_start_dma(nes_t *n)
{
    dmc_t *d = &n->s.apu.d;
    if (d->buf_empty && d->remain && !n->s.cpu.dmc_running) {
        n->s.cpu.dmc_running = 1;
        n->s.cpu.need_halt = 1;
        n->s.cpu.need_dummy = 1;
    }
}

uint16_t apu_dmc_addr(nes_t *n) { return n->s.apu.d.cur; }

void apu_dmc_dma_done(nes_t *n, uint8_t v)
{
    dmc_t *d = &n->s.apu.d;
    apu_sync(n);
    if (!d->remain)
        return;
    d->buf = v;
    d->buf_empty = 0;
    d->cur = d->cur == 0xFFFF ? 0x8000 : (uint16_t)(d->cur + 1);
    if (--d->remain == 0) {
        if (d->loop) {
            d->cur = d->addr;
            d->remain = d->len;
        } else if (d->irq_en) {
            cpu_set_irq(n, IRQ_DMC);
        }
    }
    update_event(n);
}

static void dmc_clock(nes_t *n)
{
    dmc_t *d = &n->s.apu.d;
    if (!d->silence) {
        if (d->shift & 1) {
            if (d->level <= 125) d->level += 2;
        } else {
            if (d->level >= 2) d->level -= 2;
        }
        d->shift >>= 1;
        update_out(n);
    }
    if (--d->bits == 0) {
        d->bits = 8;
        if (d->buf_empty) {
            d->silence = 1;
        } else {
            d->silence = 0;
            d->shift = d->buf;
            d->buf_empty = 1;
            dmc_start_dma(n);
        }
    }
}

static void emit_sample(nes_t *n)
{
    float x = (float)n->acc / (float)n->acc_n;
    n->acc = 0;
    n->acc_n = 0;
    /* NES output stage: two high-pass (90 Hz, 440 Hz) and a 14 kHz low-pass */
    float y1 = n->k_hp1 * (n->hp1 + x - n->hp1_prev);
    n->hp1_prev = x;
    n->hp1 = y1;
    float y2 = n->k_hp2 * (n->hp2 + y1 - n->hp2_prev);
    n->hp2_prev = y1;
    n->hp2 = y2;
    n->lp += n->k_lp * (y2 - n->lp);
    float o = n->lp;
    if (o > 32767.f) o = 32767.f;
    if (o < -32768.f) o = -32768.f;
    int next = (n->ring_w + 1) & (AUDIO_RING - 1);
    if (next != n->ring_r) {
        n->ring[n->ring_w] = (int16_t)o;
        n->ring_w = next;
    }
}

void apu_init_output(nes_t *n)
{
    double r = n->rate;
    n->rate_step = (uint32_t)(r / CPU_HZ * 4294967296.0);
    n->k_hp1 = (float)(1.0 / (1.0 + 2 * M_PI * 90.0 / r));
    n->k_hp2 = (float)(1.0 / (1.0 + 2 * M_PI * 440.0 / r));
    double rc = 1.0 / (2 * M_PI * 14000.0);
    n->k_lp = (float)((1.0 / r) / (rc + 1.0 / r));
}

static void apu_step(nes_t *n)
{
    apu_t *a = APU;

    /* frame counter */
    a->fc_count++;
    if (a->fc_count >= fc_steps[a->fc_mode][a->fc_step]) {
        if (!a->fc_inhibit && a->fc_mode == 0 && a->fc_step >= 3)
            cpu_set_irq(n, IRQ_FRAME);
        int type = fc_types[a->fc_mode][a->fc_step];
        if (type && !a->fc_block) {
            frame_tick(n, type);
            a->fc_block = 2;
        }
        if (++a->fc_step == 6) {
            a->fc_step = 0;
            a->fc_count = 0;
        }
    }
    if (a->fc_new >= 0 && --a->fc_delay == 0) {
        a->fc_mode = (a->fc_new & 0x80) ? 1 : 0;
        a->fc_step = 0;
        a->fc_count = 0;
        a->fc_new = -1;
        if (a->fc_mode && !a->fc_block) {
            frame_tick(n, 2);
            a->fc_block = 2;
        }
    }
    if (a->fc_block)
        a->fc_block--;

    if (a->lc_pending) {
        a->lc_pending = 0;
        lc_apply(&a->p[0].lc);
        lc_apply(&a->p[1].lc);
        lc_apply(&a->t.lc);
        lc_apply(&a->n.lc);
    }

    /* triangle: clocked every CPU cycle */
    if (a->t.timer == 0) {
        a->t.timer = a->t.period;
        /* periods below 2 are ultrasonic; holding the step avoids a pop */
        if (a->t.lc.len && a->t.lin && a->t.period >= 2) {
            a->t.pos = (a->t.pos + 1) & 31;
            a->dirty = 1;
        }
    } else {
        a->t.timer--;
    }

    /* pulse + noise: every other CPU cycle */
    a->odd ^= 1;
    if (a->odd) {
        for (int i = 0; i < 2; i++) {
            pulse_t *p = &a->p[i];
            if (p->timer == 0) {
                p->timer = p->period;
                p->pos = (p->pos + 1) & 7;
                a->dirty = 1;
            } else {
                p->timer--;
            }
        }
    }
    if (a->n.timer == 0) {
        a->n.timer = (uint16_t)(a->n.period - 1);
        uint16_t fb = (uint16_t)((a->n.lfsr ^ (a->n.lfsr >> (a->n.mode ? 6 : 1))) & 1);
        a->n.lfsr = (uint16_t)((a->n.lfsr >> 1) | (fb << 14));
        a->dirty = 1;
    } else {
        a->n.timer--;
    }

    /* DMC */
    if (a->d.timer == 0) {
        a->d.timer = (uint16_t)(a->d.period - 1);
        dmc_clock(n);
    } else {
        a->d.timer--;
    }
    if (a->d.start_delay && --a->d.start_delay == 0)
        dmc_start_dma(n);

    if (!n->audio_on)
        return;

    if (a->dirty) {
        a->dirty = 0;
        uint8_t o[2];
        for (int i = 0; i < 2; i++) {
            pulse_t *p = &a->p[i];
            o[i] = (p->lc.len && !sweep_mute(p, i) && duty_table[p->duty][p->pos]) ? env_out(&p->env) : 0;
        }
        uint8_t t = tri_table[a->t.pos];
        uint8_t no = (a->n.lc.len && !(a->n.lfsr & 1)) ? env_out(&a->n.env) : 0;
        a->amp = pulse_mix[o[0] + o[1]] + tnd_mix[3 * t + 2 * no + a->d.level];
    }
    n->acc += a->amp;
    n->acc_n++;
    /* fixed-point resampler: rate_step = rate / CPU_HZ in 1/2^32 units */
    uint32_t prev = n->rate_phase;
    n->rate_phase += n->rate_step;
    if (n->rate_phase < prev)
        emit_sample(n);
}

/*
 * Lazy APU. Most CPU cycles change nothing but a few down-counters, so the
 * APU is caught up in bulk: cycles that contain no channel/frame-counter
 * event and no output sample are skipped arithmetically, and only event
 * cycles run the full per-cycle step above. The result is identical to
 * stepping every cycle.
 */
/* Advance a down-counter that reloads to `reload` after reaching zero by
 * `ticks` ticks; returns how many times it expired. */
INLINE uint32_t timer_adv(uint16_t *timer, uint32_t reload, uint32_t ticks)
{
    uint32_t t = *timer;
    if (ticks <= t) {
        *timer = (uint16_t)(t - ticks);
        return 0;
    }
    ticks -= t + 1;
    uint32_t cnt = 1, per = reload + 1;
    if (ticks >= per) {
        cnt += ticks / per;
        ticks %= per;
    }
    *timer = (uint16_t)(reload - ticks);
    return cnt;
}

INLINE int pulse_audible(nes_t *n, int i)
{
    pulse_t *p = &n->s.apu.p[i];
    return p->lc.len && !sweep_mute(p, i) && env_out(&p->env);
}

INLINE int tri_steps(apu_t *a)
{
    return a->t.lc.len && a->t.lin && a->t.period >= 2;
}

/* Cycles until the next cycle that must run the full step: a frame-counter
 * step, a DMC clock, an output sample, or an audible channel change. */
static uint32_t cycles_to_event(nes_t *n)
{
    apu_t *a = APU;
    int audio = n->audio_on;
    uint32_t j = fc_steps[a->fc_mode][a->fc_step] - a->fc_count;
    uint32_t e;
    if (a->fc_new >= 0 && a->fc_delay < j) j = a->fc_delay;
    if ((e = (uint32_t)a->d.timer + 1) < j) j = e;
    if (a->d.start_delay && a->d.start_delay < j) j = a->d.start_delay;
    if (audio) {
        if (tri_steps(a) && (e = (uint32_t)a->t.timer + 1) < j) j = e;
        if (a->n.lc.len && env_out(&a->n.env) && (e = (uint32_t)a->n.timer + 1) < j) j = e;
        for (int i = 0; i < 2; i++)
            if (pulse_audible(n, i) && (e = 2u * a->p[i].timer + 1 + (a->odd ? 1 : 0)) < j) j = e;
        uint32_t room = ~n->rate_phase; /* cycles until the phase wraps */
        if ((e = room / n->rate_step + 1) < j) j = e;
    }
    return j;
}

static void skip_cycles(nes_t *n, uint32_t k)
{
    apu_t *a = APU;
    a->fc_count += k;
    if (a->fc_new >= 0) a->fc_delay = (uint8_t)(a->fc_delay - k);
    a->fc_block = a->fc_block > k ? (uint8_t)(a->fc_block - k) : 0;
    a->d.timer = (uint16_t)(a->d.timer - k);
    if (a->d.start_delay) a->d.start_delay = (uint8_t)(a->d.start_delay - k);

    /* silent or unchanged channels still advance their phase exactly */
    uint32_t c = timer_adv(&a->t.timer, a->t.period, k);
    if (c && tri_steps(a)) {
        a->t.pos = (uint8_t)((a->t.pos + c) & 31);
    }
    uint32_t processed = a->odd ? k / 2 : (k + 1) / 2;
    for (int i = 0; i < 2; i++) {
        c = timer_adv(&a->p[i].timer, a->p[i].period, processed);
        if (c) {
            a->p[i].pos = (uint8_t)((a->p[i].pos + c) & 7);
        }
    }
    a->odd ^= (uint8_t)(k & 1);
    c = timer_adv(&a->n.timer, a->n.period - 1u, k);
    if (c) {
        uint16_t l = a->n.lfsr;
        int sh = a->n.mode ? 6 : 1;
        while (c--)
            l = (uint16_t)((l >> 1) | (((l ^ (l >> sh)) & 1) << 14));
        a->n.lfsr = l;
    }
    /* none of the above is audible (or audio is off), so amp is unchanged */
    if (n->audio_on) {
        n->acc += a->amp * (int32_t)k;
        n->acc_n += (int32_t)k;
        n->rate_phase += n->rate_step * k;
    }
}

static void update_event(nes_t *n)
{
    apu_t *a = APU;
    /* the CPU must catch the APU up when it can raise the frame IRQ or
     * request a DMC DMA; everything else is only seen through registers */
    uint32_t j = fc_steps[a->fc_mode][a->fc_step] - a->fc_count;
    if (a->fc_new >= 0 && a->fc_delay < j) j = a->fc_delay;
    uint32_t dmc = (uint32_t)a->d.timer + 1 + (uint32_t)(a->d.bits - 1) * a->d.period;
    if (dmc < j) j = dmc;
    if (a->d.start_delay && a->d.start_delay < j) j = a->d.start_delay;
    a->ev_cycle = a->cycle + j - 1;
    cpu_resched(n);
}

void apu_sync(nes_t *n)
{
    apu_t *a = APU;
    uint64_t target = n->s.cpu.cycles;
    /* a->cycle is the next CPU cycle to process */
    while (a->cycle <= target) {
        uint64_t left = target - a->cycle + 1;
        if (!a->lc_pending && (!a->dirty || !n->audio_on)) {
            uint32_t j = cycles_to_event(n);
            if (j > 1) {
                uint32_t k = j - 1;
                if (k > left) k = (uint32_t)left;
                skip_cycles(n, k);
                a->cycle += k;
                continue;
            }
        }
        apu_step(n);
        a->cycle++;
    }
    update_event(n);
}

uint8_t apu_read_status(nes_t *n)
{
    apu_t *a = APU;
    apu_sync(n);
    uint8_t r = 0;
    if (a->p[0].lc.len) r |= 1;
    if (a->p[1].lc.len) r |= 2;
    if (a->t.lc.len) r |= 4;
    if (a->n.lc.len) r |= 8;
    if (a->d.remain) r |= 0x10;
    if (n->s.cpu.irq & IRQ_FRAME) r |= 0x40;
    if (n->s.cpu.irq & IRQ_DMC) r |= 0x80;
    cpu_clear_irq(n, IRQ_FRAME);
    return r;
}

static void write_env(env_t *e, uint8_t v)
{
    e->loop = (v >> 5) & 1;
    e->constant = (v >> 4) & 1;
    e->vol = v & 15;
}

void apu_write(nes_t *n, uint16_t addr, uint8_t v)
{
    apu_t *a = APU;
    apu_sync(n);
    a->dirty = 1;
    a->lc_pending = 1;
    switch (addr) {
    case 0x4000: case 0x4004: {
        pulse_t *p = &a->p[(addr >> 2) & 1];
        p->duty = v >> 6;
        p->lc.new_halt = (v >> 5) & 1;
        write_env(&p->env, v);
    } break;
    case 0x4001: case 0x4005: {
        pulse_t *p = &a->p[(addr >> 2) & 1];
        p->sw_en = v >> 7;
        p->sw_period = (v >> 4) & 7;
        p->sw_neg = (v >> 3) & 1;
        p->sw_shift = v & 7;
        p->sw_reload = 1;
    } break;
    case 0x4002: case 0x4006: {
        pulse_t *p = &a->p[(addr >> 2) & 1];
        p->period = (uint16_t)((p->period & 0x700) | v);
    } break;
    case 0x4003: case 0x4007: {
        pulse_t *p = &a->p[(addr >> 2) & 1];
        p->period = (uint16_t)((p->period & 0xFF) | ((v & 7) << 8));
        lc_load(&p->lc, v >> 3);
        p->pos = 0;
        p->env.start = 1;
    } break;
    case 0x4008:
        a->t.ctrl = v >> 7;
        a->t.lc.new_halt = v >> 7;
        a->t.lin_reload = v & 0x7F;
        break;
    case 0x400A:
        a->t.period = (uint16_t)((a->t.period & 0x700) | v);
        break;
    case 0x400B:
        a->t.period = (uint16_t)((a->t.period & 0xFF) | ((v & 7) << 8));
        lc_load(&a->t.lc, v >> 3);
        a->t.lin_flag = 1;
        break;
    case 0x400C:
        a->n.lc.new_halt = (v >> 5) & 1;
        write_env(&a->n.env, v);
        break;
    case 0x400E:
        a->n.mode = v >> 7;
        a->n.period = noise_table[v & 15];
        break;
    case 0x400F:
        lc_load(&a->n.lc, v >> 3);
        a->n.env.start = 1;
        break;
    case 0x4010:
        a->d.irq_en = v >> 7;
        a->d.loop = (v >> 6) & 1;
        a->d.period = dmc_table[v & 15];
        if (!a->d.irq_en)
            cpu_clear_irq(n, IRQ_DMC);
        break;
    case 0x4011:
        a->d.level = v & 0x7F;
        break;
    case 0x4012:
        a->d.addr = (uint16_t)(0xC000 | (v << 6));
        break;
    case 0x4013:
        a->d.len = (uint16_t)((v << 4) | 1);
        break;
    case 0x4015: {
        lenc_t *l[4] = {&a->p[0].lc, &a->p[1].lc, &a->t.lc, &a->n.lc};
        for (int i = 0; i < 4; i++) {
            l[i]->enabled = (v >> i) & 1;
            if (!l[i]->enabled)
                l[i]->len = 0;
        }
        cpu_clear_irq(n, IRQ_DMC);
        if (!(v & 0x10)) {
            a->d.remain = 0;
        } else if (a->d.remain == 0) {
            a->d.cur = a->d.addr;
            a->d.remain = a->d.len;
            /* the DMA starts 2-3 cycles after the write */
            a->d.start_delay = (n->s.cpu.cycles & 1) ? 3 : 2;
        }
    } break;
    case 0x4017:
        a->fc_new = v;
        a->fc_delay = (n->s.cpu.cycles & 1) ? 4 : 3;
        a->fc_inhibit = (v >> 6) & 1;
        if (a->fc_inhibit)
            cpu_clear_irq(n, IRQ_FRAME);
        break;
    }
    update_event(n);
}

void apu_power(nes_t *n)
{
    apu_t *a = APU;
    init_mix();
    memset(a, 0, sizeof(*a));
    a->n.lfsr = 1;
    a->n.period = noise_table[0];
    a->d.period = dmc_table[0];
    a->d.timer = DMC_PHASE;
    a->d.bits = 8;
    a->d.buf_empty = 1;
    a->d.silence = 1;
    a->d.addr = a->d.cur = 0xC000;
    a->d.len = 1;
    a->fc_new = 0;
    a->fc_delay = 3;
    a->dirty = 1;
    a->cycle = 0; /* next cycle to process (the CPU counts from 0 at power) */
    a->ev_cycle = 0;
}

void apu_reset(nes_t *n)
{
    apu_t *a = APU;
    apu_sync(n);
    apu_write(n, 0x4015, 0);
    a->fc_new = a->fc_mode ? 0x80 : 0;
    a->fc_delay = 3;
    a->fc_step = 0;
    a->fc_count = 0;
    a->t.pos = 0;
    a->d.level &= 1;
    /* pulse and noise length counters are un-halted; triangle untouched */
    a->p[0].lc.halt = a->p[0].lc.new_halt = 0;
    a->p[1].lc.halt = a->p[1].lc.new_halt = 0;
    a->n.lc.halt = a->n.lc.new_halt = 0;
    update_event(n);
}
