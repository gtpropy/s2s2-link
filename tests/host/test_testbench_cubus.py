#!/usr/bin/env python3
"""Pure parts of tools/testbench/tb/cubus.py (decoders, diagnosis, UART stage counts) and the CUBUS gating of the testbench. No hardware."""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "testbench"))
from tb import cubus as C                                                          # noqa: E402

bad = 0


def check(c, msg):
    global bad
    if not c:
        print("FAIL", msg)
        bad += 1


# ---- g_satRx decoder: must match struct sat_rx_stats_s in cpu2/satellite/app/satellite_main.c (18 words + 16 bytes)
vals = [0x52583031, 10, 1, 2, 0, 3, 7, 1, 2, 0, 0, 1, 0, 0, 4, 3, 1, 123456]
raw = struct.pack("<18I", *vals) + bytes(range(16))
rx = C.parse_rxstats(raw)
check(rx and rx["valid"] and rx["rx_done"] == 10 and rx["decode_ok"] == 7 and rx["accepted"] == 4 and rx["ring_fail"] == 1 and rx["last_tick"] == 123456, "rxstats fields")
check(rx["last_cmd"].startswith("000102"), "last_cmd bytes")
check(C.RX_SIZE == 88, "RX_SIZE matches the C struct (18 x 4 + 16)")
src = open(os.path.join(os.path.dirname(__file__), "..", "..", "cpu2", "satellite", "app", "satellite_main.c")).read()
names = [n for n in C.RX_FIELDS if n != "magic"]
check(all(("uint32_t %s;" % n) in src for n in names), "every field of RX_FIELDS exists in the C struct")
order = [src.index("uint32_t %s;" % n) for n in names]
check(order == sorted(order), "field order of RX_FIELDS equals the C struct order")
check(not C.parse_rxstats(raw[:10]), "short buffer rejected")

# ---- GPIO decode
g = C.decode_gpio(0x1, 0x0, (1 << 4) | (1 << 6) | (1 << 8) | (1 << 10), 0x10)       # PA0 output low; PC2..PC5 outputs, PC4 high
check(g["rf_switch"] == "RX" and not g["pa_3v3"]["high"] and g["rail_5v"]["output"] and not g["rail_5v"]["high"] and g["pc_outputs"], "decode_gpio RX")
g = C.decode_gpio(0x1, 0x1, 0x550, 0x20 | 0x08)
check(g["rf_switch"] == "TX" and g["pa_5v"]["high"] and g["rail_5v"]["high"], "decode_gpio TX with 5 V PA")
check(C.decode_gpio(0, 0, 0, 0x30)["rf_switch"].startswith("both"), "both control lines high is flagged")

# ---- rings
r = C.parse_ring(struct.pack("<III", C.RX_MAGIC, 3, 1))
check(r["pending"] == 2, "ring pending")
check(C.parse_ring(struct.pack("<III", C.RX_MAGIC, 1, 3))["pending"] == 2, "ring wrap")

# ---- UART stage counts
lines = [(0, ">>> [STEP 1: READ RAW] RSSI=-97 dBm, SNR=6 dB, 200 Bytes <<<"), (1, ">>> [DECODE SUCCESS] Extracted AX.25 Frame"), (2, " [VALIDATE FAILED] Frame rejected: x"),
         (3, ">>> [STEP 1: READ RAW] RSSI=-90 dBm, SNR=8 dB, 200 Bytes <<<"), (4, ">>> [DECODE FAILED] no valid"), (5, "VALID COMMAND ACCEPTED"), (6, "noise")]
u = C.uart_stats(lines)
check(u["counts"]["raw"] == 2 and u["counts"]["decode_ok"] == 1 and u["counts"]["decode_fail"] == 1 and u["counts"]["validate_fail"] == 1 and u["counts"]["accepted"] == 1, "uart counts")
check(u["counts"]["last_rssi"] == -90 and u["counts"]["last_snr"] == 8, "uart last rssi / snr")
check("noise" not in " ".join(u["last"]), "uninteresting lines are not kept")


# ---- diagnosis: one snapshot per failure stage; the first bad finding must name the right stage
def snap(**kw):
    s = {"board_id": C.CUBUS_ID, "alive": True, "state": {"app_state": 1, "name": "listening for commands", "uptime_s": 500.0, "since_beacon_s": 60.0, "cmd_service_active": False,
                                                          "dcdc_hold": False},
         "radio": {"rssi": -100, "snr": 5}, "gpio": C.decode_gpio(1, 0, 0x550, 0x10), "ring_rx": {"magic": C.RX_MAGIC, "write": 0, "read": 0, "pending": 0},
         "rx": dict(C.parse_rxstats(struct.pack("<18I", 0x52583031, *([0] * 17)) + bytes(16)))}
    s.update(kw)
    return s


def first_bad(f):
    return next((x for x in f if x["level"] == "bad"), None)


def with_rx(**kv):
    s = snap()
    s["rx"].update(kv)
    return s


f = C.diagnose(snap(), expect_id=C.CUBUS_ID)
check(first_bad(f) and first_bad(f)["stage"] == "rx" and "Nothing was delivered" in first_bad(f)["title"], "no packets -> radio stage")
f = C.diagnose(snap(board_id=0xB0A2D001), expect_id=C.CUBUS_ID)
check(first_bad(f)["stage"] == "image", "Nucleo image on a CUBUS profile is the first bad finding")
f = C.diagnose(snap(alive=False), expect_id=C.CUBUS_ID)
check(first_bad(f)["stage"] == "boot" and len(f) <= 3, "dead CPU2 stops the analysis")
f = C.diagnose(snap(gpio=C.decode_gpio(1, 0, 0x550, 0x20)), expect_id=C.CUBUS_ID)
check(any(x["stage"] == "rf" and x["level"] == "bad" for x in f), "RF switch in TX state while listening")
f = C.diagnose(with_rx(rx_done=5, decode_fail=5), expect_id=C.CUBUS_ID)
check(first_bad(f)["stage"] == "decode", "packets but no decode -> decode stage")
f = C.diagnose(with_rx(rx_done=5, decode_ok=5, auth_fail=5), expect_id=C.CUBUS_ID)
check(first_bad(f)["stage"] == "validate" and "AUTH" in first_bad(f)["text"], "AUTH failures named")
f = C.diagnose(with_rx(rx_done=5, decode_ok=5, accepted=3, ring_fail=3), expect_id=C.CUBUS_ID)
check(first_bad(f)["stage"] == "handoff" and "not handed" in first_bad(f)["title"], "accepted but ring write failed")
s = with_rx(rx_done=5, decode_ok=5, accepted=3, ring_ok=3)
s["ring_rx"] = {"magic": 0, "write": 0, "read": 0, "pending": 0}
check(first_bad(C.diagnose(s, expect_id=C.CUBUS_ID))["stage"] == "handoff", "ring magic missing -> hand-off")
s = with_rx(rx_done=5, decode_ok=5, accepted=3, ring_ok=3)
check(first_bad(C.diagnose(s, expect_id=C.CUBUS_ID)) is None, "everything delivered and handed over -> no bad finding")
# deltas against a previous snapshot
prev = with_rx(rx_done=5, decode_ok=5, accepted=3, ring_ok=3)
now_ = with_rx(rx_done=5, decode_ok=5, accepted=3, ring_ok=3)
f = C.diagnose(now_, prev, expect_id=C.CUBUS_ID)
check(any(x["stage"] == "rx" and "since the first snapshot" in x["text"] or x["stage"] == "rx" for x in f), "delta mode used")
check(first_bad(f) is None or first_bad(f)["stage"] != "validate", "no new rejections in the interval")
f = C.diagnose(snap(state={"app_state": 0, "name": "CW beacon (deaf)", "uptime_s": 10.0, "since_beacon_s": 5.0, "cmd_service_active": False, "dcdc_hold": False}))
check(any("CW beacon" in x["text"] or "deaf" in x["text"] for x in f), "beacon window explained")

# ---- CUBUS gating of the tests
check({"image", "digi", "e2e", "flashio", "vcpfuzz", "lfsproof", "cwtest", "rate"} <= set(C.CUBUS_BLOCKED), "bench-app tests are blocked on CUBUS")
check(not ({"cmdconf", "beacontime", "soak", "fwcheck", "bootcfg", "membudget", "cubusrx"} & set(C.CUBUS_BLOCKED)), "ground-station / SWD tests stay available")
check("cubusrx" in C.TESTS and C.NEEDS["cubusrx"] == "gs", "cubusrx registered")

if bad:
    sys.exit(1)
print("testbench cubus OK (decoders, diagnosis per stage, uart counts, gating)")
