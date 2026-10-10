#!/bin/sh
# Assemble cpu2/common/protocol/seal_tag.S and check it under an emulated Cortex-M0 (Unicorn) against a Python
# model and the official test vector.   Needs: pip install unicorn   (use a scratch venv)
#   ./run.sh [python]
set -e
cd "$(dirname "$0")"
PY=${1:-python3}
TC=$(ls -d /Applications/ArmGNUToolchain/*/arm-none-eabi/bin 2>/dev/null | tail -1)
[ -n "$TC" ] && PATH="$TC:$PATH"
OUT=$(mktemp -d)
arm-none-eabi-gcc -mcpu=cortex-m0plus -mthumb -DAUTH_ENABLE=1 -c ../../cpu2/common/protocol/seal_tag.S -o "$OUT/seal_tag.o"
arm-none-eabi-ld -T link.ld -o "$OUT/seal.elf" "$OUT/seal_tag.o"
arm-none-eabi-objcopy -O binary "$OUT/seal.elf" "$OUT/seal.bin"
cp "$OUT/seal.elf" "$OUT/seal.bin" .
"$PY" test_seal_tag.py
rm -f seal.elf seal.bin; rm -rf "$OUT"
