#!/bin/sh
# Host tests: ./run.sh   (needs clang; the Dire Wolf cross-checks also need direwolf's atest + gen_packets and tools/.venv)
#   rt_auth         packet framing + integrity check round trip, every flipped bit rejected, replay
#   test_digipeater digipeater rules (New-N paths, loop, duplicates, rate limits, round trips)
#   test_saf        store-and-forward memory: priority by hop budget, forward interval, TTL, eviction, journal bookkeeping, hop-limited relaying across 3 nodes
#   digi_chain      satellite digipeater chain with the real framing code, repeats decoded by Dire Wolf at 4800 bit/s G3RUH
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
COM=$ROOT/cpu2/common
OUT=$(mktemp -d)
sh build_host.sh "$OUT/rt_auth" "$ROOT/cpu2/satellite/mission/protocol.c" rt_auth.c
"$OUT/rt_auth"
clang -std=gnu11 -O1 -Wall -Wextra -I"$COM/digipeater" "$COM/digipeater/digipeater.c" test_digipeater.c -o "$OUT/test_digipeater"
"$OUT/test_digipeater"
clang -std=gnu11 -O1 -Wall -Wextra -I"$COM/digipeater" "$COM/digipeater/digipeater.c" "$COM/digipeater/saf.c" test_saf.c -o "$OUT/test_saf"
"$OUT/test_saf"
if command -v atest >/dev/null 2>&1; then
  [ -x "$ROOT/tools/.venv/bin/python" ] && sh digi_chain.sh || echo "digi_chain skipped: tools/.venv missing"
else
  echo "Dire Wolf cross-check skipped: atest not installed"
fi
rm -rf "$OUT"
