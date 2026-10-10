#!/bin/sh
# The satellite digipeater chain with the real framing code and Dire Wolf as the independent receiver:
#   sender packet -> Protocol_ExtractFrame -> Digi_Process -> repeat packet -> 4800 bit/s G3RUH audio -> Dire Wolf (atest)
# Needs: clang, direwolf (atest), tools/.venv (numpy).
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd); T=$(mktemp -d); PY=$ROOT/tools/.venv/bin/python
python3 "$ROOT/tools/config/s2s2_core.py" gen --quiet
COM=$ROOT/cpu2/common
clang -std=gnu11 -O1 -w -DAUTH_ENABLE=0 -I"$ROOT/cpu2/satellite/mission" -I"$COM/protocol" -I"$COM/protocol/ax25" -I"$COM/protocol/g3ruh" \
  -I"$COM/system/include" -I"$COM/digipeater" -I"$ROOT/generated" \
  "$ROOT/cpu2/satellite/mission/protocol.c" "$COM/digipeater/digipeater.c" "$COM/protocol/ax25/ax25.c" "$COM/protocol/ax25/crc16.c" \
  "$COM/protocol/g3ruh/g3ruh.c" shim_seal.c digi_chain.c -o "$T/digi_chain"

"$T/digi_chain" \
  'A1ABC>APRS,ARISS:direct alias, repeated once' \
  'K1XYZ>APRS,WIDE2-2:!4237.14N/07120.83W-wide2-2 gets traced' \
  'A1ABC>APRS,ARISS:direct alias, repeated once' \
  'B2BBB>APRS,WIDE1-1:fill-in tier is not for a satellite' \
  'C3CCC>APRS,9N2SJ-1:addressed to the satellite itself' \
  'D4DDD>APRS:heard direct, no path' \
  'E5EEE>APRS,K1FILL*,WIDE2-1:trace of the earlier hop is kept' \
  > "$T/chain.txt" 2> "$T/decisions.txt"
cat "$T/decisions.txt"

"$PY" - "$T/chain.txt" "$T/outs.wav" <<'PY'
import sys, numpy as np, wave
sys.path.insert(0, ".")
import gfsk_audio as g
pk = [bytes.fromhex(l.split()[1]) for l in open(sys.argv[1]) if l.startswith("OUT ")]
x = g.stream(pk, gap_s=0.4)
with wave.open(sys.argv[2], "wb") as w:
    w.setnchannels(1); w.setsampwidth(2); w.setframerate(g.FS); w.writeframes(x.tobytes())
print("%d repeated radio packets -> %s" % (len(pk), sys.argv[2]))
PY
echo "== Dire Wolf (4800 bit/s G3RUH) decodes the repeats:"
atest -B 4800 -g "$T/outs.wav" 2>&1 | sed 's/\x1b\[[0-9;]*[A-Za-z]//g' | grep -E "^\[[0-9]+\]|packets decoded" | sed 's/^/   /' | tee "$T/dw.txt"
OK=1
for W in 'A1ABC>APRS,9N2SJ-1*:direct alias, repeated once' \
         'K1XYZ>APRS,9N2SJ-1*,WIDE2-1:!4237.14N/07120.83W-wide2-2 gets traced' \
         'C3CCC>APRS,9N2SJ-1*:addressed to the satellite itself' \
         'E5EEE>APRS,K1FILL,9N2SJ-1*:trace of the earlier hop is kept'; do
  grep -qF -- "$W" "$T/dw.txt" || { echo "   MISSING: $W"; OK=0; }
done
N=$(grep -c "^   \[[0-9]*\]" "$T/dw.txt" || true); [ "$N" = 4 ] || { echo "   expected exactly 4 decoded frames, got $N"; OK=0; }
rm -rf "$T"
[ "$OK" = 1 ] && echo "digipeater chain vs Dire Wolf: OK" || { echo "digipeater chain vs Dire Wolf: FAILED"; exit 1; }
