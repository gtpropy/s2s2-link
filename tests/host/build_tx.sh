#!/bin/sh
# usage: build_tx.sh <leading_flags> <out_binary> [fw_dir]
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd); COM=$ROOT/cpu2/common
FLAGS=$1; OUT=$2; FW=${3:-$ROOT/cpu2/satellite}; NOCAP=${NOCAP:-0}
python3 "$ROOT/tools/config/s2s2_core.py" gen --quiet
T=$(mktemp -d)
cp -R "$FW/mission" "$T/mission"
sed -i '' "s/^#define PROTOCOL_LEADING_FLAG_COUNT .*/#define PROTOCOL_LEADING_FLAG_COUNT $FLAGS/" "$T/mission/config.h"
clang -std=gnu11 -O1 -w -I"$T/mission" -I"$COM/protocol" -I"$COM/protocol/ax25" -I"$COM/protocol/g3ruh" \
  -I"$COM/system/include" -I"$ROOT/generated" \
  "$T/mission/protocol.c" "$COM/protocol/seal_wrap.c" shim_seal.c txpkt.c \
  "$COM/protocol/ax25/ax25.c" "$COM/protocol/ax25/crc16.c" "$COM/protocol/g3ruh/g3ruh.c" -o "$OUT"
rm -rf "$T"
