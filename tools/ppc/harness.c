/*
 * Freestanding test harness for running the core as a big-endian 32-bit
 * PowerPC Linux binary under qemu-ppc (no libc needed).
 *
 *   harness hash  rom frames   -> framebuffer/audio hash (compare with x86)
 *   harness test  rom frames   -> blargg $6000 result
 *   harness bench rom frames   -> just runs (count instructions with qemu's insn plugin)
 */
#include "../../src/nes.h"
#include <stddef.h>
#include <stdint.h>

/* ---- tiny libc ---- */
static long sys3(long n, long a, long b, long c)
{
    register long r0 __asm__("r0") = n;
    register long r3 __asm__("r3") = a;
    register long r4 __asm__("r4") = b;
    register long r5 __asm__("r5") = c;
    __asm__ volatile("sc\n\tbns+ 1f\n\tneg %1,%1\n1:"
                     : "+r"(r0), "+r"(r3), "+r"(r4), "+r"(r5)
                     :
                     : "cr0", "memory", "r6", "r7", "r8", "r9", "r10", "r11", "r12");
    return r3;
}
#define SYS_exit 1
#define SYS_read 3
#define SYS_write 4
#define SYS_open 5

void *memset(void *d, int c, size_t n) { unsigned char *p = d; while (n--) *p++ = (unsigned char)c; return d; }
void *memcpy(void *d, const void *s, size_t n) { unsigned char *p = d; const unsigned char *q = s; while (n--) *p++ = *q++; return d; }
void *memmove(void *d, const void *s, size_t n)
{
    unsigned char *p = d; const unsigned char *q = s;
    if (p < q) while (n--) *p++ = *q++;
    else { p += n; q += n; while (n--) *--p = *--q; }
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++) if (*p != *q) return *p - *q;
    return 0;
}
static unsigned char heap[8 << 20];
static size_t heap_top;
void *malloc(size_t n) { n = (n + 15) & ~15u; void *p = heap + heap_top; heap_top += n; return p; }
void *calloc(size_t a, size_t b) { void *p = malloc(a * b); memset(p, 0, a * b); return p; }
void free(void *p) { (void)p; }
float powf(float x, float y) { (void)y; return x; }
float cosf(float x) { (void)x; return 1; }
float sinf(float x) { (void)x; return 0; }

uint64_t __udivdi3(uint64_t a, uint64_t b)
{
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = (r << 1) | ((a >> i) & 1);
        if (r >= b) { r -= b; q |= 1ULL << i; }
    }
    return q;
}
uint64_t __umoddi3(uint64_t a, uint64_t b) { return a - __udivdi3(a, b) * b; }
unsigned __fixunsdfsi(double d)
{
    if (d < 2147483648.0) return (unsigned)(int)d;
    return (unsigned)(int)(d - 2147483648.0) + 0x80000000u;
}

static void puts_(const char *s) { size_t n = 0; while (s[n]) n++; sys3(SYS_write, 1, (long)s, (long)n); }
static void puthex(uint64_t v, int digits)
{
    char b[17];
    for (int i = digits - 1; i >= 0; i--) { b[i] = "0123456789abcdef"[v & 15]; v >>= 4; }
    b[digits] = 0;
    puts_(b);
}
static void putdec(unsigned v) { char b[12]; int i = 11; b[i] = 0; do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v); puts_(b + i); }
static int atoi_(const char *s) { int v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0'); return v; }
static int streq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

static unsigned char rom[1 << 20];

static int main_(int argc, char **argv)
{
    if (argc < 4) { puts_("usage: harness hash|test|bench rom frames\n"); return 2; }
    long fd = sys3(SYS_open, (long)argv[2], 0, 0);
    if (fd < 0) { puts_("open failed\n"); return 2; }
    long sz = 0, r;
    while ((r = sys3(SYS_read, fd, (long)(rom + sz), (long)(sizeof(rom) - sz))) > 0) sz += r;
    int frames = atoi_(argv[3]);
    nes_t *n = nes_create(48000);
    if (nes_load_rom(n, rom, (size_t)sz)) { puts_("load failed\n"); return 2; }
    uint64_t h = 1469598103934665603ULL, ha = h;
    int16_t tmp[4096];
    int result = -1;
    for (int i = 0; i < frames; i++) {
        if (streq(argv[1], "hash"))
            nes_set_buttons(n, 0, (uint8_t)((i / 30) & 1 ? 0x08 : (i * 7 >> 3) & 0xF1));
        nes_run_frame(n);
        int c;
        while ((c = nes_audio_read(n, tmp, 4096)) > 0)
            for (int k = 0; k < c; k++) ha = (ha ^ (uint16_t)tmp[k]) * 1099511628211ULL;
        if (streq(argv[1], "hash")) {
            const uint16_t *fb = nes_framebuffer(n);
            for (int k = 0; k < 256 * 240; k++) h = (h ^ fb[k]) * 1099511628211ULL;
        }
        if (streq(argv[1], "test") && nes_peek(n, 0x6001) == 0xDE && nes_peek(n, 0x6002) == 0xB0 &&
            nes_peek(n, 0x6003) == 0x61 && nes_peek(n, 0x6000) < 0x80) {
            result = nes_peek(n, 0x6000);
            break;
        }
    }
    if (streq(argv[1], "hash")) { puthex(h, 16); puts_(" "); puthex(ha, 16); puts_("\n"); }
    if (streq(argv[1], "test")) { puts_(result == 0 ? "PASS\n" : result < 0 ? "TIMEOUT\n" : "FAIL\n"); }
    if (streq(argv[1], "bench")) { puts_("ran "); putdec((unsigned)frames); puts_(" frames\n"); }
    return 0;
}

__attribute__((used)) void cstart(long *sp)
{
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    sys3(SYS_exit, main_(argc, argv), 0, 0);
}

__asm__(".globl _start\n_start:\n\tmr 3,1\n\tclrrwi 1,1,4\n\tstwu 1,-16(1)\n\tbl cstart\n");
