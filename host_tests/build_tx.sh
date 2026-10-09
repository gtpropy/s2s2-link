#!/bin/sh
# usage: build_tx.sh <leading_flags> <out_binary> [fw_dir]
set -e
FLAGS=$1; OUT=$2; FW=${3:-../Satellite_M0+/Com_sat}; NOCAP=${NOCAP:-0}
T=$(mktemp -d)
cp -R "$FW/Mission" "$T/Mission"
sed -i '' "s/^#define PROTOCOL_LEADING_FLAG_COUNT .*/#define PROTOCOL_LEADING_FLAG_COUNT $FLAGS/" "$T/Mission/config.h"
if [ "$NOCAP" = 1 ]; then python3 - "$T/Mission/protocol.c" <<'PY'
import sys,re
f=sys.argv[1]; s=open(f).read()
s=re.sub(r"if \(payloadLength > 100\)\s*\{\s*leadingFlags = 12;\s*\}","",s)
open(f,"w").write(s)
PY
fi
clang -std=gnu11 -O1 -w -I"$T/Mission" -I"$FW/protocol" -I"$FW/protocol/ax25" -I"$FW/protocol/g3ruh" -I"$FW/Inc" \
  "$T/Mission/protocol.c" "$T/Mission/seal_wrap.c" shim_seal.c txpkt.c \
  "$FW/protocol/ax25/ax25.c" "$FW/protocol/ax25/crc16.c" "$FW/protocol/g3ruh/g3ruh.c" -o "$OUT"
rm -rf "$T"
