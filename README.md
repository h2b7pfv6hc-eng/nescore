# nescore

Een cycle-accurate NES-emulator, gebouwd voor de Wii.

De core is in gewoon C99 geschreven en hangt niet af van een platform. De
Wii-frontend (`wii/`) gebruikt libogc en draait vanuit het Homebrew Channel.

## Waarin dit beter is dan FCE Ultra GX

FCE Ultra GX is volwassen en ondersteunt honderden mappers. Daar wint
nescore niet op. Waar nescore wel op wint:

- **Nauwkeurigheid.** De CPU, PPU en APU lopen op het niveau van losse
  klokcycli, inclusief DMA-stalls, de exacte VBlank/NMI-races, de sprite-
  overflowbug, open bus en de MMC3-IRQ-timing. Het doel is Mesen-niveau.
  Alle getoetste tests slagen (zie hieronder).
- **Gebouwd voor een trage big-endian CPU.** De PPU en APU lopen "lazy": ze
  worden alleen bijgewerkt wanneer de CPU iets kan zien, en lange stukken
  worden in één keer berekend. Dat geeft exact dezelfde uitvoer als
  stap-voor-stap emuleren (getest op beeld en geluid), met veel minder werk.
- **Run-ahead** (1-2 frames) haalt de ingebouwde inputvertraging van games
  weg, als de Wii snel genoeg is (zie Prestaties).
- **Savestates zijn één `memcpy`**: de hele machinestatus zit in één struct
  zonder pointers.
- **PAL-Wii's draaien op 60 Hz** (EuRGB60), zodat games op volle snelheid
  lopen.

## Testresultaten

| Suite | Resultaat |
|---|---|
| nestest (alle 8991 instructies, incl. unofficial opcodes, cycle-exact) | ✅ |
| instr_test-v5, instr_misc, instr_timing | ✅ alles |
| cpu_interrupts_v2, cpu_dummy_writes, cpu_dummy_reads, cpu_exec_space, cpu_reset | ✅ alles |
| ppu_vbl_nmi (incl. even/odd-timing), ppu_open_bus, ppu_read_buffer | ✅ alles |
| oam_read, oam_stress | ✅ |
| sprite_hit_tests_2005 (11), sprite_overflow_tests (5) | ✅ alles |
| apu_test (8), apu_reset (6) | ✅ alles |
| mmc3_test_2 | ✅ 5/5 voor MMC3B/C (6-MMC3_alt test de oudere MMC3A) |
| sprdma_and_dmc_dma, dma_2007_read/write, read_write_2007 | ✅ |
| dma_4016_read, double_2007_read | ❌ bekende randgevallen (DMC-DMA tijdens controller/$2007-lezen) |

Daarnaast: de core is gebouwd als **big-endian 32-bit PowerPC**-programma en
gedraaid onder qemu (`-cpu 750`, dezelfde CPU-familie als de Wii). Beeld en
geluid zijn bit-voor-bit gelijk aan de x86-versie, en de grote testsuites
slagen ook daar.

## Prestaties

Gemeten in PowerPC-instructies per NES-frame (qemu, `tools/ppc/count.sh`):

| ROM | instructies/frame |
|---|---|
| CMC80s (demo) | ~5,9 M |
| Blade Buster (MMC3) | ~6,7 M |
| sprite-demo met continu $2002-pollen | ~8,8 M |

De Wii-CPU (Broadway, 729 MHz) haalt naar schatting 9-12 M instructies per
1/60 s. Volle snelheid zou dus moeten lukken, maar dat is **nog niet op een
echte Wii getest**. Run-ahead kost het dubbele; de frontend schakelt het
vanzelf terug als het niet binnen een frame past. Het menu toont de
CPU-belasting, handig om terug te melden.

## Bouwen

**Zonder installatie:** push deze map naar GitHub. De workflow in
`.github/workflows/wii.yml` bouwt met de officiële devkitPro-container en
levert `apps/nescore/` (boot.dol, meta.xml, icon.png) als download.

**Lokaal** (met devkitPro geïnstalleerd: devkitPPC, libogc, libfat):

```
make -C wii dist
```

Testtools voor de pc:

```
make
git clone https://github.com/christopherpow/nes-test-roms
make test ROMS=nes-test-roms
```

## Installeren op de Wii

1. Kopieer `apps/nescore/` naar `sd:/apps/nescore/`.
2. Zet je eigen `.nes`-bestanden in `sd:/nes/` (of `usb:/nes/`).
3. Start nescore vanuit het Homebrew Channel.

Saves komen in `nes/saves/` (`.sav` voor batterij-RAM, `.st0` voor de
savestate).

## Besturing

| | Wii Remote (zijwaarts) | + Nunchuk | Classic / Wii U Pro | GameCube |
|---|---|---|---|---|
| D-pad | D-pad | stick of D-pad | D-pad of linkerstick | D-pad of stick |
| A | 2 | A | A of X | A |
| B | 1 | B | B of Y | B |
| Start / Select | + / − | + / − | + / − | Start / Z |
| Menu | HOME | HOME | HOME | L + R |

Snel opslaan: houd B ingedrukt en druk op +. Snel laden: houd B ingedrukt en druk op −.

In het menu: savestate opslaan/laden, reset, kleurenpalet (eigen signaalmodel of
op echte hardware gemeten paletten van FirebrandX), run-ahead (uit/1/2 frames),
scherpe of zachte schaling, overscan verbergen, beeldverhouding (8:7
pixel-accuraat of 4:3) en een andere game kiezen.

## Ondersteunde mappers

0 (NROM), 1 (MMC1), 2 (UxROM), 3 (CNROM), 4 (MMC3), 7 (AxROM), 9 (MMC2),
10 (MMC4), 11 (Color Dreams), 66 (GxROM), 71 (Camerica). Daarmee draait het
grootste deel van de officiële NES-bibliotheek.

## Nog niet aanwezig

- Meer mappers (o.a. VRC-reeks, FME-7, MMC5) en de Famicom Disk System
- Uitbreidingsgeluid (VRC6, N163, ...)
- `.zip`-ondersteuning, cheats, Zapper
- PAL-NES-timing (nu alleen NTSC)

## Opbouw

```
src/        core: cpu.c, ppu.c, apu.c, cart.c (mappers), nes.c (bus, API)
unity/      alle core-bestanden als één compilatie-eenheid (snelst)
wii/        Wii-frontend (libogc), Makefile, meta.xml, icon.png
tools/      testrunners, benchmark, PowerPC-harness voor qemu
```

De publieke API staat in `src/nes.h`: ROM laden, `nes_run_frame()`,
framebuffer (palette-indexen), audio (16-bit mono), input en savestates.
