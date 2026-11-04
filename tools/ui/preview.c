/* Renders the Wii menu screens to PPM images on a PC, using the drawing
 * code extracted from wii/source/main.c (tools/ui/extract.py). */
#include "../../src/nes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define MENU_W 512
#define MENU_H 480
extern u8 console_font_8x16[];
static u16 menu_lin[MENU_W * MENU_H];
static u16 pal565[512];
static nes_t *nes;
static char rom_name[256] = "Super Mario Bros. 3 (USA).nes";
static u32 frame_us_avg = 7340;
static struct { int runahead, smooth, crop, stretch, region, palette; } cfg = {1, 0, 1, 2, 0, 1};
#include "ui_extracted.h"

static void save(const char *path, int px, int py)
{
    if (px >= 0) m_cursor(px, py);
    FILE *f = fopen(path, "wb");
    /* stretch 512 -> 640 wide like the TV does */
    fprintf(f, "P6\n640 480\n255\n");
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++) {
            u16 c = menu_lin[y * MENU_W + x * 512 / 640];
            fputc(((c >> 11) & 31) * 255 / 31, f);
            fputc(((c >> 5) & 63) * 255 / 63, f);
            fputc((c & 31) * 255 / 31, f);
        }
    fclose(f);
}

int main(int argc, char **argv)
{
    uint32_t rgb[512];
    nes_palette_rgb_ex(1, rgb);
    for (int i = 0; i < 512; i++) {
        uint32_t c = rgb[i];
        pal565[i] = (u16)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
    }
    /* a game frame for the thumbnail */
    FILE *f = fopen(argv[1], "rb");
    static unsigned char rom[1 << 20];
    size_t sz = fread(rom, 1, sizeof rom, f);
    fclose(f);
    nes = nes_create(48000);
    nes_load_rom(nes, rom, sz);
    for (int i = 0; i < 300; i++) nes_run_frame(nes);

    static const char *names[] = {"Castlevania (USA).nes", "Duck Hunt (World).nes", "Mega Man 2 (USA).nes",
        "Metroid (USA).nes", "Super Mario Bros. (World).nes", "Super Mario Bros. 3 (USA).nes",
        "The Legend of Zelda (USA).nes", "Tetris (USA).nes", "Zelda II - The Adventure of Link (USA).nes",
        "Kirby's Adventure (USA).nes", "Punch-Out!! (USA).nes"};
    n_entries = 13;
    entries = calloc(n_entries, sizeof(entry_t));
    strcpy(entries[0].name, "Europe"); entries[0].is_dir = 1;
    strcpy(entries[1].name, "Homebrew"); entries[1].is_dir = 1;
    for (int i = 0; i < 11; i++) strcpy(entries[2 + i].name, names[i]);

    /* game list: 9 rows, scroll buttons, back button */
    n_hits = 0;
    for (int i = 0; i < 9; i++) hit_add(16, 84 + i * 38, 436, 34, B_ROW + i, 0);
    hit_add(460, 84, 36, 166, B_UP, 0);
    hit_add(460, 84 + 172, 36, 166, B_DOWN, 0);
    hit_add(388, 12, 104, 34, B_BACK, 0);
    draw_browser("sd:/roms/nes", 0, 9, 9, 7);
    save(argv[2], 300, 360);

    /* in-game menu */
    n_hits = 0;
    static const int left_ids[6] = {M_RESUME, M_SAVE, M_LOAD, M_RESET, M_ROMS, M_EXIT};
    for (int i = 0; i < 6; i++) hit_add(16, 200 + i * 38, 200, 32, left_ids[i], 0);
    static const int right_ids[6] = {M_REGION, M_PALETTE, M_RUNAHEAD, M_SMOOTH, M_CROP, M_STRETCH};
    for (int i = 0; i < 6; i++) hit_add(232, 70 + i * 50, 264, 44, right_ids[i], 1);
    draw_game_menu(7, "State saved", 1);
    save(argv[3], 420, 150);
    return 0;
}
