/*
 * Ricoh 2C02 PPU, emulated one dot at a time: background fetch pipeline,
 * cycle-by-cycle sprite evaluation (including the overflow bug), sprite
 * fetches, and the exact VBlank/NMI timing races.
 */
#include "core.h"

#define PPU (&n->s.ppu)
#ifndef EVAL_FAST
#define EVAL_FAST 1
#endif
#ifndef FASTPATH2
#define FASTPATH2 1
#endif
#ifndef FASTPATH
#define FASTPATH 1
#endif

static void update_nmi(nes_t *n)
{
    ppu_t *p = PPU;
    n->s.cpu.nmi_line = (p->status & 0x80) && (p->ctrl & 0x80);
}

INLINE void bus(nes_t *n, uint16_t a)
{
    if (n->map_bus)
        n->map_bus(n, a);
}

INLINE uint8_t vread(nes_t *n, uint16_t a)
{
    a &= 0x3FFF;
    uint8_t v = a < 0x2000 ? chr_read(n, a) : *nt_ptr(n, a);
    bus(n, a);
    return v;
}

static void vwrite(nes_t *n, uint16_t a, uint8_t v)
{
    a &= 0x3FFF;
    if (a < 0x2000) {
        if (n->chr_is_ram)
            n->chr[n->s.m.chr_off[a >> 10] + (a & 0x3FF)] = v;
    } else {
        *nt_ptr(n, a) = v;
    }
}

INLINE uint8_t pal_index(uint16_t a)
{
    a &= 0x1F;
    if ((a & 0x13) == 0x10)
        a &= 0x0F;
    return (uint8_t)a;
}

static void init_spread(void);

void ppu_power(nes_t *n)
{
    ppu_t *p = PPU;
    memset(p, 0, sizeof(*p));
    init_spread();
    p->cpal_dirty = 1;
    p->scanline = 0;
    p->dot = 0;
    p->warmup = 1;
    static const uint8_t boot_pal[32] = {
        0x09, 0x01, 0x00, 0x01, 0x00, 0x02, 0x02, 0x0D, 0x08, 0x10, 0x08, 0x24, 0x00, 0x00, 0x04, 0x2C,
        0x09, 0x01, 0x34, 0x03, 0x00, 0x04, 0x00, 0x14, 0x08, 0x3A, 0x00, 0x02, 0x00, 0x20, 0x2C, 0x08};
    memcpy(p->pal, boot_pal, 32);
    memset(p->oam, 0xFF, sizeof(p->oam));
}

void ppu_reset(nes_t *n)
{
    ppu_t *p = PPU;
    p->ctrl = p->mask = 0;
    p->cpal_dirty = 1;
    p->w = 0;
    p->buf = 0;
    p->x = 0;
    p->t = 0;
    update_nmi(n);
}

INLINE void inc_x(ppu_t *p)
{
    if ((p->v & 0x1F) == 31) {
        p->v &= (uint16_t)~0x1F;
        p->v ^= 0x400;
    } else {
        p->v++;
    }
}

INLINE void inc_y(ppu_t *p)
{
    if ((p->v & 0x7000) != 0x7000) {
        p->v += 0x1000;
    } else {
        p->v &= (uint16_t)~0x7000;
        int y = (p->v & 0x3E0) >> 5;
        if (y == 29) {
            y = 0;
            p->v ^= 0x800;
        } else if (y == 31) {
            y = 0;
        } else {
            y++;
        }
        p->v = (uint16_t)((p->v & ~0x3E0) | (y << 5));
    }
}

static const uint8_t rev8[256] = {
#define R2(n) n, n + 2 * 64, n + 1 * 64, n + 3 * 64
#define R4(n) R2(n), R2(n + 2 * 16), R2(n + 1 * 16), R2(n + 3 * 16)
#define R6(n) R4(n), R4(n + 2 * 4), R4(n + 1 * 4), R4(n + 3 * 4)
    R6(0), R6(2), R6(1), R6(3)};

/* Cycle-accurate sprite evaluation, dots 1-256 of visible lines. */
static void sprite_eval(nes_t *n, int d)
{
    ppu_t *p = PPU;
    int sl = p->scanline;
    if (d < 65) {
        p->ev_buf = 0xFF;
        if (!(d & 1))
            p->oam2[(d - 1) >> 1] = 0xFF;
        return;
    }
    if (d == 65) {
        p->ev_s0 = 0;
        p->ev_inrange = 0;
        p->ev_o2 = 0;
        p->ev_bug = 0;
        p->ev_done = 0;
        p->ev_h = (p->oamaddr >> 2) & 0x3F;
        p->ev_l = p->oamaddr & 3;
    } else if (d == 256) {
        p->ev_cnt = p->ev_o2 >> 2;
    }
    if (d & 1) {
        p->ev_buf = p->oam[p->oamaddr];
        return;
    }
    if (p->ev_done) {
        p->ev_h = (p->ev_h + 1) & 0x3F;
        if (p->ev_o2 >= 0x20)
            p->ev_buf = p->oam2[p->ev_o2 & 0x1F];
    } else {
        int h = (p->ctrl & 0x20) ? 16 : 8;
        if (!p->ev_inrange && sl >= p->ev_buf && sl < p->ev_buf + h)
            p->ev_inrange = 1;
        if (p->ev_o2 < 0x20) {
            p->oam2[p->ev_o2] = p->ev_buf;
            if (p->ev_inrange) {
                p->ev_l++;
                p->ev_o2++;
                if (p->ev_h == 0)
                    p->ev_s0 = 1;
                if ((p->ev_o2 & 3) == 0) {
                    p->ev_inrange = 0;
                    p->ev_l = 0;
                    p->ev_h = (p->ev_h + 1) & 0x3F;
                    if (p->ev_h == 0)
                        p->ev_done = 1;
                }
            } else {
                p->ev_h = (p->ev_h + 1) & 0x3F;
                if (p->ev_h == 0)
                    p->ev_done = 1;
            }
        } else {
            p->ev_buf = p->oam2[p->ev_o2 & 0x1F];
            if (p->ev_inrange) {
                p->status |= 0x20;
                p->ev_l++;
                if (p->ev_l == 4) {
                    p->ev_h = (p->ev_h + 1) & 0x3F;
                    p->ev_l = 0;
                }
                if (p->ev_bug == 0) {
                    p->ev_bug = 3;
                } else if (--p->ev_bug == 0) {
                    p->ev_done = 1;
                    p->ev_l = 0;
                }
            } else {
                /* hardware bug: both the sprite and byte index advance */
                p->ev_h = (p->ev_h + 1) & 0x3F;
                p->ev_l = (p->ev_l + 1) & 3;
                if (p->ev_h == 0)
                    p->ev_done = 1;
            }
        }
    }
    p->oamaddr = (uint8_t)((p->ev_l & 3) | (p->ev_h << 2));
}

/* Sprite evaluation is run lazily: nothing can observe it except PPU
 * register accesses, so it is caught up in one tight loop at dot 256 or
 * right before the CPU touches a PPU register. */
static void eval_catchup(nes_t *n, int upto)
{
    ppu_t *p = PPU;
    if (p->scanline >= 240 || p->ev_pos > upto)
        return;
    if (p->render_en) {
        int d = p->ev_pos;
        /* dots 1-64: secondary OAM is filled with $FF, two dots per byte */
        if (EVAL_FAST && d <= 64) {
            int e = upto < 64 ? upto : 64;
            int first = (d + 1) >> 1, last = e >> 1; /* even dots d..e */
            if (last >= first)
                memset(&p->oam2[first - 1], 0xFF, (size_t)(last - first + 1));
            p->ev_buf = 0xFF;
            d = e + 1;
        }
        for (; d <= upto; d++) {
            /* whole sprites while fewer than 8 are found and the address is
             * aligned: 2 dots for a miss, 8 dots for a copy */
            if (EVAL_FAST && d > 65 && !(d & 1) && d < 256 && !p->ev_done && !p->ev_inrange &&
                p->ev_l == 0 && p->ev_o2 < 0x20) {
                int sl = p->scanline, hgt = (p->ctrl & 0x20) ? 16 : 8;
                while (d + 1 <= upto && d + 1 < 256 && p->ev_o2 < 0x20 && !p->ev_done) {
                    uint8_t y = p->ev_buf;
                    if (sl >= y && sl < y + hgt) {
                        if (d + 7 > upto || d + 7 >= 256)
                            break;
                        uint8_t *src = &p->oam[p->ev_h << 2];
                        p->oam2[p->ev_o2] = y;
                        p->oam2[p->ev_o2 + 1] = src[1];
                        p->oam2[p->ev_o2 + 2] = src[2];
                        p->oam2[p->ev_o2 + 3] = src[3];
                        p->ev_o2 += 4;
                        if (p->ev_h == 0)
                            p->ev_s0 = 1;
                        d += 8;
                    } else {
                        p->oam2[p->ev_o2] = y;
                        d += 2;
                    }
                    p->ev_h = (p->ev_h + 1) & 0x3F;
                    if (p->ev_h == 0)
                        p->ev_done = 1;
                    p->oamaddr = (uint8_t)(p->ev_h << 2);
                    p->ev_buf = p->oam[p->oamaddr]; /* the odd dot's read */
                }
                if (d > upto)
                    break;
            }
            if (EVAL_FAST && p->ev_done && d > 65) {
                /* all 64 sprites examined: only the address keeps moving */
                int evens = (upto >> 1) - ((d - 1) >> 1);
                uint8_t h_before = p->ev_h;
                p->ev_h = (uint8_t)((p->ev_h + evens) & 0x3F);
                p->oamaddr = (uint8_t)((p->ev_l & 3) | (p->ev_h << 2));
                if (upto & 1) {
                    p->ev_buf = p->oam[p->oamaddr];
                } else if (p->ev_o2 >= 0x20) {
                    p->ev_buf = p->oam2[p->ev_o2 & 0x1F];
                } else if (upto - 1 >= d) {
                    uint8_t h = (uint8_t)((h_before + evens - 1) & 0x3F);
                    p->ev_buf = p->oam[(p->ev_l & 3) | (h << 2)];
                }
                if (upto >= 256)
                    p->ev_cnt = p->ev_o2 >> 2;
                break;
            }
            sprite_eval(n, d);
        }
    }
    p->ev_pos = (uint16_t)(upto + 1);
}

static void sprite_fetch(nes_t *n, int d)
{
    ppu_t *p = PPU;
    int i = (d - 257) >> 3;
    int ph = (d - 257) & 7;
    if (ph == 0) {
        bus(n, 0x2000 | (p->v & 0xFFF));
        return;
    }
    if (ph != 4)
        return;
    uint8_t y = p->oam2[i * 4], tile = p->oam2[i * 4 + 1];
    uint8_t attr = p->oam2[i * 4 + 2], x = p->oam2[i * 4 + 3];
    int h = (p->ctrl & 0x20) ? 16 : 8;
    int row = (p->scanline - y) & (h - 1);
    int valid = i < p->ev_cnt && p->scanline != 261;
    if (!valid) {
        tile = 0xFF;
        row = 0;
        attr = 0;
        x = 0xFF;
    }
    if (attr & 0x80)
        row = (h - 1) - row;
    uint16_t addr;
    if (h == 16)
        addr = (uint16_t)(((tile & 1) << 12) | ((tile & 0xFE) << 4) | ((row & 8) << 1) | (row & 7));
    else
        addr = (uint16_t)(((p->ctrl & 0x08) << 9) | (tile << 4) | row);
    uint8_t lo = vread(n, addr);
    uint8_t hi = vread(n, (uint16_t)(addr + 8));
    if (!valid)
        lo = hi = 0;
    if (attr & 0x40) {
        lo = rev8[lo];
        hi = rev8[hi];
    }
    p->sp_lo[i] = lo;
    p->sp_hi[i] = hi;
    p->sp_a[i] = attr;
    p->sp_x[i] = x;
}

/* Pre-render the 8 fetched sprites into a 256-pixel line buffer:
 * bits 0-1 pixel, 2-3 palette, 5 behind-background, 6 sprite 0. */
static void build_sprite_line(ppu_t *p)
{
    memset(p->sp_line, 0, sizeof(p->sp_line));
    for (int i = (int)p->sp_cnt - 1; i >= 0; i--) {
        uint8_t lo = p->sp_lo[i], hi = p->sp_hi[i];
        if (!(lo | hi))
            continue;
        uint8_t tag = (uint8_t)(((p->sp_a[i] & 3) << 2) | (p->sp_a[i] & 0x20) | ((i == 0 && p->sp_s0) ? 0x40 : 0));
        for (int k = 0; k < 8; k++) {
            int x = p->sp_x[i] + k;
            if (x > 255)
                break;
            uint8_t px = (uint8_t)(((hi >> (7 - k)) & 1) << 1 | ((lo >> (7 - k)) & 1));
            if (px)
                p->sp_line[x] = (uint8_t)(tag | px);
        }
    }
}

INLINE void reload_shifters(ppu_t *p)
{
    p->shl = (uint16_t)((p->shl & 0xFF00) | p->plo);
    p->shh = (uint16_t)((p->shh & 0xFF00) | p->phi);
    p->sal = (uint16_t)((p->sal & 0xFF00) | ((p->at & 1) ? 0xFF : 0));
    p->sah = (uint16_t)((p->sah & 0xFF00) | ((p->at & 2) ? 0xFF : 0));
}

INLINE void bg_fetch(nes_t *n, int d)
{
    ppu_t *p = PPU;
    switch (d & 7) {
    case 1:
        p->nt = vread(n, 0x2000 | (p->v & 0xFFF));
        break;
    case 3: {
        uint16_t a = (uint16_t)(0x23C0 | (p->v & 0x0C00) | ((p->v >> 4) & 0x38) | ((p->v >> 2) & 7));
        uint8_t at = vread(n, a);
        int sh = ((p->v >> 4) & 4) | (p->v & 2);
        p->at = (at >> sh) & 3;
    } break;
    case 5:
        p->plo = vread(n, (uint16_t)(((p->ctrl & 0x10) << 8) | (p->nt << 4) | (p->v >> 12)));
        break;
    case 7:
        p->phi = vread(n, (uint16_t)(((p->ctrl & 0x10) << 8) | (p->nt << 4) | (p->v >> 12) | 8));
        break;
    case 0:
        inc_x(p);
        break;
    }
}

INLINE void put_pixel(nes_t *n, int d)
{
    ppu_t *p = PPU;
    int x = d - 1;
    uint8_t idx = 0;
    if (p->render_en) {
        uint8_t bg = 0, bgpal = 0;
        if ((p->mask & 0x08) && (x >= 8 || (p->mask & 0x02))) {
            int b = 15 - p->x;
            bg = (uint8_t)(((p->shh >> b) & 1) << 1 | ((p->shl >> b) & 1));
            bgpal = (uint8_t)(((p->sah >> b) & 1) << 1 | ((p->sal >> b) & 1));
        }
        uint8_t sp = 0, spa = 0;
        int s0 = 0;
        if ((p->mask & 0x10) && (x >= 8 || (p->mask & 0x04))) {
            uint8_t e = p->sp_line[x];
            sp = e & 3;
            spa = (uint8_t)(((e >> 2) & 3) | (e & 0x20));
            s0 = (e >> 6) & 1;
        }
        if (s0 && bg && x != 255)
            p->status |= 0x40;
        if (sp && (!bg || !(spa & 0x20)))
            idx = (uint8_t)(0x10 | (spa & 3) << 2 | sp);
        else if (bg)
            idx = (uint8_t)(bgpal << 2 | bg);
    } else if ((p->v & 0x3F00) == 0x3F00) {
        idx = p->v & 0x1F;
    }
    uint8_t c = p->pal[pal_index(idx)] & ((p->mask & 1) ? 0x30 : 0x3F);
    n->fb[p->scanline * 256 + x] = (uint16_t)(c | ((p->mask & 0xE0) << 1));
}

static void ppu_dot(nes_t *n)
{
    ppu_t *p = PPU;
    /* advance */
    uint16_t d = (uint16_t)(p->dot + 1);
    if (d == 340 && p->skip_dot) {
        d = 341;
        p->skip_dot = 0;
    }
    if (d > 340) {
        d = 0;
        if (++p->scanline > 261)
            p->scanline = 0;
        p->ev_pos = 1;
        if (p->scanline == 240) {
            p->frame++;
            p->frame_done = 1;
        }
    }
    p->dot = d;
    p->dots++;

    if (p->vdelay && --p->vdelay == 0) {
        p->v = p->vdelay_val;
        if (!(p->render_en && (p->scanline < 240 || p->scanline == 261)))
            bus(n, p->v);
    }

    int sl = p->scanline;
    if (sl < 240 || sl == 261) {
        if (p->render_en) {
            if ((d >= 2 && d <= 257) || (d >= 322 && d <= 337)) {
                p->shl <<= 1;
                p->shh <<= 1;
                p->sal <<= 1;
                p->sah <<= 1;
            }
            if ((d & 7) == 1 && ((d >= 9 && d <= 257) || d == 329 || d == 337))
                reload_shifters(p);
            if ((d >= 1 && d <= 256) || (d >= 321 && d <= 336))
                bg_fetch(n, d);
            if (d == 256)
                inc_y(p);
            else if (d == 257)
                p->v = (uint16_t)((p->v & ~0x041F) | (p->t & 0x041F));
            else if (sl == 261 && d >= 280 && d <= 304)
                p->v = (uint16_t)((p->v & ~0x7BE0) | (p->t & 0x7BE0));
            if (sl < 240 && d == 256)
                eval_catchup(n, 256);
            if (d >= 257 && d <= 320) {
                if (d == 257) {
                    p->sp_cnt = (sl == 261) ? 0 : p->ev_cnt;
                    p->sp_s0 = p->ev_s0;
                }
                p->oamaddr = 0;
                sprite_fetch(n, d);
                if (d == 320)
                    build_sprite_line(p);
            }
        }
        if (sl < 240 && d >= 1 && d <= 256)
            put_pixel(n, d);
        /* odd frames are one dot shorter when rendering (decided at dot 339) */
        if (sl == 261 && d == 339)
            p->skip_dot = (p->frame & 1) && p->render_en;
        if (sl == 261 && d == 1) {
            p->status &= 0x1F;
            update_nmi(n);
            p->warmup = 0;
        }
    } else if (sl == 241 && d == 1) {
        if (!p->prevent_vbl)
            p->status |= 0x80;
        p->prevent_vbl = 0;
        update_nmi(n);
    }

    p->render_en = (p->mask & 0x18) != 0;
}

/* Fast path: one 8-dot tile slot (dots 8k+1..8k+8, k = 0..30) of a visible
 * line with rendering on and no register writes in flight. Produces exactly
 * what eight ppu_dot() calls would, without the per-dot dispatch. */
/* Nibble-packed background pipeline used by the span renderer: each pixel
 * is 4 bits (attribute bits 3-2, pattern bits 1-0), 8 pixels per word, so
 * the four hardware shift registers become one 64-bit (two-word) shift. */
static uint32_t spread[4][256];

static void init_spread(void)
{
    if (spread[0][1]) return;
    for (int v = 0; v < 256; v++)
        for (int k = 0; k < 8; k++)
            if (v & (0x80 >> k))
                for (int t = 0; t < 4; t++)
                    spread[t][v] |= 1u << (4 * (7 - k) + t);
}

INLINE uint32_t pack8(uint8_t lo, uint8_t hi, uint8_t alo, uint8_t ahi)
{
    return spread[0][lo] | spread[1][hi] | spread[2][alo] | spread[3][ahi];
}

INLINE uint8_t unpack8(uint32_t w, int t)
{
    uint8_t r = 0;
    for (int k = 0; k < 8; k++)
        r = (uint8_t)(r << 1 | ((w >> (4 * (7 - k) + t)) & 1));
    return r;
}

/* Fast path for the visible part of a rendering scanline: runs dots
 * dot+1 .. last (all within 1..255) exactly like ppu_dot() would.
 * Pixels never depend on the fetches made in the same dot, and the
 * fetches keep their exact order and timing. */
static void render_span(nes_t *n, int last)
{
    ppu_t *p = PPU;
    int d = p->dot + 1;
    uint8_t mask = p->mask;

    if (p->cpal_dirty) {
        uint8_t gray = (mask & 1) ? 0x30 : 0x3F;
        uint16_t emph = (uint16_t)((mask & 0xE0) << 1);
        for (int i = 0; i < 32; i++)
            p->cpal[i] = (uint16_t)((p->pal[(i & 3) ? i : 0] & gray) | emph);
        p->cpal_dirty = 0;
    }

    uint32_t hi = pack8((uint8_t)(p->shl >> 8), (uint8_t)(p->shh >> 8), (uint8_t)(p->sal >> 8), (uint8_t)(p->sah >> 8));
    uint32_t lo = pack8((uint8_t)p->shl, (uint8_t)p->shh, (uint8_t)p->sal, (uint8_t)p->sah);
    uint16_t *restrict row = &n->fb[p->scanline * 256 - 1];
    const uint16_t *restrict cpal = p->cpal;
    const uint8_t *restrict spl = p->sp_line - 1;
    int sh = 28 - 4 * p->x;
    int bg_from = (mask & 0x08) ? ((mask & 0x02) ? 1 : 9) : 1000;
    int sp_from = (mask & 0x10) ? ((mask & 0x04) ? 1 : 9) : 1000;
    int nsp = p->sp_cnt;
    uint8_t status = p->status;
    /* fetch state in locals; the mapper hook only sees the bus address */
    uint16_t v = p->v;
    uint8_t nt = p->nt, at = p->at, plo = p->plo, phi = p->phi;
    uint16_t pt = (uint16_t)((p->ctrl & 0x10) << 8);
    uint64_t dots0 = p->dots - (uint64_t)p->dot;
    void (*hook)(nes_t *, uint16_t) = n->map_bus;

    for (; d <= last; d++) {
        /* whole tile slot: 8 pixels from one 32-bit window, then 4 fetches */
        if ((d & 7) == 1 && d >= 9 && d + 7 <= last) {
            uint32_t hi1 = (hi << 4) | (lo >> 28);
            uint32_t t = pack8(plo, phi, (at & 1) ? 0xFF : 0, (at & 2) ? 0xFF : 0);
            int xs = (28 - sh); /* 4 * fine x */
            uint32_t w = xs ? (hi1 << xs) | (t >> (32 - xs)) : hi1;
            hi = (hi1 << 28) | (t >> 4);
            lo = t << 28;
            if (bg_from > 9)
                w = 0;
            uint16_t *o = &row[d];
            const uint8_t *e8 = &spl[d];
            if (!nsp || d < sp_from || !(e8[0] | e8[1] | e8[2] | e8[3] | e8[4] | e8[5] | e8[6] | e8[7])) {
                o[0] = cpal[w >> 28];
                o[1] = cpal[(w >> 24) & 15];
                o[2] = cpal[(w >> 20) & 15];
                o[3] = cpal[(w >> 16) & 15];
                o[4] = cpal[(w >> 12) & 15];
                o[5] = cpal[(w >> 8) & 15];
                o[6] = cpal[(w >> 4) & 15];
                o[7] = cpal[w & 15];
            } else {
                for (int k = 0; k < 8; k++) {
                    uint32_t idx = (w >> (28 - 4 * k)) & 15;
                    uint32_t opaque = idx & 3;
                    uint8_t e = e8[k];
                    if (e & 3) {
                        if ((e & 0x40) && opaque)
                            status |= 0x40;
                        if (!opaque || !(e & 0x20))
                            idx = 0x10 | (e & 0x0F);
                    }
                    o[k] = cpal[idx];
                }
            }
            uint16_t a = (uint16_t)(0x2000 | (v & 0xFFF));
            nt = *nt_ptr(n, a);
            if (hook) { p->dots = dots0 + (uint64_t)d; hook(n, a); }
            a = (uint16_t)(0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 7));
            at = (*nt_ptr(n, a) >> (((v >> 4) & 4) | (v & 2))) & 3;
            if (hook) { p->dots = dots0 + (uint64_t)d + 2; hook(n, a); }
            a = (uint16_t)(pt | (nt << 4) | (v >> 12));
            plo = chr_read(n, a);
            if (hook) { p->dots = dots0 + (uint64_t)d + 4; hook(n, a); }
            a = (uint16_t)(a | 8);
            phi = chr_read(n, a);
            if (hook) { p->dots = dots0 + (uint64_t)d + 6; hook(n, a); }
            if ((v & 0x1F) == 31) {
                v &= (uint16_t)~0x1F;
                v ^= 0x400;
            } else {
                v++;
            }
            d += 7;
            continue;
        }
        if (d >= 2) {
            hi = (hi << 4) | (lo >> 28);
            lo <<= 4;
        }
        if ((d & 7) == 1 && d >= 9)
            lo = pack8(plo, phi, (at & 1) ? 0xFF : 0, (at & 2) ? 0xFF : 0);
        uint32_t idx = 0;
        if (d >= bg_from) {
            idx = (hi >> sh) & 15;
            if (!(idx & 3))
                idx = 0;
        }
        if (nsp && d >= sp_from) {
            uint8_t e = spl[d];
            if (e & 3) {
                if ((e & 0x40) && idx)
                    status |= 0x40;
                if (!idx || !(e & 0x20))
                    idx = 0x10 | (e & 0x0F);
            }
        }
        row[d] = cpal[idx];

        uint16_t a;
        switch (d & 7) {
        case 1:
            a = (uint16_t)(0x2000 | (v & 0xFFF));
            nt = *nt_ptr(n, a);
            break;
        case 3:
            a = (uint16_t)(0x23C0 | (v & 0x0C00) | ((v >> 4) & 0x38) | ((v >> 2) & 7));
            at = (*nt_ptr(n, a) >> (((v >> 4) & 4) | (v & 2))) & 3;
            break;
        case 5:
            a = (uint16_t)(pt | (nt << 4) | (v >> 12));
            plo = chr_read(n, a);
            break;
        case 7:
            a = (uint16_t)(pt | (nt << 4) | (v >> 12) | 8);
            phi = chr_read(n, a);
            break;
        case 0:
            if ((v & 0x1F) == 31) {
                v &= (uint16_t)~0x1F;
                v ^= 0x400;
            } else {
                v++;
            }
            continue;
        default:
            continue;
        }
        if (hook) {
            p->dots = dots0 + (uint64_t)d;
            hook(n, a);
        }
    }
    p->status = status;
    p->v = v;
    p->nt = nt; p->at = at; p->plo = plo; p->phi = phi;
    p->shl = (uint16_t)(unpack8(hi, 0) << 8 | unpack8(lo, 0));
    p->shh = (uint16_t)(unpack8(hi, 1) << 8 | unpack8(lo, 1));
    p->sal = (uint16_t)(unpack8(hi, 2) << 8 | unpack8(lo, 2));
    p->sah = (uint16_t)(unpack8(hi, 3) << 8 | unpack8(lo, 3));
    uint32_t steps = (uint32_t)(last - p->dot);
    p->dot = (uint16_t)last;
    p->dots = dots0 + (uint64_t)last;
    p->master += steps * PPU_DIV;
}

/* Fast path: one sprite-fetch slot (dots 257+8i .. 264+8i). */
static void sprite_slot(nes_t *n)
{
    ppu_t *p = PPU;
    int d0 = p->dot + 1;
    if (d0 == 257) {
        /* dot 257 is still part of the background pipeline */
        p->shl <<= 1; p->shh <<= 1; p->sal <<= 1; p->sah <<= 1;
        reload_shifters(p);
        p->v = (uint16_t)((p->v & ~0x041F) | (p->t & 0x041F));
        p->sp_cnt = (p->scanline == 261) ? 0 : p->ev_cnt;
        p->sp_s0 = p->ev_s0;
    }
    if (p->scanline == 261 && d0 + 7 >= 280 && d0 <= 304)
        p->v = (uint16_t)((p->v & ~0x7BE0) | (p->t & 0x7BE0));
    p->oamaddr = 0;
    uint64_t dots0 = p->dots;
    p->dots = dots0 + 1;
    sprite_fetch(n, d0);
    p->dots = dots0 + 5;
    sprite_fetch(n, d0 + 4);
    p->dots = dots0;
    if (d0 + 7 == 320)
        build_sprite_line(p);
    p->dot = (uint16_t)(d0 + 7);
    p->dots += 8;
    p->master += 8 * PPU_DIV;
}

/* Fast path: the end of a rendering line, dots 321..last (<= 340): the
 * first two tiles of the next line are fetched. No pixels are output. */
static void hblank_span(nes_t *n, int last)
{
    ppu_t *p = PPU;
    for (int d = p->dot + 1; d <= last; d++) {
        p->dots++;
        if (d >= 322 && d <= 337) {
            p->shl <<= 1; p->shh <<= 1; p->sal <<= 1; p->sah <<= 1;
        }
        if (d == 329 || d == 337)
            reload_shifters(p);
        if (d <= 336)
            bg_fetch(n, d);
    }
    p->master += (uint64_t)(last - p->dot) * PPU_DIV;
    p->dot = (uint16_t)last;
}

void ppu_run(nes_t *n, uint64_t target)
{
    ppu_t *p = PPU;
    while (p->master + PPU_DIV <= target) {
        if (FASTPATH && p->scanline < 240 && p->dot < 255 && p->render_en &&
            (p->mask & 0x18) && !p->vdelay && p->master + 2 * PPU_DIV <= target) {
            uint64_t avail = (target - p->master) / PPU_DIV;
            int last = p->dot + (int)(avail > 255 ? 255 : avail);
            if (last > 255) last = 255;
            render_span(n, last);
            continue;
        }
        if (FASTPATH2 && (p->scanline < 240 || p->scanline == 261) && p->dot >= 320 && p->dot < 338 &&
            p->render_en && (p->mask & 0x18) && !p->vdelay && p->master + 2 * PPU_DIV <= target) {
            uint64_t avail = (target - p->master) / PPU_DIV;
            int cap = p->scanline == 261 ? 338 : 340;
            int last = p->dot + (int)(avail > 40 ? 40 : avail);
            if (last > cap) last = cap;
            if (last > p->dot) {
                hblank_span(n, last);
                continue;
            }
        }
        if (FASTPATH2 && (p->scanline < 240 || p->scanline == 261) && (p->dot & 7) == 0 &&
            p->dot >= 256 && p->dot <= 312 && p->render_en && (p->mask & 0x18) && !p->vdelay &&
            p->master + 8 * PPU_DIV <= target) {
            sprite_slot(n);
            continue;
        }
        /* Idle stretch of vertical blank: nothing observable happens until
         * the pre-render line, so jump ahead in one step. */
        int sl = p->scanline;
        if (sl >= 241 && sl <= 260 && !p->vdelay && !(sl == 241 && p->dot == 0)) {
            uint64_t avail = (target - p->master) / PPU_DIV;
            uint32_t pos = (uint32_t)sl * 341 + p->dot;
            uint32_t end = sl == 240 ? 241 * 341 : 260 * 341 + 340;
            if (pos < end) {
                uint64_t k = end - pos;
                if (k > avail) k = avail;
                pos += (uint32_t)k;
                p->scanline = (int16_t)(pos / 341);
                p->dot = (uint16_t)(pos % 341);
                p->dots += k;
                p->master += k * PPU_DIV;
                p->render_en = (p->mask & 0x18) != 0;
                continue;
            }
        }
        p->master += PPU_DIV;
        ppu_dot(n);
    }
    /* next point the CPU must sync at: frame end (240,0), VBlank set
     * (241,1) or VBlank clear (261,1). One dot early, to be safe across
     * the odd-frame skipped dot. */
    uint32_t pos = (uint32_t)p->scanline * 341 + p->dot;
    uint32_t ev;
    if (pos < 240 * 341) ev = 240 * 341;
    else if (pos < 241 * 341 + 1) ev = 241 * 341 + 1;
    else if (pos < 261 * 341 + 1) ev = 261 * 341 + 1;
    else ev = 262 * 341 + 240 * 341;
    uint64_t ahead = ev - pos - 1;
    if (n->s.m.safe_until != UINT64_MAX) {
        uint64_t lim = n->s.m.safe_until > p->dots ? n->s.m.safe_until - p->dots : 0;
        if (lim < ahead)
            ahead = lim;
    }
    p->ev_master = p->master + ahead * PPU_DIV;
    cpu_resched(n);
}

/* ---- CPU-facing registers ---- */

static uint8_t latch_get(ppu_t *p)
{
    for (int b = 0; b < 8; b++)
        if (p->frame - p->latch_frame[b] > 36)
            p->latch &= (uint8_t)~(1 << b);
    return p->latch;
}

static void latch_set(ppu_t *p, uint8_t v, uint8_t mask)
{
    p->latch = (uint8_t)((p->latch & ~mask) | (v & mask));
    for (int b = 0; b < 8; b++)
        if (mask & (1 << b))
            p->latch_frame[b] = (uint32_t)p->frame;
}

INLINE int rendering(ppu_t *p)
{
    return p->render_en && (p->scanline < 240 || p->scanline == 261);
}

static void inc_v_after_access(nes_t *n)
{
    ppu_t *p = PPU;
    if (rendering(p)) {
        inc_x(p);
        inc_y(p);
    } else {
        p->v = (uint16_t)((p->v + ((p->ctrl & 4) ? 32 : 1)) & 0x7FFF);
        bus(n, p->v & 0x3FFF);
    }
}

INLINE void sync_eval(nes_t *n)
{
    ppu_t *p = PPU;
    if (p->scanline < 240 && p->dot >= 1)
        eval_catchup(n, p->dot > 256 ? 256 : p->dot);
}

uint8_t ppu_read_reg(nes_t *n, uint16_t a)
{
    ppu_t *p = PPU;
    sync_eval(n);
    uint8_t open = latch_get(p);
    switch (a & 7) {
    case 2: {
        uint8_t r = (uint8_t)((p->status & 0xE0) | (open & 0x1F));
        p->status &= 0x7F;
        p->w = 0;
        update_nmi(n);
        if (p->scanline == 241 && p->dot == 0)
            p->prevent_vbl = 1;
        latch_set(p, r, 0xE0);
        return r;
    }
    case 4: {
        uint8_t r;
        if (p->render_en && p->scanline < 240) {
            if (p->dot >= 257 && p->dot <= 320) {
                int i = (p->dot - 257) >> 3;
                int k = (p->dot - 257) & 7;
                r = p->oam2[i * 4 + (k > 3 ? 3 : k)];
            } else {
                r = p->ev_buf;
            }
        } else {
            r = p->oam[p->oamaddr];
            if ((p->oamaddr & 3) == 2)
                r &= 0xE3;
        }
        latch_set(p, r, 0xFF);
        return r;
    }
    case 7: {
        uint16_t va = p->v & 0x3FFF;
        uint8_t r;
        if (va >= 0x3F00) {
            r = (uint8_t)((p->pal[pal_index(va)] & ((p->mask & 1) ? 0x30 : 0x3F)) | (open & 0xC0));
            p->buf = *nt_ptr(n, (uint16_t)(va - 0x1000));
            latch_set(p, r, 0x3F);
        } else {
            r = p->buf;
            p->buf = va < 0x2000 ? chr_read(n, va) : *nt_ptr(n, va);
            latch_set(p, r, 0xFF);
        }
        inc_v_after_access(n);
        return r;
    }
    default:
        return open;
    }
}

void ppu_write_reg(nes_t *n, uint16_t a, uint8_t v)
{
    ppu_t *p = PPU;
    sync_eval(n);
    latch_set(p, v, 0xFF);
    switch (a & 7) {
    case 0:
        if (p->warmup) return;
        p->ctrl = v;
        p->t = (uint16_t)((p->t & 0xF3FF) | ((v & 3) << 10));
        update_nmi(n);
        break;
    case 1:
        if (p->warmup) return;
        p->mask = v;
        p->cpal_dirty = 1;
        break;
    case 3:
        p->oamaddr = v;
        break;
    case 4:
        if (rendering(p)) {
            p->oamaddr = (uint8_t)(p->oamaddr + 4);
        } else {
            if ((p->oamaddr & 3) == 2)
                v &= 0xE3;
            p->oam[p->oamaddr++] = v;
        }
        break;
    case 5:
        if (p->warmup) return;
        if (!p->w) {
            p->t = (uint16_t)((p->t & ~0x1F) | (v >> 3));
            p->x = v & 7;
        } else {
            p->t = (uint16_t)((p->t & 0x8C1F) | ((v & 0xF8) << 2) | ((v & 7) << 12));
        }
        p->w ^= 1;
        break;
    case 6:
        if (p->warmup) return;
        if (!p->w) {
            p->t = (uint16_t)((p->t & 0x00FF) | ((v & 0x3F) << 8));
        } else {
            p->t = (uint16_t)((p->t & 0xFF00) | v);
            p->vdelay_val = p->t;
            p->vdelay = 3;
        }
        p->w ^= 1;
        break;
    case 7: {
        uint16_t va = p->v & 0x3FFF;
        if (va >= 0x3F00) {
            p->pal[pal_index(va)] = v & 0x3F;
            p->cpal_dirty = 1;
        } else
            vwrite(n, va, v);
        inc_v_after_access(n);
    } break;
    }
}
