/* Measures emulation speed in frames per second. */
#include "../src/nes.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    static unsigned char b[1 << 20];
    size_t sz = fread(b, 1, sizeof b, f);
    fclose(f);
    nes_t *n = nes_create(48000);
    if (nes_load_rom(n, b, sz)) return 1;
    int frames = argc > 2 ? atoi(argv[2]) : 3000;
    int16_t tmp[4096];
    clock_t t0 = clock();
    for (int i = 0; i < frames; i++) {
        nes_run_frame(n);
        while (nes_audio_read(n, tmp, 4096) > 0) {}
    }
    double s = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("%d frames in %.2fs = %.0f fps (%.1fx realtime)\n", frames, s, frames / s, frames / s / 60.0988);
    return 0;
}
