/* Prints a hash of the framebuffer and audio over N frames (for A/B checks). */
#include "../src/nes.h"
#include <stdio.h>
#include <stdlib.h>
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    static unsigned char b[1 << 20];
    size_t sz = fread(b, 1, sizeof b, f);
    fclose(f);
    nes_t *n = nes_create(48000);
    if (nes_load_rom(n, b, sz)) return 1;
    int frames = argc > 2 ? atoi(argv[2]) : 600;
    unsigned long long h = 1469598103934665603ULL, ha = h;
    int16_t tmp[4096];
    for (int i = 0; i < frames; i++) {
        nes_set_buttons(n, 0, (uint8_t)((i / 30) & 1 ? 0x08 : (i * 7 >> 3) & 0xF1));
        nes_run_frame(n);
        const uint16_t *fb = nes_framebuffer(n);
        for (int k = 0; k < 256 * 240; k++) h = (h ^ fb[k]) * 1099511628211ULL;
        int c;
        while ((c = nes_audio_read(n, tmp, 4096)) > 0)
            for (int k = 0; k < c; k++) ha = (ha ^ (uint16_t)tmp[k]) * 1099511628211ULL;
    }
    printf("%016llx %016llx\n", h, ha);
    return 0;
}
