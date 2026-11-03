#!/bin/bash
# Runs the test ROMs that report their result in memory ($6000) and prints
# a pass/fail list. Usage: tools/suite.sh /path/to/nes-test-roms
#
# Tests that only report on screen (sprite_hit_tests_2005, sprite_overflow_tests,
# cpu_dummy_reads, dmc_dma_during_read4) are checked with:
#   build/runtest rom.nes 600 out.ppm
T=${1:?usage: suite.sh /path/to/nes-test-roms}
cd "$(dirname "$0")/.."
pass=0; fail=0
run() {
  out=$(timeout 60 ./build/runtest "$1" ${2:-4000} 2>&1); rc=$?
  name=${1#$T/}
  case "$name" in
    */6-MMC3_alt.nes) echo "skip $name (tests the older MMC3A chip; nescore emulates MMC3B/C)"; return;;
  esac
  if [ $rc -eq 0 ]; then pass=$((pass+1)); echo "PASS $name";
  else fail=$((fail+1)); echo "FAIL $name :: $(echo "$out" | tr '\n' ' ' | cut -c1-150)"; fi
}
for f in $T/instr_test-v5/rom_singles/*.nes $T/instr_misc/rom_singles/*.nes $T/instr_timing/rom_singles/*.nes \
         $T/cpu_dummy_writes/*.nes $T/cpu_exec_space/*.nes $T/cpu_interrupts_v2/rom_singles/*.nes \
         $T/cpu_reset/*.nes $T/ppu_vbl_nmi/rom_singles/*.nes $T/ppu_open_bus/*.nes $T/ppu_read_buffer/*.nes \
         $T/oam_read/*.nes $T/oam_stress/*.nes $T/apu_test/rom_singles/*.nes $T/apu_reset/*.nes \
         $T/mmc3_test_2/rom_singles/*.nes $T/sprdma_and_dmc_dma/*.nes; do
  [ -f "$f" ] && run "$f"
done
echo "== $pass passed, $fail failed =="
[ $fail -eq 0 ]
