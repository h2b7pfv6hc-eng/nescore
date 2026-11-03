/*
 * Headless test-ROM runner.
 * blargg-style ROMs report through $6000 (status) / $6004 (text).
 * Optionally dumps the last frame as a PPM for screen-only tests.
 *
 *   runtest rom.nes [max_frames] [out.ppm] [hold_buttons_hex]
 * Exit code 0 = pass, 1 = fail, 2 = no result (timeout).
 */
#include "../src/core.h"
#include <stdio.h>
#include <stdlib.h>

static void dump_ppm(nes_t *n, const char *path)
{
    uint32_t pal[512];
    nes_palette_rgb(pal);
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n256 240\n255\n");
    for (int i = 0; i < 256 * 240; i++) {
        uint32_t c = pal[n->fb[i] & 511];
        fputc(c >> 16, f); fputc((c >> 8) & 255, f); fputc(c & 255, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 3;
    int max_frames = argc > 2 ? atoi(argv[2]) : 3000;
    const char *ppm = argc > 3 ? argv[3] : NULL;
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("open"); return 3; }
    static uint8_t buf[4 << 20];
    size_t sz = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    nes_t *n = nes_create(48000);
    int r = nes_load_rom(n, buf, sz);
    if (r) { printf("load: %s\n", nes_error_string(r)); return 3; }
    if (argc > 4) nes_set_buttons(n, 0, (uint8_t)strtol(argv[4], NULL, 16));

    int reset_at = -1, result = -1, prev_st = -1;
    for (int fr = 0; fr < max_frames; fr++) {
        nes_run_frame(n);
        int16_t tmp[4096];
        while (nes_audio_read(n, tmp, 4096) > 0) {}
        int sig = nes_peek(n, 0x6001) == 0xDE && nes_peek(n, 0x6002) == 0xB0 && nes_peek(n, 0x6003) == 0x61;
        if (!sig) continue;
        uint8_t st = nes_peek(n, 0x6000);
        if (st == 0x81 && prev_st != 0x81) reset_at = fr + 10;
        prev_st = st;
        if (reset_at >= 0 && fr == reset_at) { nes_reset(n); reset_at = -1; continue; }
        if (st < 0x80) { result = st; break; }
    }
    if (ppm) dump_ppm(n, ppm);
    char text[1024] = {0};
    for (int i = 0; i < 1023; i++) {
        uint8_t c = nes_peek(n, (uint16_t)(0x6004 + i));
        if (!c) break;
        text[i] = (char)c;
    }
    if (result < 0) {
        printf("TIMEOUT %s\n", text);
        return 2;
    }
    printf("%s (code %d)\n%s\n", result == 0 ? "PASS" : "FAIL", result, text);
    return result == 0 ? 0 : 1;
}
