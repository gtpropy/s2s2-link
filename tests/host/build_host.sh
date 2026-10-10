#!/bin/sh
# usage: build_host.sh <out> <protocol.c> <main.c> [fw_dir]   -- host build of the real framing code (+ integrity check)
set -e
ROOT=$(cd "$(dirname "$0")/../.." && pwd); COM=$ROOT/cpu2/common
OUT=$1; PROTO=$2; MAIN=$3; FW=${4:-$ROOT/cpu2/satellite}
python3 "$ROOT/tools/config/s2s2_core.py" gen --quiet
T=$(mktemp -d); cp -R "$FW/mission" "$T/mission"; cp "$PROTO" "$T/mission/protocol.c"
clang -std=gnu11 -O1 -w -I"$T/mission" -I"$COM/protocol" -I"$COM/protocol/ax25" -I"$COM/protocol/g3ruh" \
  -I"$COM/system/include" -I"$ROOT/generated" \
  "$T/mission/protocol.c" "$COM/protocol/seal_wrap.c" shim_seal.c "$MAIN" \
  "$COM/protocol/ax25/ax25.c" "$COM/protocol/ax25/crc16.c" "$COM/protocol/g3ruh/g3ruh.c" -o "$OUT"
rm -rf "$T"
