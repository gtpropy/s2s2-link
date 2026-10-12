"""tb.consoles"""

import re
import time
from tb.bus import log
from tb.digi import digi_inject, saf_read
from tb.links import GS, OBC
from tb.util import OBC_CWTEST, OBC_ERASE, OBC_INFO, OBC_INJECT, OBC_RATE, OBC_READF, OBC_RESET, OBC_SAF, OBC_SENDF, OBC_STATUS

# --------------------------------------------------------------------------------------
# debug consoles: palette of ground-station commands, Nucleo (CPU1) OBC commands, raw access
# --------------------------------------------------------------------------------------
GS_PALETTE = [
    {"group": "Telemetry", "label": "HK1", "send": "HK1", "help": "Housekeeping request 1 (voltages, temperatures)"},
    {"group": "Telemetry", "label": "HK2", "send": "HK2", "help": "Housekeeping request 2 (currents, flags)"},
    {"group": "Telemetry", "label": "D 1", "send": "D 1", "help": "Stored housekeeping record from CPU1 flash"},
    {"group": "Camera", "label": "SNAP", "send": "SNAP", "help": "Camera ON, real photo"},
    {"group": "Camera", "label": "DC (dummy cam)", "send": "DC", "help": "Dummy test JPEG downlink"},
    {"group": "Camera", "label": "CAM", "send": "CAM", "help": "Downlink the stored photo (stored + rebuilt in the background)"},
    {"group": "Link", "label": "PING", "send": "PING", "help": "Ping OBC"},
    {"group": "Link", "label": "BURST", "send": "BURST", "help": "Burst test request"},
    {"group": "Link", "label": "ADCS", "send": "ADCS", "help": "ADCS telecommand"},
    {"group": "Link", "label": "EPDM", "send": "EPDM", "help": "EPDM telecommand"},
    {"group": "Digipeater", "label": "DIGI hello", "send": "DIGI K1ABC>APZS2S,ARISS:>hello digipeater", "help": "UI frame via ARISS (the satellite repeats it, VIA is used up)", "edit": True},
    {"group": "Digipeater", "label": "DIGI WIDE2-2", "send": "DIGI K1ABC>APZS2S,WIDE2-2:>test hop budget 2", "help": "New-N: WIDE2-2 becomes WIDE2-1 and is repeated", "edit": True},
    {"group": "Digipeater", "label": "DIGI not for us", "send": "DIGI K1ABC>APZS2S,N0CALL:>not our via", "help": "Path names another station: the satellite must stay silent", "edit": True},
    {"group": "Ground station", "label": "S (status)", "send": "S", "help": "Counters, frequencies, TX power"},
    {"group": "Ground station", "label": "HELP", "send": "HELP", "help": "Command list of the firmware"},
    {"group": "Ground station", "label": "AUTO ON", "send": "AUTO ON", "help": "Periodic automatic HK1"},
    {"group": "Ground station", "label": "AUTO OFF", "send": "AUTO OFF", "help": ""},
    {"group": "Ground station", "label": "LISTEN 30 s", "send": "LISTEN 30000", "help": "Listen for downlink packets"},
    {"group": "Ground station", "label": "PA HP", "send": "PA HP", "help": "Select the high power PA"},
    {"group": "Ground station", "label": "PA LP", "send": "PA LP", "help": "Select the low power PA"},
    {"group": "Ground station", "label": "CARRIER 3 s", "send": "CARRIER 3", "help": "CW carrier on the uplink frequency", "danger": True},
    {"group": "Ground station", "label": "ANT", "send": "ANT", "help": "ANTENNA DEPLOYMENT: burns the wire on the real satellite", "danger": True},
    {"group": "Raw", "label": "HEX …", "send": "HEX 53 01 1D D1 F2 00 00 00 00 00 00 00 00", "help": "Transmit raw hex bytes", "edit": True},
]

OBC_PALETTE = [
    {"name": "STATUS", "cmd": OBC_STATUS, "help": "0 = CPU1 up and the CPU2 heartbeat advances"},
    {"name": "RESET", "cmd": OBC_RESET, "seq": 1, "help": "Clear CPU2's replay floor (use after the GS was rebooted)"},
    {"name": "INFO", "cmd": OBC_INFO, "help": "File held on CPU1's LittleFS (name, size, pages)"},
    {"name": "SAF", "cmd": OBC_SAF, "args": ["n"], "help": "Store-and-forward journal: 0 = status line, n = record n"},
    {"name": "SAF clear", "cmd": OBC_SAF, "seq": 0xFFFF, "help": "Empty the journal (reset CPU2 afterwards)", "danger": True},
    {"name": "INJECT", "cmd": OBC_INJECT, "args": ["text"], "default": {"text": "K1ABC>APZS2S,ARISS:>hello via CPU1"},
     "help": "Hand the digipeater a TNC2 frame as if it had been received over the air"},
    {"name": "READF", "cmd": OBC_READF, "args": ["seq"], "help": "Read chunk n back out of the CPU1 flash file"},
    {"name": "SENDF", "cmd": OBC_SENDF, "args": ["seq", "total"], "help": "CPU2 transmits chunk n of the stored file"},
    {"name": "ERASE", "cmd": OBC_ERASE, "args": ["total"], "help": "Create / empty the stored file", "danger": True},
    {"name": "CWTEST", "cmd": OBC_CWTEST, "args": ["seq", "total"], "help": "seq = seconds (0 stops), total = 0 CW HELLO, 1 GMSK, 2 CW + carrier"},
    {"name": "RATE", "cmd": OBC_RATE, "args": ["seq", "total"], "help": "Air-rate test: seq = gap ms, total = packets"},
]


def obc_cmd(name, a):
    """Run one OBC command by name and wait for its answer. Returns {status, meaning, data}."""
    if not OBC.connected():
        raise RuntimeError("connect the Nucleo's VCP port first")
    if name == "INJECT":
        st, why = digi_inject(a.get("text", ""))
        return {"status": st, "meaning": why}
    if name == "SAF" and int(a.get("n", 0) or 0) == 0:
        line, recs = saf_read()
        return {"status": 0 if line else None, "meaning": line or "no answer", "data": recs}
    p = next((x for x in OBC_PALETTE if x["name"] == name), None)
    if not p:
        raise RuntimeError(f"unknown OBC command {name}")
    seq = int(a.get("seq", p.get("seq", a.get("n", 0))) or 0)
    total = int(a.get("total", 0) or 0)
    data = b""
    if name == "SETNAME":
        data = str(a.get("text", "")).encode()
    mk = OBC.mark()
    if name == "READF":
        st, blk, ok = OBC.read_flash(seq)
        return {"status": st, "meaning": "no answer" if st is None else f"status 0x{st:02X}",
                "data": blk.hex() if blk else None, "crc_ok": ok}
    if name == "INFO":
        txt = OBC.info()
        return {"status": 0 if txt else None, "meaning": txt or "no answer"}
    OBC.send(p["cmd"], seq, total, data)
    st = OBC.wait_ack(p["cmd"], seq, mk, 120 if name in ("SENDF", "ERASE") else 20)
    meaning = "no answer" if st is None else ("ok" if st == 0 else f"status 0x{st:02X}")
    if name == "STATUS" and st is not None:
        meaning = {0: "CPU1 up, CPU2 heartbeat advancing"}.get(st, f"status 0x{st:02X} (CPU2 not answering)")
    return {"status": st, "meaning": meaning}


def obc_raw(hexs):
    """Send raw bytes to the Nucleo's VCP (for protocol debugging); returns what came back in the next second."""
    if not OBC.connected():
        raise RuntimeError("connect the Nucleo's VCP port first")
    b = bytes.fromhex(re.sub(r"[^0-9A-Fa-f]", "", hexs))
    mk, n0 = OBC.mark(), len(OBC.lines)
    OBC.ser.write(b)
    log(f"Nucleo <- raw {len(b)} B: {b[:24].hex()}{'...' if len(b) > 24 else ''}")
    time.sleep(1.0)
    with OBC.lock:
        acks = OBC.acks[mk:]
        lines = [t for _, t in OBC.lines[n0:]]
    return {"sent": len(b), "acks": [{"cmd": c, "seq": s, "status": st} for _, c, s, st in acks], "lines": lines}


def console_lines(src, since, limit=500):
    link = GS if src == "gs" else OBC
    with link.lock:
        n = len(link.lines)
        rows = link.lines[max(since, n - limit):]
    return {"next": n, "lines": [{"t": round(t, 2), "msg": m} for t, m in rows]}
