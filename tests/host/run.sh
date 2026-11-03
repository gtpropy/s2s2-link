#!/bin/sh
# Host tests: ./run.sh   (needs clang; the Dire Wolf cross-checks also need direwolf's atest + gen_packets and tools/.venv)
#   rt_auth         packet framing + integrity check round trip, every flipped bit rejected, replay
#   test_digipeater digipeater rules (New-N paths, loop, duplicates, rate limits, round trips)
#   test_saf        store-and-forward memory: priority by hop budget, forward interval, TTL, eviction, journal bookkeeping, hop-limited relaying across 3 nodes
#   test_imagestore camera fragments -> picture (background image builder), shuffled / duplicated / restarted
#   test_testbench_analysis  beacon period / listen window / fuzz frame / byte-compare helpers of the testbench tests
#   test_testbench_features  trends / report diff, scenario validation + engine, log store search / regex / CSV, alerts
#   test_testbench_cubus  CUBUS DEBUG section of the testbench: g_satRx / GPIO / ring decoders, diagnosis per stage, UART stage counts, test gating
#   test_board_guard flash lock: a Nucleo / GS / NuttX image is refused on a CUBUS board (stub openocd, no hardware)
#   test_mission_control  TLE checking + browser orbit maths (satellite.js in node) of Mission Control
#   test_attitude   attitude cube math of the web tools (node)
#   test_cubus_pins CUBUS pin ownership: only the radio pins may be touched (source scan + a CUBUS image build)
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
[ -x "$ROOT/tools/.venv/bin/python" ] && "$ROOT/tools/.venv/bin/python" test_imagestore.py || python3 test_imagestore.py
python3 test_testbench_analysis.py
python3 test_testbench_features.py
python3 test_testbench_cubus.py
python3 test_cubus_pins.py
python3 test_board_guard.py
python3 test_packet_format.py
python3 test_gs_store.py
if command -v node >/dev/null 2>&1; then python3 test_mission_control.py; else echo "mission control test skipped: node not installed"; fi
if command -v node >/dev/null 2>&1; then node test_attitude.js; else echo "attitude test skipped: node not installed"; fi
if command -v atest >/dev/null 2>&1; then
  [ -x "$ROOT/tools/.venv/bin/python" ] && sh digi_chain.sh || echo "digi_chain skipped: tools/.venv missing"
else
  echo "Dire Wolf cross-check skipped: atest not installed"
fi
rm -rf "$OUT"
