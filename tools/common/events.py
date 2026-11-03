"""
events.py -- turns the ground station's UART text into structured events and keeps a history.

parse_gs_line(text) -> list of event dicts, each {"kind": ..., ...}:
    ack        {"what": "accepted"|"completed", "mission": 5}
    nack       {"err": 9, "name": "invalid command", "mission": 255}
    cw         {"text": "S2S2 9N2SJ ..."}            decoded Morse text
    beacon     {"hex": "...", "b1": {...}, "b2": {...}}   the 8 CW housekeeping bytes read both ways (Packet Format sheet, CW)
    camera     {"pkt": 1, "total": 69, "hex": "..."}    one camera fragment (also goes to the image store)
    file       {"pkt": 1, "total": 5, "addr": 0, "len": 128, "hex": "..."}   one chunk of a file download (epdm.txt, adcs.txt, sat_log.txt, housekeeping ...)
    rssi       {"rssi": -97, "snr": 0}                 one received radio packet
    ascii      {"text": "ACK: PING OK (OBC_ONLINE)"}    a plain-text answer of the satellite (ping)
    hk         {}                                      a 128-byte housekeeping packet was shown
    rate       {...}                                   raw-rate counters
EventLog keeps the last events in memory and in a small SQLite file (data/testbench/events.db) so the dashboard has history after a restart.
"""
import collections
import json
import os
import re
import sqlite3
import threading
import time

ERR_NAMES = {2: "packet error", 3: "CRC/checksum error", 4: "incomplete packet", 5: "requested data missing",
             6: "requested data unavailable", 7: "satellite busy", 8: "mission inactive", 9: "invalid command",
             10: "memory/file error", 11: "transmission failed"}
MISSIONS = {1: "housekeeping", 2: "beacon 1", 3: "beacon 2", 4: "ADCS", 5: "camera", 6: "NIR camera", 7: "NDWI image", 8: "EPDM",
            9: "digipeater", 10: "satellite log", 255: "general"}
SYS = {0: "?", 1: "safe", 2: "low power", 3: "normal"}
MISSION_ST = {0: "reserved", 1: "camera", 2: "ADCS", 3: "EPDM"}

_JSON = re.compile(r'\{"type":"([A-Z0-9]+)".*\}')


def _s16(v):
    return v - 65536 if v > 32767 else v


def decode_beacon(hexs):
    """8 bytes of CW BEACON 1 / 2 (Packet Format, CW sheet). The sheet has no beacon number, so both readings are returned."""
    b = bytes.fromhex(hexs)
    if len(b) != 8:
        return None
    b1 = {"system": SYS.get(b[0] >> 6, "?"), "mission": MISSION_ST.get((b[0] >> 4) & 3, "?"), "hours_since_reset": b[0] & 15,
          "sol_total_i": b[1], "sol_total_v": (b[2] << 8) | b[3], "bat_v": ((b[4] << 8) | b[5]) / 100.0, "bat_i": _s16((b[6] << 8) | b[7]) / 100.0}   # BAT_I is the OBC's int16 (negative = discharging)

    def s8(x):
        return x - 256 if x > 127 else x
    b2 = {"gyro": [s8(b[0]), s8(b[1]), s8(b[2])], "magneto": [s8(b[3]), s8(b[4]), s8(b[5])],
          "solar_cells": {n: bool(b[6] & (1 << i)) for i, n in enumerate(["+X", "+Y", "-Z", "+Y2", "+Z"])}, "bat_temp_c": s8(b[7])}
    return {"b1": b1, "b2": b2}


def decode_hk(j):
    """The ground station's machine-readable HK line (128-byte housekeeping packet) in engineering units."""
    g = lambda k: j.get(k, 0)
    return {
        "bat_v": g("vbat") / 100, "sol_v": g("vsol") / 100, "bus_v": g("vbus") / 100,
        "panels_v": {"SP1": g("sp1") / 100, "SP2": g("sp2") / 100, "SP3": g("sp3") / 100, "SP4": g("sp4") / 100, "SP5": g("sp5") / 100},
        "ant_t": g("t_ant") / 10, "bat_t": g("t_bat") / 10, "bpb_t": g("t_bpb") / 10,
        "unreg_i": g("i_unreg") / 100, "i_3v3": g("i_3v3") / 100, "i_5v": g("i_5v") / 100, "bat_i": g("i_bat") / 100, "bus_i": g("i_bus") / 100,
        "flag1": g("f1"), "flag2": g("f2"),
        "gyro": [g("gx") / 100, g("gy") / 100, g("gz") / 100],          # deg/s (the packet carries centi-deg/s)
        "mag": [g("mx"), g("my"), g("mz")],                            # uT
        "seq": g("seq"), "crc_ok": bool(g("crc")), "rssi": g("rssi"), "hex": j.get("hex", ""),
    }


def parse_gs_line(txt):
    out = []
    m = _JSON.search(txt)
    if m:
        try:
            j = json.loads(m.group(0))
        except ValueError:
            j = None
        if j:
            t = j.get("type")
            if t == "ACK":
                mm = re.search(r"mission=0x([0-9A-Fa-f]+)", j.get("msg", ""))
                mis = int(mm.group(1), 16) if mm else None
                out.append({"kind": "ack", "what": "completed" if "Completed" in j.get("msg", "") else "accepted", "mission": mis,
                            "mission_name": MISSIONS.get(mis), "text": j.get("msg")})
            elif t == "NACK":
                mm = re.search(r"err=0x([0-9A-Fa-f]+).*mission=0x([0-9A-Fa-f]+)", j.get("msg", ""))
                err, mis = (int(mm.group(1), 16), int(mm.group(2), 16)) if mm else (None, None)
                out.append({"kind": "nack", "err": err, "name": ERR_NAMES.get(err, "?"), "mission": mis,
                            "mission_name": MISSIONS.get(mis), "text": j.get("msg")})
            elif t == "CWHK":
                d = decode_beacon(j.get("hex", ""))
                if d:
                    out.append({"kind": "beacon", "hex": j["hex"], **d})
            elif t == "CWRX":
                out.append({"kind": "cw", "text": j.get("text", ""), "callsign": j.get("callsign")})
            elif t == "CAMERA":
                out.append({"kind": "camera", "pkt": j.get("pkt"), "total": j.get("total"), "offset": j.get("offset"),
                            "len": j.get("len"), "hex": j.get("hex", "")})
            elif t == "FLASH":                           # one chunk of a file / housekeeping download (128 bytes of the OBC file)
                out.append({"kind": "file", "pkt": j.get("pkt"), "total": j.get("total"), "addr": j.get("addr"), "len": j.get("len"),
                            "status": j.get("status", 0), "hex": j.get("hex", "")})
            elif t == "HK":
                out.append({"kind": "hk", **decode_hk(j)})
            elif t == "RATE":
                out.append({"kind": "rate", **{k: v for k, v in j.items() if k != "type"}})
            elif t in ("CWSIG", "CWCHAR", "CWCAR"):
                out.append({"kind": "cwraw", "type": t, **{k: v for k, v in j.items() if k != "type"}})
        return out
    m = re.search(r"\[CW DECODED\] (.*)", txt)
    if m:
        out.append({"kind": "cw", "text": m.group(1).strip()})
    m = re.search(r'\[GS RX\] ASCII response: "(.*)"', txt)
    if m:                                                    # e.g. the ping answer "ACK: PING OK (OBC_ONLINE)"
        out.append({"kind": "ascii", "text": m.group(1)})
    m = re.search(r"\[GS RX\] Packet #(\d+) \| RSSI=(-?\d+) dBm \| SNR=(-?\d+) dB \| (\d+) bytes", txt)
    if m:
        out.append({"kind": "rssi", "n": int(m.group(1)), "rssi": int(m.group(2)), "snr": int(m.group(3)), "bytes": int(m.group(4))})
    if "HK TELEMETRY PACKET DETECTED" in txt:
        out.append({"kind": "hkdet"})
    return out


class EventLog:
    """Last events in memory (ring) and in SQLite. Noisy kinds (cwraw, rssi) are kept in memory only."""
    PERSIST = {"ack", "nack", "cw", "beacon", "hk", "file", "ascii", "digi", "frame", "command", "test"}

    def __init__(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.execute("CREATE TABLE IF NOT EXISTS ev (t REAL, kind TEXT, body TEXT)")
        self.db.execute("CREATE INDEX IF NOT EXISTS ev_k ON ev (kind, t)")
        self.lock = threading.Lock()
        self.ring = collections.deque(maxlen=2000)
        self.latest = {}

    def add(self, ev):
        ev = dict(ev)
        ev.setdefault("t", round(time.time(), 3))
        with self.lock:
            self.ring.append(ev)
            self.latest[ev["kind"]] = ev
            if ev["kind"] in self.PERSIST:
                self.db.execute("INSERT INTO ev VALUES (?,?,?)", (ev["t"], ev["kind"], json.dumps(ev, default=str)))
                self.db.commit()
        return ev

    def recent(self, kind=None, limit=200):
        with self.lock:
            rows = [e for e in self.ring if kind is None or e["kind"] == kind]
        return rows[-limit:]

    def between(self, t0, t1, kinds=None):
        """Persisted events with t0 <= t < t1, oldest first (kinds: iterable of kind names or None)."""
        q, a = "SELECT body FROM ev WHERE t >= ? AND t < ?", [t0, t1]
        if kinds:
            q += " AND kind IN (%s)" % ",".join("?" * len(kinds))
            a += list(kinds)
        q += " ORDER BY t"
        with self.lock:
            return [json.loads(r[0]) for r in self.db.execute(q, a)]

    def counts(self, t0, t1):
        """{kind: number of persisted events} with t0 <= t < t1."""
        with self.lock:
            return dict(self.db.execute("SELECT kind, COUNT(*) FROM ev WHERE t >= ? AND t < ? GROUP BY kind", (t0, t1)).fetchall())

    def history(self, kind=None, limit=500):
        q, a = "SELECT body FROM ev", []
        if kind:
            q += " WHERE kind=?"
            a.append(kind)
        q += " ORDER BY t DESC LIMIT ?"
        a.append(limit)
        with self.lock:
            return [json.loads(r[0]) for r in self.db.execute(q, a)][::-1]
