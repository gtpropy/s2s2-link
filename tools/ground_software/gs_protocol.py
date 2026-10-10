# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-09-01
"""
gs_protocol.py -- pure-Python (no Qt, no serial) parser for gs_main.c's
real UART output, plus beacon1_t/beacon2_t telemetry struct decoding.
Shared by response_analyzer.py (GUI) and mission_control_api.py (web) --
one source of truth for the line formats instead of two drifting copies.

Line formats mirror satellite/gs_main.c exactly (see the architecture
plan / investigation notes this session): RX ACK, the "--- LABEL ---"
telemetry block (fields + hex dump + ASCII payload), FRAME REJECTED,
SESSION STATS, TX COMMAND / COMMAND SENT|TIMED OUT, GAP REPORT.

beacon1_t/beacon2_t layouts are a byte-for-byte port of
core/Src/s2s2_commands.c's beacon1_pack()/beacon2_pack() (big-endian,
exact field order) -- verified against that source, not guessed.

HONESTY NOTE (read before trusting these values for anything): as of
this firmware, Send_Beacon1()/Send_Beacon2() (ipcc_m0plus_main.c) fill
beacon1_t/beacon2_t with memset(0) and only set tim_day/tim_hour (from
real uptime) on beacon1 -- no sensor hardware is wired up yet. Every
other field decoded here is a real wire value, but that value is
currently always 0 (placeholder), not a sensor reading. PLACEHOLDER_
FIELDS below flags exactly which ones, so callers can label them
honestly instead of presenting zeros as real telemetry.
"""
from __future__ import annotations

import re
import struct
import time

# =====================================================================
# gs_main.c line formats
# =====================================================================

RE_ACK = re.compile(
    r"RX ACK: status=0x([0-9A-Fa-f]{2}) \(([^)]*)\) data_id=0x([0-9A-Fa-f]{2}) acked_seq=(\d+)")
RE_BLOCK_HEADER = re.compile(r"^\s*---\s*(.+?)\s*---\s*$")
RE_FIELDS1 = re.compile(
    r"version=(\d+) type=(\d+) src=(\d+) dst=(\d+) seq=(\d+) session=0x([0-9A-Fa-f]{4}) flags=0x([0-9A-Fa-f]{2})")
RE_FIELDS2 = re.compile(
    r"payload_len=(\d+) rs_bytes_corrected=(-?\d+) rssi=(-?\d+) dBm snr=(-?\d+) dB")
RE_HEXLINE = re.compile(r"^([0-9A-Fa-f]{2})(\s[0-9A-Fa-f]{2})*\s*$")
RE_PAYLOAD_TEXT = re.compile(r'Payload \(text\):\s*"(.*)"$')
RE_REJECTED = re.compile(r"FRAME REJECTED.*?rssi=(-?\d+)\s*snr=(-?\d+)")
RE_STATS = re.compile(r"SESSION STATS: (\d+) OK \| (\d+) bad \| (\d+) misaddressed")
RE_TX_COMMAND = re.compile(r"TX COMMAND 0x([0-9A-Fa-f]{2}) \(seq=(\d+)\)")
RE_TX_RESULT = re.compile(r"COMMAND (SENT|TIMED OUT) - SWITCHING TO RX")
RE_GAP_REPORT = re.compile(r"GAP REPORT: (\d+) missing seq number")
RE_GAP_NONE = re.compile(r"no gaps - session complete")

TYPE_NAMES = {0: "TELEMETRY", 1: "TELECOMMAND", 2: "ACK", 3: "GAP_REPORT", 4: "RAW"}
DATA_ID_BEACON1 = 0x02
DATA_ID_BEACON2 = 0x03
DATA_ID_NAMES = {
    0x01: "HOUSEKEEPING", 0x02: "BEACON1", 0x03: "BEACON2", 0x04: "ADCS",
    0x05: "CAMERA_RGB", 0x06: "CAMERA_NIR", 0x07: "NDWI", 0x08: "EPDM",
    0x09: "DIGIPEATER", 0x0A: "SAT_LOG", 0xFF: "GENERAL",
}
ACK_STATUS_NAMES = {
    0x00: "accepted", 0x01: "completed", 0x02: "packet error", 0x03: "CRC/checksum error",
    0x04: "incomplete packet", 0x05: "data missing", 0x06: "data unavailable",
    0x07: "busy", 0x08: "mission inactive", 0x09: "invalid command",
    0x0A: "memory/file error", 0x0B: "transmission failed", 0x0C: "auth failed",
}

# =====================================================================
# beacon1_t / beacon2_t -- byte-for-byte port of s2s2_commands.c's
# beacon1_pack()/beacon2_pack() (big-endian, order verified against that
# source's *p++ = ... / put_u16(p, ...) sequence).
# =====================================================================

BEACON1_FIELDS = [
    "type", "tim_day", "tim_hour", "bat_v", "bat_i", "bat_temp", "raw_v", "raw_i",
    "sol_tot_v", "sol_tot_i", "unreg_i", "main_3v3_i", "msn_3v3_i", "v5_i", "adc_3v32_i",
    "temp1", "temp2", "temp3", "temp4", "temp5", "bpb_t", "ant_p_t", "obc_com_t",
    "system_stat", "mcu_stat", "adc_stat",
]
BEACON1_STRUCT = struct.Struct(">" + "B" + "H" + "B" + "H" + "H" + "B" * 3 + "H" + "B" * 17)
assert BEACON1_STRUCT.size == 30 and len(BEACON1_FIELDS) == 26

BEACON2_FIELDS = [
    "tim_day", "sol_p1_v", "sol_p2_v", "sol_p3_v", "sol_p4_v", "sol_p5_v",
    "sol_p1_i", "sol_p2_i", "sol_p3_i", "sol_p4_i", "sol_p5_i",
    "gyro_x", "gyro_y", "gyro_z", "mag_x", "mag_y", "mag_z", "power_stat",
]
BEACON2_STRUCT = struct.Struct(">" + "H" + "B" * 10 + "H" * 6 + "B")
assert BEACON2_STRUCT.size == 25 and len(BEACON2_FIELDS) == 18

# Fields that are currently always 0 -- memset placeholder, no sensor
# wired up (see Send_Beacon1()/Send_Beacon2(), ipcc_m0plus_main.c).
# Everything NOT listed here is a genuinely real, currently-populated
# value (right now: only beacon1's tim_day/tim_hour and its own
# constant `type`... `type` is also memset(0), so listed too).
BEACON1_PLACEHOLDER_FIELDS = set(BEACON1_FIELDS) - {"tim_day", "tim_hour"}
BEACON2_PLACEHOLDER_FIELDS = set(BEACON2_FIELDS)  # entirely placeholder, no real field at all


def decode_beacon1(payload: bytes) -> dict:
    if len(payload) < BEACON1_STRUCT.size:
        return {}
    values = BEACON1_STRUCT.unpack_from(payload)
    return dict(zip(BEACON1_FIELDS, values))


def decode_beacon2(payload: bytes) -> dict:
    if len(payload) < BEACON2_STRUCT.size:
        return {}
    values = BEACON2_STRUCT.unpack_from(payload)
    return dict(zip(BEACON2_FIELDS, values))


# =====================================================================
# Stateful line-by-line parser -- feed() one line at a time, get back a
# list of zero or more event dicts (usually 0 or 1; the telemetry block
# spans several lines and only yields once complete).
# =====================================================================

class GsProtocolParser:
    def __init__(self):
        self._pending = None  # dict accumulating a "--- LABEL ---" block

    def feed(self, line: str) -> list:
        stripped = line.rstrip()
        if not stripped:
            return []
        events = []

        m = RE_ACK.match(stripped)
        if m:
            status_hex, status_name, data_id_hex, acked_seq = m.groups()
            events.append({
                "kind": "ACK", "ts": time.time(), "raw_line": line,
                "status": int(status_hex, 16), "status_name": status_name,
                "data_id": int(data_id_hex, 16), "seq": int(acked_seq),
            })
            return events

        m = RE_BLOCK_HEADER.match(stripped)
        if m:
            self._pending = {"label": m.group(1), "hex_bytes": bytearray()}
            return events

        if self._pending is not None:
            m = RE_FIELDS1.search(stripped)
            if m:
                (self._pending["version"], self._pending["type"], self._pending["src"],
                 self._pending["dst"], self._pending["seq"], session_hex,
                 flags_hex) = m.groups()
                self._pending["session"] = int(session_hex, 16)
                self._pending["flags"] = int(flags_hex, 16)
                return events
            m = RE_FIELDS2.search(stripped)
            if m:
                (self._pending["payload_len"], self._pending["rs_corrected"],
                 self._pending["rssi"], self._pending["snr"]) = (int(x) for x in m.groups())
                return events
            if RE_HEXLINE.match(stripped):
                self._pending["hex_bytes"].extend(bytes.fromhex(stripped.replace(" ", "")))
                return events
            m = RE_PAYLOAD_TEXT.search(stripped)
            if m:
                p = self._pending
                self._pending = None
                type_num = int(p.get("type", -1))
                event = {
                    "kind": TYPE_NAMES.get(type_num, p["label"]), "ts": time.time(), "raw_line": line,
                    "version": int(p.get("version", 0)), "type": type_num,
                    "src": int(p.get("src", 0)), "dst": int(p.get("dst", 0)),
                    "seq": int(p.get("seq", 0)), "session": p.get("session", 0),
                    "flags": p.get("flags", 0), "rs_corrected": p.get("rs_corrected", 0),
                    "rssi": p.get("rssi", 0), "snr": p.get("snr", 0),
                    "payload_text": m.group(1), "payload_hex": bytes(p["hex_bytes"]).hex(),
                }
                if event["type"] == 0 and len(p["hex_bytes"]) >= 1:  # TYPE_TELEMETRY
                    data_id = p["hex_bytes"][0]
                    body = bytes(p["hex_bytes"][1:])
                    event["data_id"] = data_id
                    if data_id == DATA_ID_BEACON1:
                        event["beacon1"] = decode_beacon1(body)
                    elif data_id == DATA_ID_BEACON2:
                        event["beacon2"] = decode_beacon2(body)
                events.append(event)
                return events

        m = RE_REJECTED.search(stripped)
        if m:
            rssi, snr = m.groups()
            events.append({"kind": "REJECTED", "ts": time.time(), "raw_line": line,
                            "rssi": int(rssi), "snr": int(snr)})
            return events

        m = RE_STATS.match(stripped)
        if m:
            ok, bad, misaddr = (int(x) for x in m.groups())
            events.append({"kind": "STATS", "ts": time.time(), "raw_line": line,
                            "ok": ok, "bad": bad, "misaddressed": misaddr})
            return events

        m = RE_TX_COMMAND.match(stripped)
        if m:
            opcode, seq = m.groups()
            events.append({"kind": "TX", "ts": time.time(), "raw_line": line,
                            "opcode": int(opcode, 16), "seq": int(seq)})
            return events

        m = RE_TX_RESULT.search(stripped)
        if m:
            events.append({"kind": "TX_RESULT", "ts": time.time(), "raw_line": line,
                            "result": m.group(1)})
            return events

        m = RE_GAP_REPORT.match(stripped)
        if m:
            events.append({"kind": "GAP", "ts": time.time(), "raw_line": line,
                            "missing": int(m.group(1))})
            return events

        if RE_GAP_NONE.search(stripped):
            events.append({"kind": "GAP", "ts": time.time(), "raw_line": line, "missing": 0})
            return events

        events.append({"kind": "RAW", "ts": time.time(), "raw_line": line})
        return events
