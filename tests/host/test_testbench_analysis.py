#!/usr/bin/env python3
"""Pure analysis helpers of tools/testbench/tests_extra.py (no hardware)."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "testbench"))
from tb import tests_extra as T                                                     # noqa: E402

bad = 0


def check(c, msg):
    global bad
    if not c:
        print("FAIL", msg)
        bad += 1


def crc16(d, c=0xFFFF):
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


SYNC = b"\xC5" * 8 + b"\xA7"


def frame(cmd, seq=0, total=0, data=b""):
    body = bytes([cmd]) + struct.pack("<HH", seq, total) + bytes([len(data)]) + data
    return SYNC + body + struct.pack("<H", crc16(body))


# beacon period
t = [1000 + 90 * i for i in range(8)]
ok, st, why = T.check_beacon_period(t, 90, 4, 700)
check(ok and st["mean"] == 90 and st["std"] == 0 and st["expected_n"] == 7, "perfect beacons")
t2 = t[:3] + t[4:]                                                         # one beacon missed: a 180 s gap
ok, st, why = T.check_beacon_period(t2, 90, 4, 700)
check(not ok and "missed" in why, "missed decode detected: " + why)
ok, st, why = T.check_beacon_period([1000], 90, 4, 700)
check(not ok and "only 1" in why, "too few beacons")
ok, st, why = T.check_beacon_period([1000 + 90 * i + (3 if i % 2 else 0) for i in range(6)], 90, 4, 600)
check(ok, "jitter inside the tolerance is fine")

# listen window
probes = [(o, 40 <= o < 90) for o in range(0, 90, 10)] * 2
w = T.estimate_window(probes)
check(w == (40, 90, 50), f"window {w}")
probes2 = [(o, 40 <= o < 90 and not (o == 60 and k == 1)) for k in range(2) for o in range(0, 90, 10)]   # 60 s failed in the 2nd sweep
w2 = T.estimate_window(probes2)
check(w2 is not None and w2[2] == 20, f"flaky offset splits the window into runs of 20 s: {w2}")
check(T.estimate_window([(o, False) for o in range(0, 90, 10)]) is None, "no window")

# bytes
check(T.first_mismatch(b"abc", b"abc") is None and T.first_mismatch(b"abc", b"abd") == 2 and T.first_mismatch(b"ab", b"abc") == 2, "first_mismatch")
check(T.count_diff(b"abcd", b"abxd") == 1 and T.count_diff(b"ab", b"abcd") == 2, "count_diff")
check(len(set(T.pattern_chunk(5))) > 100, "pattern chunk is not a repeated byte")
check(T.pattern_chunk(5) == T.pattern_chunk(5) and T.pattern_chunk(5) != T.pattern_chunk(6) and len(T.pattern_chunk(1)) == 128, "pattern chunk")

# fuzz frames really are bad
cases = T.fuzz_cases(frame, crc16, SYNC)
by = {c[0]: c for c in cases}
f = by["bad CRC"][1]
check(crc16(f[len(SYNC):-2]) != struct.unpack("<H", f[-2:])[0], "bad CRC frame has a wrong CRC")
f = by["one flipped bit"][1]
check(crc16(f[len(SYNC):-2]) != struct.unpack("<H", f[-2:])[0], "flipped-bit frame has a wrong CRC")
f = by["declared length longer than the data"][1]
check(f[len(SYNC) + 5] == 200 and len(f) < len(SYNC) + 6 + 200, "declared length longer than the data")
check(by["sync too short"][1] == frame(2, 0x1111)[1:] and not by["sync too short"][2], "short sync is informational")
check(all(c[2] for c in cases if c[0] != "sync too short"), "all other cases must be ignored")
# LittleFS proof helpers
import random as _r
_g = _r.Random(3)
pic = bytes(_g.getrandbits(8) for _ in range(1000))
flash = bytearray(b"\xff" * (4 * 2048))
flash[2048 + 8:2048 + 16] = b"littlefs"
flash[2048 + 100:2048 + 100 + len(pic)] = pic                                   # contiguous copy of the file
check(T.lfs_superblocks(bytes(flash)) == [1], "superblock found in block 1")
check(T.lfs_superblocks(b"\xff" * 4096) == [], "no superblock in erased flash")
f, t, miss = T.chunk_hits(bytes(flash), pic)
check((f, t, miss) == (8, 8, []), f"all 8 chunks found: {(f, t, miss)}")
flash2 = bytearray(flash)
flash2[2048 + 100 + 256:2048 + 100 + 384] = b"\xff" * 128                         # chunk 3 destroyed
f, t, miss = T.chunk_hits(bytes(flash2), pic)
check(f == 7 and miss == [3], f"destroyed chunk is reported: {(f, t, miss)}")
check(T.chunk_hits(b"\xff" * 100, b"") == (0, 0, []), "empty file")
print("testbench analysis: FAILED" if bad else f"testbench analysis OK ({len(cases)} fuzz frames, beacon period, window, byte compare)")
sys.exit(1 if bad else 0)
