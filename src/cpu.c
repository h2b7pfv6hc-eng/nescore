/*
 * Ricoh 2A03 CPU. Every memory access is one CPU cycle; the PPU and APU
 * are advanced inside each cycle (split into two halves around the bus
 * access) so register reads/writes land on the exact PPU dot.
 * Interrupt lines are sampled per cycle like the real chip: the decision
 * to take an IRQ/NMI uses the state at the end of the second-to-last cycle.
 */
#include "core.h"

#define CPU (&n->s.cpu)
#ifndef APU_LOCKSTEP
#define APU_LOCKSTEP 0
#endif
#ifndef FORCE_FINE
#define FORCE_FINE 0
#endif

/* Work that only happens at scheduled points (VBlank/NMI, frame end, mapper
 * IRQ, APU IRQ/DMA). Kept out of line so the per-cycle path stays small,
 * which matters for the Wii's 32 KB instruction cache. */
static NOINLINE void cycle_events(nes_t *n)
{
    cpu_t *c = CPU;
    uint64_t end = (c->cycles + 1) * CPU_DIV;
    if (end - PPU_OFFSET >= n->s.ppu.ev_master || n->s.m.fine || FORCE_FINE)
        ppu_run(n, end - PPU_OFFSET);
    if (c->cycles >= n->s.apu.ev_cycle)
        apu_sync(n);
    cpu_resched(n);
}

/* Recompute the next cycle at which cycle_events() must run. */
void cpu_resched(nes_t *n)
{
    cpu_t *c = CPU;
    uint32_t now = (uint32_t)c->cycles;
    uint32_t at = (uint32_t)n->s.apu.ev_cycle;
    if (n->s.m.fine || FORCE_FINE) {
        at = now;
    } else {
        /* first cycle N with 12(N+1) - PPU_OFFSET >= ev_master */
        int64_t delta = (int64_t)(n->s.ppu.ev_master + PPU_OFFSET) - (int64_t)((c->cycles + 1) * CPU_DIV);
        uint32_t p = now;
        if (delta > 0) {
            if (delta > 0x10000000)
                delta = 0x10000000;
            p += ((uint32_t)delta + CPU_DIV - 1) / CPU_DIV; /* 32-bit divide */
        }
        if ((int32_t)(p - at) < 0)
            at = p;
    }
    c->sync_at = at;
}

INLINE void start_cycle(nes_t *n, int read)
{
    cpu_t *c = CPU;
    c->phase = read ? 5 : 7;
    c->cycles++;
#if APU_LOCKSTEP
    apu_sync(n);
#endif
}

INLINE void end_cycle(nes_t *n, int read)
{
    cpu_t *c = CPU;
    /* The PPU and APU run lazily: they are caught up before any register or
     * mapper access, and here only at events the CPU can see without asking
     * (VBlank/NMI, frame end, an armed mapper IRQ, APU IRQ/DMA). */
    if (UNLIKELY((int32_t)((uint32_t)c->cycles - c->sync_at) >= 0))
        cycle_events(n);
    c->prev_need_nmi = c->need_nmi;
    if (!c->prev_nmi_line && c->nmi_line)
        c->need_nmi = 1;
    c->prev_nmi_line = c->nmi_line;
    c->prev_run_irq = c->run_irq;
    c->run_irq = c->irq && !(c->p & FI);
}

void cpu_set_irq(nes_t *n, uint8_t src) { CPU->irq |= src; }
void cpu_clear_irq(nes_t *n, uint8_t src) { CPU->irq &= (uint8_t)~src; }

/* OAM DMA and DMC DMA. The CPU is halted on a read cycle; DMA "get"
 * cycles fall on even CPU cycles, "put" cycles on odd ones. */
static void process_dma(nes_t *n, uint16_t read_addr)
{
    cpu_t *c = CPU;
    uint16_t spr_count = 0;
    uint8_t spr_addr = 0, val = 0;
    int skip_dummy = (read_addr == 0x4016 || read_addr == 0x4017);

    start_cycle(n, 1);
    if (!skip_dummy)
        bus_read(n, read_addr);
    end_cycle(n, 1);
    c->need_halt = 0;

    while (c->dmc_running || c->spr_running) {
        int get = (c->cycles & 1) == 0;
        if (get) {
            if (c->dmc_running && !c->need_halt && !c->need_dummy) {
                if (c->need_halt) c->need_halt = 0;
                else if (c->need_dummy) c->need_dummy = 0;
                start_cycle(n, 1);
                val = bus_read(n, apu_dmc_addr(n));
                end_cycle(n, 1);
                c->dmc_running = 0;
                apu_dmc_dma_done(n, val);
            } else if (c->spr_running) {
                if (c->need_halt) c->need_halt = 0;
                else if (c->need_dummy) c->need_dummy = 0;
                start_cycle(n, 1);
                val = bus_read(n, (uint16_t)(c->spr_page << 8 | spr_addr));
                end_cycle(n, 1);
                spr_addr++;
                spr_count++;
            } else {
                if (c->need_halt) c->need_halt = 0;
                else if (c->need_dummy) c->need_dummy = 0;
                start_cycle(n, 1);
                if (!skip_dummy)
                    bus_read(n, read_addr);
                end_cycle(n, 1);
            }
        } else {
            if (c->spr_running && (spr_count & 1)) {
                if (c->need_halt) c->need_halt = 0;
                else if (c->need_dummy) c->need_dummy = 0;
                start_cycle(n, 1);
                bus_write(n, 0x2004, val);
                end_cycle(n, 1);
                spr_count++;
                if (spr_count == 0x200)
                    c->spr_running = 0;
            } else {
                if (c->need_halt) c->need_halt = 0;
                else if (c->need_dummy) c->need_dummy = 0;
                start_cycle(n, 1);
                if (!skip_dummy)
                    bus_read(n, read_addr);
                end_cycle(n, 1);
            }
        }
    }
}

static NOINLINE uint8_t rd(nes_t *n, uint16_t a)
{
    if (UNLIKELY(CPU->need_halt))
        process_dma(n, a);
    start_cycle(n, 1);
    uint8_t v = bus_read(n, a);
    end_cycle(n, 1);
    return v;
}

static NOINLINE void wr(nes_t *n, uint16_t a, uint8_t v)
{
    start_cycle(n, 0);
    bus_write(n, a, v);
    end_cycle(n, 0);
}

INLINE void push(nes_t *n, uint8_t v) { wr(n, 0x100 | CPU->s--, v); }
INLINE uint8_t pull(nes_t *n) { return rd(n, 0x100 | ++CPU->s); }

INLINE void setzn(cpu_t *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~(FZ | FN)) | (v ? 0 : FZ) | (v & 0x80));
}

/* ---- addressing modes (they perform the exact dummy reads) ---- */

INLINE uint16_t am_imm(nes_t *n) { return CPU->pc++; }
INLINE uint16_t am_zp(nes_t *n) { return rd(n, CPU->pc++); }
INLINE uint16_t am_zpi(nes_t *n, uint8_t i)
{
    uint8_t b = rd(n, CPU->pc++);
    rd(n, b);
    return (uint8_t)(b + i);
}
INLINE uint16_t am_abs(nes_t *n)
{
    uint16_t lo = rd(n, CPU->pc++);
    uint16_t hi = rd(n, CPU->pc++);
    return (uint16_t)(lo | hi << 8);
}
INLINE uint16_t am_absi(nes_t *n, uint8_t i, int w)
{
    uint16_t b = am_abs(n);
    uint16_t a = (uint16_t)(b + i);
    if (w || ((a ^ b) & 0x100))
        rd(n, (uint16_t)((b & 0xFF00) | (a & 0xFF)));
    return a;
}
INLINE uint16_t am_izx(nes_t *n)
{
    uint8_t p = rd(n, CPU->pc++);
    rd(n, p);
    p = (uint8_t)(p + CPU->x);
    uint16_t lo = rd(n, p);
    uint16_t hi = rd(n, (uint8_t)(p + 1));
    return (uint16_t)(lo | hi << 8);
}
INLINE uint16_t am_izy(nes_t *n, int w)
{
    uint8_t p = rd(n, CPU->pc++);
    uint16_t lo = rd(n, p);
    uint16_t hi = rd(n, (uint8_t)(p + 1));
    uint16_t b = (uint16_t)(lo | hi << 8);
    uint16_t a = (uint16_t)(b + CPU->y);
    if (w || ((a ^ b) & 0x100))
        rd(n, (uint16_t)((b & 0xFF00) | (a & 0xFF)));
    return a;
}

/* ---- ALU ---- */

INLINE void op_adc(cpu_t *c, uint8_t v)
{
    unsigned s = c->a + v + (c->p & FC);
    c->p = (uint8_t)((c->p & ~(FC | FV)) | (s > 0xFF ? FC : 0) |
                     ((~(c->a ^ v) & (c->a ^ s) & 0x80) ? FV : 0));
    c->a = (uint8_t)s;
    setzn(c, c->a);
}
INLINE void op_sbc(cpu_t *c, uint8_t v) { op_adc(c, (uint8_t)~v); }
INLINE void op_cmp(cpu_t *c, uint8_t r, uint8_t v)
{
    uint8_t d = (uint8_t)(r - v);
    c->p = (uint8_t)((c->p & ~FC) | (r >= v ? FC : 0));
    setzn(c, d);
}
INLINE uint8_t op_asl(cpu_t *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~FC) | (v >> 7));
    v <<= 1;
    setzn(c, v);
    return v;
}
INLINE uint8_t op_lsr(cpu_t *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~FC) | (v & 1));
    v >>= 1;
    setzn(c, v);
    return v;
}
INLINE uint8_t op_rol(cpu_t *c, uint8_t v)
{
    uint8_t r = (uint8_t)(v << 1 | (c->p & FC));
    c->p = (uint8_t)((c->p & ~FC) | (v >> 7));
    setzn(c, r);
    return r;
}
INLINE uint8_t op_ror(cpu_t *c, uint8_t v)
{
    uint8_t r = (uint8_t)(v >> 1 | (c->p & FC) << 7);
    c->p = (uint8_t)((c->p & ~FC) | (v & 1));
    setzn(c, r);
    return r;
}
INLINE uint8_t op_inc(cpu_t *c, uint8_t v) { v++; setzn(c, v); return v; }
INLINE uint8_t op_dec(cpu_t *c, uint8_t v) { v--; setzn(c, v); return v; }
INLINE uint8_t op_slo(cpu_t *c, uint8_t v) { v = op_asl(c, v); c->a |= v; setzn(c, c->a); return v; }
INLINE uint8_t op_rla(cpu_t *c, uint8_t v) { v = op_rol(c, v); c->a &= v; setzn(c, c->a); return v; }
INLINE uint8_t op_sre(cpu_t *c, uint8_t v) { v = op_lsr(c, v); c->a ^= v; setzn(c, c->a); return v; }
INLINE uint8_t op_rra(cpu_t *c, uint8_t v) { v = op_ror(c, v); op_adc(c, v); return v; }
INLINE uint8_t op_dcp(cpu_t *c, uint8_t v) { v--; op_cmp(c, c->a, v); return v; }
INLINE uint8_t op_isc(cpu_t *c, uint8_t v) { v++; op_sbc(c, v); return v; }

/* Unstable "SH*" stores: value is ANDed with high byte + 1, and on a page
 * crossing the value also replaces the high byte of the address. */
static void op_sh(nes_t *n, uint16_t base, uint8_t idx, uint8_t reg)
{
    uint16_t a = (uint16_t)(base + idx);
    rd(n, (uint16_t)((base & 0xFF00) | (a & 0xFF)));
    uint8_t v = (uint8_t)(reg & ((base >> 8) + 1));
    if ((a ^ base) & 0x100)
        a = (uint16_t)((a & 0xFF) | (v << 8));
    wr(n, a, v);
}

static void branch(nes_t *n, int cond)
{
    cpu_t *c = CPU;
    int8_t off = (int8_t)rd(n, c->pc++);
    if (cond) {
        /* A taken branch without page crossing delays a pending IRQ. */
        if (c->run_irq && !c->prev_run_irq)
            c->run_irq = 0;
        rd(n, c->pc);
        uint16_t np = (uint16_t)(c->pc + off);
        if ((np ^ c->pc) & 0x100)
            rd(n, (uint16_t)((c->pc & 0xFF00) | (np & 0xFF)));
        c->pc = np;
    }
}

static void interrupt(nes_t *n)
{
    cpu_t *c = CPU;
    rd(n, c->pc);
    rd(n, c->pc);
    push(n, (uint8_t)(c->pc >> 8));
    push(n, (uint8_t)c->pc);
    uint16_t vec;
    if (c->need_nmi) {
        c->need_nmi = 0;
        vec = 0xFFFA;
    } else {
        vec = 0xFFFE;
    }
    push(n, (uint8_t)((c->p | FU) & ~FB));
    c->p |= FI;
    uint16_t lo = rd(n, vec);
    uint16_t hi = rd(n, (uint16_t)(vec + 1));
    c->pc = (uint16_t)(lo | hi << 8);
}

void cpu_power(nes_t *n)
{
    cpu_t *c = CPU;
    memset(c, 0, sizeof(*c));
    c->a = c->x = c->y = 0;
    c->s = 0xFD;
    c->p = FI | FU;
    c->cycles = (uint64_t)-1;
    c->sync_at = 0;
    c->pc = (uint16_t)(bus_read(n, 0xFFFC) | bus_read(n, 0xFFFD) << 8);
    for (int i = 0; i < 8; i++) {
        start_cycle(n, 1);
        end_cycle(n, 1);
    }
}

void cpu_reset(nes_t *n)
{
    cpu_t *c = CPU;
    c->s -= 3;
    c->p |= FI;
    c->irq = 0;
    c->jam = 0;
    c->need_nmi = c->prev_need_nmi = 0;
    c->need_halt = c->need_dummy = c->dmc_running = c->spr_running = 0;
    c->pc = (uint16_t)(bus_read(n, 0xFFFC) | bus_read(n, 0xFFFD) << 8);
    for (int i = 0; i < 8; i++) {
        start_cycle(n, 1);
        end_cycle(n, 1);
    }
}

#define READ(op, mode, body) case op: { uint8_t v = rd(n, mode); body; } break;
#define STORE(op, mode, val) case op: { uint16_t a_ = mode; wr(n, a_, val); } break;
#define RMW(op, mode, fn) case op: { uint16_t a_ = mode; uint8_t v = rd(n, a_); wr(n, a_, v); wr(n, a_, fn(c, v)); } break;
#define IMPL(op, body) case op: rd(n, c->pc); body; break;

#define GROUP_READ(o_imm, o_zp, o_zpx, o_abs, o_abx, o_aby, o_izx, o_izy, body) \
    READ(o_imm, am_imm(n), body) READ(o_zp, am_zp(n), body) READ(o_zpx, am_zpi(n, c->x), body) \
    READ(o_abs, am_abs(n), body) READ(o_abx, am_absi(n, c->x, 0), body) READ(o_aby, am_absi(n, c->y, 0), body) \
    READ(o_izx, am_izx(n), body) READ(o_izy, am_izy(n, 0), body)

#define GROUP_RMW7(o_zp, o_zpx, o_abs, o_abx, o_aby, o_izx, o_izy, fn) \
    RMW(o_zp, am_zp(n), fn) RMW(o_zpx, am_zpi(n, c->x), fn) RMW(o_abs, am_abs(n), fn) \
    RMW(o_abx, am_absi(n, c->x, 1), fn) RMW(o_aby, am_absi(n, c->y, 1), fn) \
    RMW(o_izx, am_izx(n), fn) RMW(o_izy, am_izy(n, 1), fn)

#define GROUP_RMW4(o_zp, o_zpx, o_abs, o_abx, fn) \
    RMW(o_zp, am_zp(n), fn) RMW(o_zpx, am_zpi(n, c->x), fn) RMW(o_abs, am_abs(n), fn) \
    RMW(o_abx, am_absi(n, c->x, 1), fn)

void cpu_step(nes_t *n)
{
    cpu_t *c = CPU;
    if (UNLIKELY(c->jam)) {
        rd(n, c->pc);
        return;
    }
    uint8_t op = rd(n, c->pc++);
    switch (op) {
    GROUP_READ(0x09, 0x05, 0x15, 0x0D, 0x1D, 0x19, 0x01, 0x11, { c->a |= v; setzn(c, c->a); })
    GROUP_READ(0x29, 0x25, 0x35, 0x2D, 0x3D, 0x39, 0x21, 0x31, { c->a &= v; setzn(c, c->a); })
    GROUP_READ(0x49, 0x45, 0x55, 0x4D, 0x5D, 0x59, 0x41, 0x51, { c->a ^= v; setzn(c, c->a); })
    GROUP_READ(0x69, 0x65, 0x75, 0x6D, 0x7D, 0x79, 0x61, 0x71, op_adc(c, v))
    GROUP_READ(0xE9, 0xE5, 0xF5, 0xED, 0xFD, 0xF9, 0xE1, 0xF1, op_sbc(c, v))
    GROUP_READ(0xC9, 0xC5, 0xD5, 0xCD, 0xDD, 0xD9, 0xC1, 0xD1, op_cmp(c, c->a, v))
    GROUP_READ(0xA9, 0xA5, 0xB5, 0xAD, 0xBD, 0xB9, 0xA1, 0xB1, { c->a = v; setzn(c, v); })
    READ(0xEB, am_imm(n), op_sbc(c, v))

    READ(0xA2, am_imm(n), { c->x = v; setzn(c, v); })
    READ(0xA6, am_zp(n), { c->x = v; setzn(c, v); })
    READ(0xB6, am_zpi(n, c->y), { c->x = v; setzn(c, v); })
    READ(0xAE, am_abs(n), { c->x = v; setzn(c, v); })
    READ(0xBE, am_absi(n, c->y, 0), { c->x = v; setzn(c, v); })
    READ(0xA0, am_imm(n), { c->y = v; setzn(c, v); })
    READ(0xA4, am_zp(n), { c->y = v; setzn(c, v); })
    READ(0xB4, am_zpi(n, c->x), { c->y = v; setzn(c, v); })
    READ(0xAC, am_abs(n), { c->y = v; setzn(c, v); })
    READ(0xBC, am_absi(n, c->x, 0), { c->y = v; setzn(c, v); })
    READ(0xE0, am_imm(n), op_cmp(c, c->x, v))
    READ(0xE4, am_zp(n), op_cmp(c, c->x, v))
    READ(0xEC, am_abs(n), op_cmp(c, c->x, v))
    READ(0xC0, am_imm(n), op_cmp(c, c->y, v))
    READ(0xC4, am_zp(n), op_cmp(c, c->y, v))
    READ(0xCC, am_abs(n), op_cmp(c, c->y, v))
    READ(0x24, am_zp(n), { c->p = (uint8_t)((c->p & ~(FZ | FV | FN)) | (v & (FV | FN)) | ((c->a & v) ? 0 : FZ)); })
    READ(0x2C, am_abs(n), { c->p = (uint8_t)((c->p & ~(FZ | FV | FN)) | (v & (FV | FN)) | ((c->a & v) ? 0 : FZ)); })

    STORE(0x85, am_zp(n), c->a)
    STORE(0x95, am_zpi(n, c->x), c->a)
    STORE(0x8D, am_abs(n), c->a)
    STORE(0x9D, am_absi(n, c->x, 1), c->a)
    STORE(0x99, am_absi(n, c->y, 1), c->a)
    STORE(0x81, am_izx(n), c->a)
    STORE(0x91, am_izy(n, 1), c->a)
    STORE(0x86, am_zp(n), c->x)
    STORE(0x96, am_zpi(n, c->y), c->x)
    STORE(0x8E, am_abs(n), c->x)
    STORE(0x84, am_zp(n), c->y)
    STORE(0x94, am_zpi(n, c->x), c->y)
    STORE(0x8C, am_abs(n), c->y)

    case 0x0A: rd(n, c->pc); c->a = op_asl(c, c->a); break;
    case 0x4A: rd(n, c->pc); c->a = op_lsr(c, c->a); break;
    case 0x2A: rd(n, c->pc); c->a = op_rol(c, c->a); break;
    case 0x6A: rd(n, c->pc); c->a = op_ror(c, c->a); break;
    GROUP_RMW4(0x06, 0x16, 0x0E, 0x1E, op_asl)
    GROUP_RMW4(0x46, 0x56, 0x4E, 0x5E, op_lsr)
    GROUP_RMW4(0x26, 0x36, 0x2E, 0x3E, op_rol)
    GROUP_RMW4(0x66, 0x76, 0x6E, 0x7E, op_ror)
    GROUP_RMW4(0xE6, 0xF6, 0xEE, 0xFE, op_inc)
    GROUP_RMW4(0xC6, 0xD6, 0xCE, 0xDE, op_dec)

    /* unofficial read-modify-write combos */
    GROUP_RMW7(0x07, 0x17, 0x0F, 0x1F, 0x1B, 0x03, 0x13, op_slo)
    GROUP_RMW7(0x27, 0x37, 0x2F, 0x3F, 0x3B, 0x23, 0x33, op_rla)
    GROUP_RMW7(0x47, 0x57, 0x4F, 0x5F, 0x5B, 0x43, 0x53, op_sre)
    GROUP_RMW7(0x67, 0x77, 0x6F, 0x7F, 0x7B, 0x63, 0x73, op_rra)
    GROUP_RMW7(0xC7, 0xD7, 0xCF, 0xDF, 0xDB, 0xC3, 0xD3, op_dcp)
    GROUP_RMW7(0xE7, 0xF7, 0xEF, 0xFF, 0xFB, 0xE3, 0xF3, op_isc)

    READ(0xA7, am_zp(n), { c->a = c->x = v; setzn(c, v); })
    READ(0xB7, am_zpi(n, c->y), { c->a = c->x = v; setzn(c, v); })
    READ(0xAF, am_abs(n), { c->a = c->x = v; setzn(c, v); })
    READ(0xBF, am_absi(n, c->y, 0), { c->a = c->x = v; setzn(c, v); })
    READ(0xA3, am_izx(n), { c->a = c->x = v; setzn(c, v); })
    READ(0xB3, am_izy(n, 0), { c->a = c->x = v; setzn(c, v); })
    READ(0xAB, am_imm(n), { c->a = c->x = v; setzn(c, v); })
    STORE(0x87, am_zp(n), c->a & c->x)
    STORE(0x97, am_zpi(n, c->y), c->a & c->x)
    STORE(0x8F, am_abs(n), c->a & c->x)
    STORE(0x83, am_izx(n), c->a & c->x)
    READ(0x0B, am_imm(n), { c->a &= v; setzn(c, c->a); c->p = (uint8_t)((c->p & ~FC) | (c->a >> 7)); })
    READ(0x2B, am_imm(n), { c->a &= v; setzn(c, c->a); c->p = (uint8_t)((c->p & ~FC) | (c->a >> 7)); })
    READ(0x4B, am_imm(n), { c->a &= v; c->a = op_lsr(c, c->a); })
    READ(0x6B, am_imm(n), {
        c->a &= v;
        c->a = (uint8_t)(c->a >> 1 | (c->p & FC) << 7);
        setzn(c, c->a);
        c->p = (uint8_t)((c->p & ~(FC | FV)) | ((c->a >> 6) & 1) | ((((c->a >> 6) ^ (c->a >> 5)) & 1) ? FV : 0));
    })
    READ(0xCB, am_imm(n), {
        uint8_t ax = c->a & c->x;
        c->p = (uint8_t)((c->p & ~FC) | (ax >= v ? FC : 0));
        c->x = (uint8_t)(ax - v);
        setzn(c, c->x);
    })
    READ(0x8B, am_imm(n), { c->a = (uint8_t)((c->a | 0xEE) & c->x & v); setzn(c, c->a); })
    READ(0xBB, am_absi(n, c->y, 0), { v &= c->s; c->a = c->x = c->s = v; setzn(c, v); })
    case 0x9C: { uint16_t b = am_abs(n); op_sh(n, b, c->x, c->y); } break;
    case 0x9E: { uint16_t b = am_abs(n); op_sh(n, b, c->y, c->x); } break;
    case 0x9F: { uint16_t b = am_abs(n); op_sh(n, b, c->y, c->a & c->x); } break;
    case 0x9B: { uint16_t b = am_abs(n); c->s = c->a & c->x; op_sh(n, b, c->y, c->s); } break;
    case 0x93: {
        uint8_t p = rd(n, c->pc++);
        uint16_t lo = rd(n, p);
        uint16_t hi = rd(n, (uint8_t)(p + 1));
        op_sh(n, (uint16_t)(lo | hi << 8), c->y, c->a & c->x);
    } break;

    /* NOPs that still read */
    IMPL(0x1A, ;) IMPL(0x3A, ;) IMPL(0x5A, ;) IMPL(0x7A, ;) IMPL(0xDA, ;) IMPL(0xFA, ;) IMPL(0xEA, ;)
    READ(0x80, am_imm(n), (void)v) READ(0x82, am_imm(n), (void)v) READ(0x89, am_imm(n), (void)v)
    READ(0xC2, am_imm(n), (void)v) READ(0xE2, am_imm(n), (void)v)
    READ(0x04, am_zp(n), (void)v) READ(0x44, am_zp(n), (void)v) READ(0x64, am_zp(n), (void)v)
    READ(0x14, am_zpi(n, c->x), (void)v) READ(0x34, am_zpi(n, c->x), (void)v)
    READ(0x54, am_zpi(n, c->x), (void)v) READ(0x74, am_zpi(n, c->x), (void)v)
    READ(0xD4, am_zpi(n, c->x), (void)v) READ(0xF4, am_zpi(n, c->x), (void)v)
    READ(0x0C, am_abs(n), (void)v)
    READ(0x1C, am_absi(n, c->x, 0), (void)v) READ(0x3C, am_absi(n, c->x, 0), (void)v)
    READ(0x5C, am_absi(n, c->x, 0), (void)v) READ(0x7C, am_absi(n, c->x, 0), (void)v)
    READ(0xDC, am_absi(n, c->x, 0), (void)v) READ(0xFC, am_absi(n, c->x, 0), (void)v)

    /* implied */
    IMPL(0xAA, { c->x = c->a; setzn(c, c->x); })
    IMPL(0x8A, { c->a = c->x; setzn(c, c->a); })
    IMPL(0xA8, { c->y = c->a; setzn(c, c->y); })
    IMPL(0x98, { c->a = c->y; setzn(c, c->a); })
    IMPL(0xBA, { c->x = c->s; setzn(c, c->x); })
    IMPL(0x9A, { c->s = c->x; })
    IMPL(0xE8, { c->x++; setzn(c, c->x); })
    IMPL(0xC8, { c->y++; setzn(c, c->y); })
    IMPL(0xCA, { c->x--; setzn(c, c->x); })
    IMPL(0x88, { c->y--; setzn(c, c->y); })
    IMPL(0x18, { c->p &= (uint8_t)~FC; })
    IMPL(0x38, { c->p |= FC; })
    IMPL(0x58, { c->p &= (uint8_t)~FI; })
    IMPL(0x78, { c->p |= FI; })
    IMPL(0xB8, { c->p &= (uint8_t)~FV; })
    IMPL(0xD8, { c->p &= (uint8_t)~FD; })
    IMPL(0xF8, { c->p |= FD; })

    /* stack */
    IMPL(0x48, push(n, c->a))
    IMPL(0x08, push(n, c->p | FB | FU))
    IMPL(0x68, { rd(n, 0x100 | c->s); c->a = pull(n); setzn(c, c->a); })
    IMPL(0x28, { rd(n, 0x100 | c->s); c->p = (uint8_t)((pull(n) & ~FB) | FU); })

    /* flow */
    case 0x4C: c->pc = am_abs(n); break;
    case 0x6C: {
        uint16_t p = am_abs(n);
        uint16_t lo = rd(n, p);
        uint16_t hi = rd(n, (uint16_t)((p & 0xFF00) | ((p + 1) & 0xFF)));
        c->pc = (uint16_t)(lo | hi << 8);
    } break;
    case 0x20: {
        uint16_t lo = rd(n, c->pc++);
        rd(n, 0x100 | c->s);
        push(n, (uint8_t)(c->pc >> 8));
        push(n, (uint8_t)c->pc);
        uint16_t hi = rd(n, c->pc);
        c->pc = (uint16_t)(lo | hi << 8);
    } break;
    case 0x60: {
        rd(n, c->pc);
        rd(n, 0x100 | c->s);
        uint16_t lo = pull(n);
        uint16_t hi = pull(n);
        c->pc = (uint16_t)(lo | hi << 8);
        rd(n, c->pc++);
    } break;
    case 0x40: {
        rd(n, c->pc);
        rd(n, 0x100 | c->s);
        c->p = (uint8_t)((pull(n) & ~FB) | FU);
        uint16_t lo = pull(n);
        uint16_t hi = pull(n);
        c->pc = (uint16_t)(lo | hi << 8);
    } break;
    case 0x00: {
        rd(n, c->pc++);
        push(n, (uint8_t)(c->pc >> 8));
        push(n, (uint8_t)c->pc);
        uint16_t vec = 0xFFFE;
        if (c->need_nmi) {
            c->need_nmi = 0;
            vec = 0xFFFA;
        }
        push(n, c->p | FB | FU);
        c->p |= FI;
        uint16_t lo = rd(n, vec);
        uint16_t hi = rd(n, (uint16_t)(vec + 1));
        c->pc = (uint16_t)(lo | hi << 8);
        c->prev_need_nmi = 0;
    } break;

    case 0x10: branch(n, !(c->p & FN)); break;
    case 0x30: branch(n, c->p & FN); break;
    case 0x50: branch(n, !(c->p & FV)); break;
    case 0x70: branch(n, c->p & FV); break;
    case 0x90: branch(n, !(c->p & FC)); break;
    case 0xB0: branch(n, c->p & FC); break;
    case 0xD0: branch(n, !(c->p & FZ)); break;
    case 0xF0: branch(n, c->p & FZ); break;

    default: /* KIL/JAM: the CPU locks up until reset */
        c->jam = 1;
        c->pc--;
        return;
    }

    if (c->prev_run_irq || c->prev_need_nmi)
        interrupt(n);
}
