/* Single translation unit build: lets the compiler inline the PPU/APU
 * into the CPU's per-cycle path. Use this file instead of the five
 * separate sources for the fastest result (e.g. on the Wii). */
#include "../src/nes.c"
#include "../src/cart.c"
#include "../src/apu.c"
#include "../src/ppu.c"
#include "../src/cpu.c"
