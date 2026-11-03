#!/bin/bash
# Builds the big-endian PowerPC harness and reports instructions per frame.
# Needs: clang (ppc32 target), ld.lld, qemu-ppc + libinsn.so plugin.
set -e
cd "$(dirname "$0")/../.."
INC=${PPCINC:?set PPCINC to the stub header dir}
Q=${QEMU:-qemu-ppc}; P=${INSN_PLUGIN:?set INSN_PLUGIN}
clang -target powerpc-unknown-linux-gnu -mcpu=750 -O2 -ffp-contract=off -ffreestanding -fno-builtin -nostdlib -static \
  -fuse-ld=lld -nostdinc -isystem "$(clang -print-resource-dir)/include" -I "$INC" \
  -o build/harness_ppc tools/ppc/harness.c unity/nescore.c
for r in "$@"; do
  a=$($Q -cpu 750 -plugin $P -d plugin build/harness_ppc bench "$r" 60 2>&1 | grep -o "insns: [0-9]*" | tail -1 | grep -o "[0-9]*")
  b=$($Q -cpu 750 -plugin $P -d plugin build/harness_ppc bench "$r" 180 2>&1 | grep -o "insns: [0-9]*" | tail -1 | grep -o "[0-9]*")
  echo "$(basename "$r"): $(( (b - a) / 120 )) PPC instructions/frame"
done
