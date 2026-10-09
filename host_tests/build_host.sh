#!/bin/sh
# usage: build_host.sh <out> <protocol.c> <main.c> [fw_dir]   -- host build of the real framing code
set -e
OUT=$1; PROTO=$2; MAIN=$3; FW=${4:-../Satellite_M0+/Com_sat}
T=$(mktemp -d); cp -R "$FW/Mission" "$T/Mission"; cp "$PROTO" "$T/Mission/protocol.c"
clang -std=gnu11 -O1 -w -I"$T/Mission" -I"$FW/protocol" -I"$FW/protocol/ax25" -I"$FW/protocol/g3ruh" -I"$FW/Inc" \
  "$T/Mission/protocol.c" "$T/Mission/seal_wrap.c" shim_seal.c "$MAIN" \
  "$FW/protocol/ax25/ax25.c" "$FW/protocol/ax25/crc16.c" "$FW/protocol/g3ruh/g3ruh.c" -o "$OUT"
rm -rf "$T"
