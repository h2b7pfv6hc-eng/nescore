/*
 * Cartridge loading (iNES / NES 2.0) and mappers.
 *
 * Supported: 0 NROM, 1 MMC1, 2 UxROM, 3 CNROM, 4 MMC3, 7 AxROM,
 * 9 MMC2, 10 MMC4, 11 Color Dreams, 66 GxROM, 71 Camerica.
 * Together these cover the large majority of the licensed library.
 */
#include "core.h"
#include <stdlib.h>

#define M (&n->s.m)

enum { MIR_H, MIR_V, MIR_A, MIR_B, MIR_4 };

static void set_mirror(nes_t *n, int mode)
{
    static const uint8_t maps[5][4] = {
        {0, 0, 1, 1}, {0, 1, 0, 1}, {0, 0, 0, 0}, {1, 1, 1, 1}, {0, 1, 2, 3}};
    if (n->four_screen)
        mode = MIR_4;
    memcpy(M->nt_map, maps[mode], 4);
}

static void prg8(nes_t *n, int slot, int bank)
{
    uint32_t count = n->prg_size / 0x2000;
    if (bank < 0)
        bank += (int)count;
    M->prg_off[slot] = ((uint32_t)bank % count) * 0x2000;
}

static void prg16(nes_t *n, int slot, int bank)
{
    uint32_t count = n->prg_size / 0x4000;
    if (count == 0) count = 1;
    if (bank < 0)
        bank += (int)count;
    uint32_t b = (uint32_t)bank % count;
    M->prg_off[slot * 2] = (b * 0x4000) % n->prg_size;
    M->prg_off[slot * 2 + 1] = (b * 0x4000 + 0x2000) % n->prg_size;
}

static void prg32(nes_t *n, int bank)
{
    prg16(n, 0, bank * 2);
    prg16(n, 1, bank * 2 + 1);
}

static void chr1(nes_t *n, int slot, int bank)
{
    uint32_t count = n->chr_size / 0x400;
    M->chr_off[slot] = ((uint32_t)bank % count) * 0x400;
}

static void chr4(nes_t *n, int slot, int bank)
{
    for (int i = 0; i < 4; i++)
        chr1(n, slot * 4 + i, bank * 4 + i);
}

static void chr8(nes_t *n, int bank)
{
    for (int i = 0; i < 8; i++)
        chr1(n, i, bank * 8 + i);
}

/* ---------------- NROM / simple discrete boards ---------------- */

static void nrom_reset(nes_t *n)
{
    prg16(n, 0, 0);
    prg16(n, 1, -1);
    chr8(n, 0);
}

static void uxrom_write(nes_t *n, uint16_t a, uint8_t v)
{
    if (a >= 0x8000)
        prg16(n, 0, v);
}

static void cnrom_write(nes_t *n, uint16_t a, uint8_t v)
{
    if (a >= 0x8000)
        chr8(n, v);
}

static void axrom_reset(nes_t *n)
{
    prg32(n, 0);
    chr8(n, 0);
    set_mirror(n, MIR_A);
}

static void axrom_write(nes_t *n, uint16_t a, uint8_t v)
{
    if (a >= 0x8000) {
        prg32(n, v & 0x0F);
        set_mirror(n, (v & 0x10) ? MIR_B : MIR_A);
    }
}

static void colordreams_reset(nes_t *n)
{
    prg32(n, 0);
    chr8(n, 0);
}

static void colordreams_write(nes_t *n, uint16_t a, uint8_t v)
{
    if (a >= 0x8000) {
        prg32(n, v & 3);
        chr8(n, v >> 4);
    }
}

static void gxrom_write(nes_t *n, uint16_t a, uint8_t v)
{
    if (a >= 0x8000) {
        prg32(n, (v >> 4) & 3);
        chr8(n, v & 3);
    }
}

static void camerica_write(nes_t *n, uint16_t a, uint8_t v)
{
    if (a >= 0xC000)
        prg16(n, 0, v);
    else if (a >= 0x9000 && a < 0xA000)
        set_mirror(n, (v & 0x10) ? MIR_B : MIR_A);
}

/* ---------------- MMC1 ---------------- */
/* r[0]=control r[1]=chr0 r[2]=chr1 r[3]=prg */

static void mmc1_update(nes_t *n)
{
    uint8_t *r = M->r;
    static const int mir[4] = {MIR_A, MIR_B, MIR_V, MIR_H};
    set_mirror(n, mir[r[0] & 3]);
    int outer = (n->prg_size > 0x40000) ? (r[1] & 0x10) : 0;
    int bank = (r[3] & 0x0F) | outer;
    switch ((r[0] >> 2) & 3) {
    case 0: case 1:
        prg16(n, 0, (bank & ~1));
        prg16(n, 1, (bank & ~1) + 1);
        break;
    case 2:
        prg16(n, 0, outer);
        prg16(n, 1, bank);
        break;
    case 3:
        prg16(n, 0, bank);
        prg16(n, 1, outer | 0x0F);
        break;
    }
    if (r[0] & 0x10) {
        chr4(n, 0, r[1]);
        chr4(n, 1, r[2]);
    } else {
        chr8(n, (r[1] & 0x1E) >> 1);
    }
    M->ram_enable = !(r[3] & 0x10);
}

static void mmc1_reset(nes_t *n)
{
    M->r[0] = 0x0C;
    M->r[1] = M->r[2] = M->r[3] = 0;
    M->shift = 0;
    M->shift_count = 0;
    mmc1_update(n);
}

static void mmc1_write(nes_t *n, uint16_t a, uint8_t v)
{
    if (a < 0x8000)
        return;
    uint64_t now = n->s.cpu.cycles;
    /* consecutive-cycle writes (RMW instructions) only count once */
    if (now - M->last_write < 2) {
        M->last_write = now;
        return;
    }
    M->last_write = now;
    if (v & 0x80) {
        M->shift = 0;
        M->shift_count = 0;
        M->r[0] |= 0x0C;
        mmc1_update(n);
        return;
    }
    M->shift |= (uint8_t)((v & 1) << M->shift_count);
    if (++M->shift_count == 5) {
        M->r[(a >> 13) & 3] = M->shift;
        M->shift = 0;
        M->shift_count = 0;
        mmc1_update(n);
    }
}

/* ---------------- MMC3 ---------------- */
/* r[0..7]=bank regs r[8]=bank select r[9]=mirroring */

INLINE void mmc3_fine(nes_t *n)
{
    /* The PPU may lag behind the CPU only as long as the IRQ cannot fire.
     * Allow up to 4 counter clocks per scanline to stay safe with odd
     * pattern-table setups (8x16 sprites mixing both tables). */
    if (!M->irq_enable) {
        M->fine = 0;
        M->safe_until = UINT64_MAX;
    } else if (M->irq_counter <= 1 || M->irq_reload) {
        M->fine = 1;
        M->safe_until = 0;
    } else {
        M->fine = 0;
        M->safe_until = n->s.ppu.dots + (uint64_t)((M->irq_counter - 1) / 4) * 341;
    }
    if (M->safe_until != UINT64_MAX) {
        /* pull the PPU's next sync point in if the IRQ can now come sooner */
        uint64_t lim = M->safe_until > n->s.ppu.dots ? M->safe_until - n->s.ppu.dots : 0;
        uint64_t ev = n->s.ppu.master + lim * PPU_DIV;
        if (ev < n->s.ppu.ev_master)
            n->s.ppu.ev_master = ev;
    }
    cpu_resched(n);
}

static void mmc3_update(nes_t *n)
{
    uint8_t *r = M->r;
    if (r[8] & 0x40) {
        prg8(n, 0, -2);
        prg8(n, 2, r[6]);
    } else {
        prg8(n, 0, r[6]);
        prg8(n, 2, -2);
    }
    prg8(n, 1, r[7]);
    prg8(n, 3, -1);
    int inv = (r[8] & 0x80) ? 4 : 0;
    chr1(n, 0 ^ inv, r[0] & 0xFE);
    chr1(n, 1 ^ inv, r[0] | 1);
    chr1(n, 2 ^ inv, r[1] & 0xFE);
    chr1(n, 3 ^ inv, r[1] | 1);
    chr1(n, 4 ^ inv, r[2]);
    chr1(n, 5 ^ inv, r[3]);
    chr1(n, 6 ^ inv, r[4]);
    chr1(n, 7 ^ inv, r[5]);
    set_mirror(n, (r[9] & 1) ? MIR_H : MIR_V);
}

static void mmc3_reset(nes_t *n)
{
    static const uint8_t init[8] = {0, 2, 4, 5, 6, 7, 0, 1};
    memcpy(M->r, init, 8);
    M->r[8] = 0;
    M->r[9] = n->vertical ? 0 : 1;
    M->ram_enable = 1;
    M->ram_write = 1;
    M->irq_counter = M->irq_latch = 0;
    M->irq_enable = M->irq_reload = 0;
    mmc3_update(n);
}

static void mmc3_write(nes_t *n, uint16_t a, uint8_t v)
{
    if (a < 0x8000)
        return;
    switch (a & 0xE001) {
    case 0x8000: M->r[8] = v; mmc3_update(n); break;
    case 0x8001: M->r[M->r[8] & 7] = v; mmc3_update(n); break;
    case 0xA000: M->r[9] = v; mmc3_update(n); break;
    case 0xA001:
        M->ram_enable = (v >> 7) & 1;
        M->ram_write = !((v >> 6) & 1);
        break;
    case 0xC000: M->irq_latch = v; break;
    case 0xC001: M->irq_counter = 0; M->irq_reload = 1; break;
    case 0xE000: M->irq_enable = 0; cpu_clear_irq(n, IRQ_MAPPER); break;
    case 0xE001: M->irq_enable = 1; break;
    }
    mmc3_fine(n);
}

static void mmc3_bus(nes_t *n, uint16_t a)
{
    if (a & 0x1000) {
        if (!M->a12) {
            M->a12 = 1;
            if (n->s.ppu.dots - M->a12_low_at > 10) {
                if (M->irq_counter == 0 || M->irq_reload) {
                    M->irq_counter = M->irq_latch;
                    M->irq_reload = 0;
                } else {
                    M->irq_counter--;
                }
                if (M->irq_counter == 0 && M->irq_enable)
                    cpu_set_irq(n, IRQ_MAPPER);
                mmc3_fine(n);
            }
        }
    } else if (M->a12) {
        M->a12 = 0;
        M->a12_low_at = n->s.ppu.dots;
    }
}

/* ---------------- MMC2 / MMC4 ---------------- */
/* r[0]=chr0 FD r[1]=chr0 FE r[2]=chr1 FD r[3]=chr1 FE r[4]=latch0 r[5]=latch1 */

static void mmc2_update_chr(nes_t *n)
{
    chr4(n, 0, M->r[4] ? M->r[1] : M->r[0]);
    chr4(n, 1, M->r[5] ? M->r[3] : M->r[2]);
}

static void mmc2_reset(nes_t *n)
{
    if (n->mapper == 9) {
        prg8(n, 0, 0);
        prg8(n, 1, -3);
        prg8(n, 2, -2);
        prg8(n, 3, -1);
    } else {
        prg16(n, 0, 0);
        prg16(n, 1, -1);
    }
    memset(M->r, 0, 6);
    M->r[4] = M->r[5] = 1;
    mmc2_update_chr(n);
}

static void mmc2_write(nes_t *n, uint16_t a, uint8_t v)
{
    switch (a & 0xF000) {
    case 0xA000:
        if (n->mapper == 9) prg8(n, 0, v & 0x0F);
        else prg16(n, 0, v & 0x0F);
        break;
    case 0xB000: M->r[0] = v & 0x1F; mmc2_update_chr(n); break;
    case 0xC000: M->r[1] = v & 0x1F; mmc2_update_chr(n); break;
    case 0xD000: M->r[2] = v & 0x1F; mmc2_update_chr(n); break;
    case 0xE000: M->r[3] = v & 0x1F; mmc2_update_chr(n); break;
    case 0xF000: set_mirror(n, (v & 1) ? MIR_H : MIR_V); break;
    }
}

static void mmc2_bus(nes_t *n, uint16_t a)
{
    if (a >= 0x2000)
        return;
    int changed = 0;
    if (n->mapper == 9 ? a == 0x0FD8 : (a & 0xFFF8) == 0x0FD8) { M->r[4] = 0; changed = 1; }
    else if (n->mapper == 9 ? a == 0x0FE8 : (a & 0xFFF8) == 0x0FE8) { M->r[4] = 1; changed = 1; }
    else if ((a & 0xFFF8) == 0x1FD8) { M->r[5] = 0; changed = 1; }
    else if ((a & 0xFFF8) == 0x1FE8) { M->r[5] = 1; changed = 1; }
    if (changed)
        mmc2_update_chr(n);
}

/* ---------------- loader ---------------- */

int cart_load(nes_t *n, const uint8_t *d, size_t size)
{
    if (size < 16 || memcmp(d, "NES\x1A", 4) != 0)
        return NES_ERR_FORMAT;
    int nes2 = (d[7] & 0x0C) == 0x08;
    uint32_t prg_units = d[4], chr_units = d[5];
    int mapper = (d[6] >> 4) | (d[7] & 0xF0);
    int sub = 0;
    if (nes2) {
        mapper |= (d[8] & 0x0F) << 8;
        sub = d[8] >> 4;
        prg_units |= (uint32_t)(d[9] & 0x0F) << 8;
        chr_units |= (uint32_t)(d[9] & 0xF0) << 4;
    } else if (d[12] || d[13] || d[14] || d[15]) {
        mapper &= 0x0F; /* old dumping tools wrote junk into bytes 7-15 */
    }
    size_t off = 16 + ((d[6] & 4) ? 512 : 0);
    uint32_t prg_size = prg_units * 0x4000;
    uint32_t chr_size = chr_units * 0x2000;
    if (prg_size == 0 || off + prg_size > size)
        return NES_ERR_FORMAT;
    if (off + prg_size + chr_size > size)
        chr_size = (uint32_t)(size - off - prg_size) & ~0x1FFFu;

    switch (mapper) {
    case 0: case 1: case 2: case 3: case 4: case 7:
    case 9: case 10: case 11: case 66: case 71:
        break;
    default:
        return NES_ERR_MAPPER;
    }

    uint8_t *rom = (uint8_t *)malloc(prg_size + (chr_size ? chr_size : 1));
    if (!rom)
        return NES_ERR_MEMORY;
    memcpy(rom, d + off, prg_size);
    if (chr_size)
        memcpy(rom + prg_size, d + off + prg_size, chr_size);

    free(n->rom);
    n->rom = rom;
    n->prg = rom;
    n->prg_size = prg_size;
    if (chr_size) {
        n->chr = rom + prg_size;
        n->chr_size = chr_size;
        n->chr_is_ram = 0;
    } else {
        n->chr = n->s.chr_ram;
        n->chr_size = sizeof(n->s.chr_ram);
        n->chr_is_ram = 1;
    }
    n->mapper = mapper;
    n->submapper = sub;
    n->battery = (d[6] >> 1) & 1;
    n->four_screen = (d[6] >> 3) & 1;
    n->vertical = d[6] & 1;

    n->map_write = NULL;
    n->map_bus = NULL;
    n->map_reset = nrom_reset;
    switch (mapper) {
    case 1: n->map_write = mmc1_write; n->map_reset = mmc1_reset; break;
    case 2: n->map_write = uxrom_write; break;
    case 3: n->map_write = cnrom_write; break;
    case 4: n->map_write = mmc3_write; n->map_bus = mmc3_bus; n->map_reset = mmc3_reset; break;
    case 7: n->map_write = axrom_write; n->map_reset = axrom_reset; break;
    case 9: case 10: n->map_write = mmc2_write; n->map_bus = mmc2_bus; n->map_reset = mmc2_reset; break;
    case 11: n->map_write = colordreams_write; n->map_reset = colordreams_reset; break;
    case 66: n->map_write = gxrom_write; n->map_reset = colordreams_reset; break;
    case 71: n->map_write = camerica_write; break;
    }
    return NES_OK;
}

void cart_power(nes_t *n)
{
    memset(M, 0, sizeof(*M));
    M->ram_enable = 1;
    M->ram_write = 1;
    M->safe_until = UINT64_MAX;
    set_mirror(n, n->vertical ? MIR_V : MIR_H);
    n->map_reset(n);
}

void cart_reset(nes_t *n)
{
    if (n->mapper == 1 || n->mapper == 4) {
        /* these boards keep their registers across a reset */
        return;
    }
    if (n->mapper == 2 || n->mapper == 3 || n->mapper == 0 || n->mapper == 71)
        n->map_reset(n);
}
