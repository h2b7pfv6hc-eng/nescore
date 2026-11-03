/* Runs nestest.nes in automation mode and compares every instruction
 * (registers + cycle count) against the reference log. */
#include "../src/core.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    if (argc < 3) { fprintf(stderr, "usage: nestest rom log\n"); return 2; }
    FILE *f = fopen(argv[1], "rb");
    static uint8_t buf[1 << 20];
    size_t sz = fread(buf, 1, sizeof(buf), f);
    fclose(f);
    nes_t *n = nes_create(48000);
    if (nes_load_rom(n, buf, sz)) { puts("load failed"); return 1; }
    n->s.cpu.pc = 0xC000;
    FILE *lg = fopen(argv[2], "r");
    char line[256];
    int ln = 0;
    while (fgets(line, sizeof line, lg)) {
        ln++;
        unsigned pc, a, x, y, p, sp;
        unsigned long long cyc;
        pc = (unsigned)strtoul(line, NULL, 16);
        sscanf(strstr(line, "A:"), "A:%x X:%x Y:%x P:%x SP:%x", &a, &x, &y, &p, &sp);
        sscanf(strstr(line, "CYC:"), "CYC:%llu", &cyc);
        cpu_t *c = &n->s.cpu;
        if (c->pc != pc || c->a != a || c->x != x || c->y != y || c->p != p || c->s != sp || c->cycles != cyc) {
            printf("MISMATCH line %d\nexpected: %s", ln, line);
            printf("got:      %04X A:%02X X:%02X Y:%02X P:%02X SP:%02X CYC:%llu\n",
                   c->pc, c->a, c->x, c->y, c->p, c->s, (unsigned long long)c->cycles);
            return 1;
        }
        cpu_step(n);
    }
    printf("nestest: all %d instructions match (official + unofficial)\n", ln);
    printf("result codes $02=%02X $03=%02X\n", nes_peek(n, 2), nes_peek(n, 3));
    return 0;
}
