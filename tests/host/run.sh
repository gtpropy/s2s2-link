#!/bin/sh
# Host tests of the packet framing + integrity check: ./run.sh   (needs clang)
set -e
cd "$(dirname "$0")"
ROOT=$(cd ../.. && pwd)
OUT=$(mktemp -d)
for AUTH in 1; do
  sh build_host.sh "$OUT/rt_auth" "$ROOT/cpu2/satellite/mission/protocol.c" rt_auth.c
  "$OUT/rt_auth"
done
rm -rf "$OUT"
