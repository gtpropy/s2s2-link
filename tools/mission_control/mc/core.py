"""mc.core -- state, ground-station link, command executor, views (no HTTP here)."""
import collections
import json
import os
import queue
import re
import sqlite3
import threading
import time

import authz
import events as ev_mod
import imagestore
from gslink import GSLink

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))        # tools/mission_control
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DATA = os.environ.get("S2S_MC_DATA") or os.path.join(ROOT, "data", "mission_control")        # the variable is for tests


def now():
    return time.time()


# ----------------------------------------------------------------------------------------------------------------- event bus (SSE)
class Bus:
    def __init__(self):
        self.lock = threading.Lock()
        self.subs = []

    def publish(self, _kind, **kw):
        ev = {"t": round(now(), 3), "kind": _kind, **kw}
        with self.lock:
            subs = list(self.subs)
        for q in subs:
            try:
                q.put_nowait(ev)
            except queue.Full:
                pass

    def subscribe(self):
        q = queue.Queue(maxsize=2000)
        with self.lock:
            self.subs.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.subs:
                self.subs.remove(q)


BUS = Bus()
EVENTS = ev_mod.EventLog(os.path.join(DATA, "events.db"))
def _publish_image(name, **kw):
    """The image summary carries a field called "kind" (jpg / png); the bus event already has one, so it is passed on as "media"."""
    if "kind" in kw:
        kw["media"] = kw.pop("kind")
    BUS.publish(name, **kw)


IMAGES = imagestore.ImageStore(os.path.join(DATA, "images"), _publish_image)
AUTH = None                                          # created by init()

# ----------------------------------------------------------------------------------------------------------------- database
DB = sqlite3.connect(os.path.join(DATA, "mission.db"), check_same_thread=False)
DBL = threading.Lock()
with DBL:
    DB.execute("CREATE TABLE IF NOT EXISTS commands (id INTEGER PRIMARY KEY AUTOINCREMENT, t REAL, user TEXT, cmd TEXT, line TEXT, "
               "state TEXT, detail TEXT, t_end REAL)")
    DB.execute("CREATE TABLE IF NOT EXISTS audit (t REAL, user TEXT, action TEXT, detail TEXT)")
    DB.execute("CREATE TABLE IF NOT EXISTS settings (k TEXT PRIMARY KEY, v TEXT)")
    DB.execute("CREATE TABLE IF NOT EXISTS hb (t REAL, ok INTEGER, rtt REAL, rssi REAL, snr REAL)")
    DB.execute("CREATE INDEX IF NOT EXISTS hb_t ON hb (t)")
    DB.execute("CREATE TABLE IF NOT EXISTS raw (t REAL, text TEXT)")
    DB.execute("CREATE INDEX IF NOT EXISTS raw_t ON raw (t)")
    DB.execute("CREATE TABLE IF NOT EXISTS rx (t REAL, rssi REAL, snr REAL)")             # every received packet's signal: the day summary needs it after a restart
    DB.execute("CREATE INDEX IF NOT EXISTS rx_t ON rx (t)")
    DB.commit()


HOOKS = collections.defaultdict(list)                  # name -> [callables]; ops modules (store, link) register here, core never imports them


def hook(name, *a):
    for f in list(HOOKS[name]):
        try:
            f(*a)
        except Exception as e:                           # a recording problem must never stop the radio path
            print("hook %s failed: %s" % (name, e))


def audit(user, action, detail=""):
    with DBL:
        DB.execute("INSERT INTO audit VALUES (?,?,?,?)", (now(), user, action, detail))
        DB.commit()


def setting(k, default=None):
    with DBL:
        r = DB.execute("SELECT v FROM settings WHERE k=?", (k,)).fetchone()
    return json.loads(r[0]) if r else default


def set_setting(k, v):
    with DBL:
        DB.execute("INSERT OR REPLACE INTO settings VALUES (?,?)", (k, json.dumps(v)))
        DB.commit()


# ----------------------------------------------------------------------------------------------------------------- ground station
STATE = {"latest": {}, "rssi": collections.deque(maxlen=600), "counters": collections.Counter(), "last_contact": 0.0}
SERIES = {"hk": collections.deque(maxlen=5000), "beacon": collections.deque(maxlen=5000)}


RAW = collections.deque(maxlen=3000)                 # every line the ground station printed (the "raw log" screen)
_raw_n = [0]


def raw_since(t0, n=300):
    return [r for r in list(RAW) if r["t"] >= t0][-n:]


def raw_log(t, txt):
    txt = txt.rstrip("\r\n")[:600]
    if not txt.strip():
        return
    r = {"t": round(t, 3), "text": txt}
    RAW.append(r)
    with DBL:
        DB.execute("INSERT INTO raw VALUES (?,?)", (r["t"], txt))
        _raw_n[0] += 1
        if _raw_n[0] % 500 == 0:                                       # keep the newest 30000 lines
            DB.execute("DELETE FROM raw WHERE rowid <= (SELECT MAX(rowid) FROM raw) - 30000")
        DB.commit()
    BUS.publish("raw", line=r)
    hook("raw", r)


def on_gs_line(t, txt):
    raw_log(t, txt)
    for e in ev_mod.parse_gs_line(txt):
        k = e["kind"]
        if k == "camera":
            try:
                IMAGES.ingest("gs", int(e["pkt"]) - 1, int(e["total"]), bytes.fromhex(e["hex"]))
                STATE["last_contact"] = t
            except (TypeError, ValueError):
                pass
            continue
        if k == "cwraw" and e.get("type") == "CWCAR" and (e.get("ms") or 0) >= 300 and e.get("rssi") is not None:
            # a tuning carrier of at least 300 ms is the start of a CW beacon: count it as a signal heard (contact + received signal),
            # also when the characters are too weak to decode. No SNR is measured for it.
            STATE["rssi"].append((t, e["rssi"], None))
            STATE["last_contact"] = t
            BUS.publish("evt", ev={"kind": "rssi", "t": round(t, 3), "rssi": e["rssi"], "snr": None, "src": "cw"})
            continue
        if k in ("cwraw", "rate", "hkdet"):
            continue
        e = EVENTS.add({**e, "t": round(t, 3)})
        STATE["counters"][k] += 1
        if k in ("ack", "nack", "beacon", "hk", "cw"):
            STATE["last_contact"] = t
            STATE["latest"][k] = e
        if k == "rssi":
            STATE["rssi"].append((t, e["rssi"], e["snr"]))
            STATE["last_contact"] = t
        if k in SERIES:
            SERIES[k].append(e)
        BUS.publish("evt", ev=e)
        hook("event", e)


_gs_last = {"up": None, "err": None}


def on_gs_state(up, port, err):
    BUS.publish("link", connected=up, port=port, error=err)
    if _gs_last["up"] != up or (not up and _gs_last["err"] != err):             # every change of the ground-station connection is a record for the day summary
        _gs_last.update(up=up, err=err)
        audit("system", "ground station connected" if up else "ground station disconnected", "%s %s" % (port or "", err or "")[:200])


GS = None                                            # created in main(): the serial port is only touched once the program is started

# ----------------------------------------------------------------------------------------------------------------- commands
# Mission commands of the Packet Format sheet. `gs` is the line the ground-station firmware understands; debug-only GS commands (raw HEX,
# PA, PWR, CARRIER, BURST, RATE, AUTO, dummy camera) are deliberately not offered here.
CATALOG = [
    {"id": "health", "label": "Health / housekeeping 1", "group": "Telemetry", "gs": "HK1", "role": "operator", "wait_s": 90,
     "help": "Requests the 128-byte housekeeping packet: bus and battery voltages, temperatures, currents and inertial / magnetic readings."},
    {"id": "hk2", "label": "Housekeeping 2", "group": "Telemetry", "gs": "HK2", "role": "operator", "wait_s": 90, "help": "Requests the latest stored housekeeping record (currents and flags)."},
    {"id": "hkdl", "label": "Stored housekeeping download", "group": "Telemetry", "gs": "D {count}", "role": "operator", "wait_s": 240,
     "args": [{"name": "count", "label": "records", "default": 5, "min": 1, "max": 100}], "help": "Downloads stored housekeeping records from the on-board flash."},
    {"id": "ping", "label": "Ping the OBC", "group": "Link", "gs": "PING", "role": "operator", "wait_s": 90,
     "help": "Is the on-board computer answering? The satellite replies ACK accepted, the text \"ACK: PING OK (OBC_ONLINE)\" and ACK completed."},
    {"id": "adcs", "label": "Run the ADCS mission", "group": "Missions", "gs": "ADCS", "role": "operator", "critical": True, "wait_s": 120,
     "help": "Starts the attitude determination and control (ADCS) mission, MCU 03, using the command defined in the reference firmware. The satellite acknowledges the command; no data is returned with it. Results are read from the ADCS data file."},
    {"id": "epdm", "label": "Run the EPDM mission", "group": "Missions", "gs": "EPDM", "role": "operator", "critical": True, "wait_s": 120,
     "help": "Starts the EPDM mission (earthquake precursor detection: magnetic-field precursor measurements), MCU 05, using the command defined in the reference firmware. The satellite acknowledges the command; no data is returned with it. Results are read from the EPDM data file."},
    {"id": "adcs_file", "label": "ADCS file download (adcs.txt, F7)", "group": "Data files", "gs": "ADCSFILE {count}", "role": "operator", "wait_s": 240, "args": [{"name": "count", "label": "pieces of 128 bytes", "default": 5, "min": 1, "max": 100}],
     "help": "Downloads adcs.txt (file F7 of the Packet Format file table). The content format is not final; it is displayed as decoded text."},
    {"id": "epdm_file", "label": "EPDM file download (epdm.txt, F6)", "group": "Data files", "gs": "EPDMFILE {count}", "role": "operator", "wait_s": 240, "args": [{"name": "count", "label": "pieces of 128 bytes", "default": 5, "min": 1, "max": 100}],
     "help": "Downloads epdm.txt (file F6 of the Packet Format file table). The content format is not final; it is displayed as decoded text."},
    {"id": "satlog", "label": "Satellite log download (sat_log.txt, F3)", "group": "Data files", "gs": "SATLOG {count}", "role": "operator", "wait_s": 240, "args": [{"name": "count", "label": "pieces of 128 bytes", "default": 5, "min": 1, "max": 100}],
     "help": "Downloads sat_log.txt (file F3), the satellite event log."},
    {"id": "flags", "label": "Flags file download (flags.txt, F1)", "group": "Data files", "gs": "FLAGS {count}", "role": "operator", "wait_s": 240, "args": [{"name": "count", "label": "pieces of 128 bytes", "default": 5, "min": 1, "max": 100}],
     "help": "Downloads flags.txt (file F1)."},
    {"id": "rsvtable", "label": "Reserved table download (rsv_table.txt, F4)", "group": "Data files", "gs": "RSVTABLE {count}", "role": "operator", "wait_s": 240, "args": [{"name": "count", "label": "pieces of 128 bytes", "default": 5, "min": 1, "max": 100}],
     "help": "Downloads rsv_table.txt (file F4)."},
    {"id": "cam_run", "label": "Camera: take a picture", "group": "Camera", "gs": "SNAP", "role": "operator", "critical": True, "wait_s": 240,
     "help": "Starts the camera mission: the on-board computer captures an image and stores it."},
    {"id": "cam_dl", "label": "Camera: downlink the stored picture", "group": "Camera", "gs": "CAM", "role": "operator", "critical": True, "wait_s": 600,
     "help": "Transfers the stored image to the ground. Fragments are assembled automatically in the Images view."},
    {"id": "dummycam", "label": "Camera: dummy test picture", "group": "Camera", "gs": "DUMMYCAM", "role": "operator", "critical": True, "wait_s": 600,
     "help": "Transfers the on-board test image (69 fragments) without capturing a new one. Used to verify the imaging chain."},
    {"id": "digi", "label": "Digipeater message", "group": "Digipeater", "gs": "DIGI {text}", "role": "operator", "critical": True, "wait_s": 5,
     "args": [{"name": "text", "label": "SRC>DEST,VIA:text", "default": "9N2SJ>APZS2S,APNDIG:>S2S2 test"}],
     "help": "Transmits one AX.25 UI frame that the satellite digipeater may repeat on the downlink."},
    {"id": "antenna", "label": "Antenna deployment", "group": "Deployment", "gs": "ANT", "role": "admin", "critical": True, "wait_s": 240,
     "help": "Activates the antenna release mechanism. This action is irreversible."},
]
# Plain-language names for people who are new to the software, the over-the-air opcode of the Packet Format sheet (hex, as the
# satellite sees it: SAT-ID, MCU id, opcode B1..B3; the data-id / address / count fields follow) and an icon.
SIMPLE = {
    "health": ("Health report", "HK", "53 01 1D D1 F2", "ACK accepted, one 128-byte housekeeping packet, ACK completed."),
    "hk2": ("Housekeeping record", "HK", "53 01 1D D1 F8", "ACK accepted, stored housekeeping data in 128-byte blocks, ACK completed."),
    "hkdl": ("Stored housekeeping download", "HKD", "53 01 1D D1 F8", "ACK accepted, stored housekeeping data in 128-byte blocks, ACK completed."),
    "ping": ("Link test (ping)", "PING", "53 01 1D D1 F5", "ACK accepted, the text \"ACK: PING OK (OBC_ONLINE)\", ACK completed."),
    "adcs": ("ADCS mission start", "ADCS", "53 03 A0 53 CF", "ACK accepted, ACK completed. No data packet is part of the response."),
    "epdm": ("EPDM mission start", "EPDM", "53 05 EC CF CF", "ACK accepted, ACK completed. No data packet is part of the response."),
    "adcs_file": ("ADCS data file (adcs.txt)", "ADCS", "53 01 1D D2 F7", "ACK accepted, the file in 128-byte blocks, ACK completed; NACK 05 if the file is not present."),
    "epdm_file": ("EPDM data file (epdm.txt)", "EPDM", "53 01 1D D2 F6", "ACK accepted, the file in 128-byte blocks, ACK completed; NACK 05 if the file is not present."),
    "satlog": ("Satellite log (sat_log.txt)", "LOG", "53 01 1D D1 F3", "ACK accepted, the file in 128-byte blocks, ACK completed; NACK 05 if the file is not present."),
    "flags": ("Status flags (flags.txt)", "FLG", "53 01 1D D1 F1", "ACK accepted, the file in 128-byte blocks, ACK completed; NACK 05 if the file is not present."),
    "rsvtable": ("Reserved table (rsv_table.txt)", "RSV", "53 01 1D D1 F4", "ACK accepted, the file in 128-byte blocks, ACK completed; NACK 05 if the file is not present."),
    "cam_run": ("Capture image", "CAM", "53 04 CC 5E BD", "ACK accepted, ACK completed. The image stays in on-board storage until downloaded."),
    "cam_dl": ("Download stored image", "IMG", "53 01 1D D2 F5", "ACK accepted, the image in 69 fragments, ACK completed; NACK 05 if no image is stored."),
    "dummycam": ("Test image downlink", "TST", "53 04 CC 5E BE", "ACK accepted, the on-board test image in 69 fragments, ACK completed."),
    "digi": ("Digipeater test frame", "DIGI", "", "One-way transmission. The satellite repeats the frame on the downlink if the digipeater rules accept it."),
    "antenna": ("Antenna deployment", "ANT", "53 01 DE EF AA", "ACK accepted, ACK completed."),
}
HELP = {   # operator-facing descriptions (professional wording); the first sentence is shown on the command cards
    "health": "Requests the 128-byte housekeeping packet: bus and battery voltages, temperatures, currents and inertial / magnetic readings.",
    "hk2": "Requests the latest stored housekeeping record (currents and flags).",
    "hkdl": "Downloads stored housekeeping records from the on-board flash.",
    "ping": "Verifies that the on-board computer responds. The satellite acknowledges and returns a status text.",
    "adcs": "Starts the attitude determination and control (ADCS) mission, MCU 03, with the command defined in the reference firmware. The satellite acknowledges the command; no data is returned with it. Results are read from the ADCS data file.",
    "epdm": "Starts the EPDM mission (earthquake precursor detection: magnetic-field precursor measurements), MCU 05, with the command defined in the reference firmware. The satellite acknowledges the command; no data is returned with it. Results are read from the EPDM data file.",
    "adcs_file": "Downloads adcs.txt (file F7 of the Packet Format file table). The content format is not final; it is displayed as decoded text.",
    "epdm_file": "Downloads epdm.txt (file F6 of the Packet Format file table). The content format is not final; it is displayed as decoded text.",
    "satlog": "Downloads sat_log.txt (file F3), the satellite event log.",
    "flags": "Downloads flags.txt (file F1).",
    "rsvtable": "Downloads rsv_table.txt (file F4).",
    "cam_run": "Starts the camera mission: the on-board computer captures an image and stores it.",
    "cam_dl": "Transfers the stored image to the ground. Fragments are assembled automatically in the Images view.",
    "dummycam": "Transfers the on-board test image (69 fragments) without capturing a new one. Used to verify the imaging chain.",
    "digi": "Transmits one AX.25 UI frame that the satellite digipeater may repeat on the downlink.",
    "antenna": "Activates the antenna release mechanism. This action is irreversible.",
}
for _c in CATALOG:
    _c["help"] = HELP[_c["id"]]
    _c["simple"], _c["icon"], _c["opcode"], _c["reply"] = SIMPLE[_c["id"]]
JOBS = {}
JQ = queue.Queue()
TX = threading.Lock()                                # one radio transmission sequence at a time (commands, heartbeat)
CUR = {"id": None, "abort": threading.Event()}


def rssi_near(t, window=25.0):
    """The received-signal sample (rssi dBm, snr dB) closest to time t, if one lies within `window` seconds."""
    best = None
    for (ts, rs, sn) in list(STATE["rssi"]):
        if abs(ts - t) <= window and (best is None or abs(ts - t) < abs(best[0] - t)):
            best = (ts, rs, sn)
    return {"t": best[0], "rssi": best[1], "snr": best[2]} if best else None


TERMINAL = ("completed", "nack", "timeout", "incomplete", "cancelled", "error", "sent")


def job_update(jid, **kw):
    j = JOBS[jid]
    j.update(kw)
    if j["state"] in TERMINAL and "rssi" not in j.setdefault("detail", {}):
        acks = [e["t"] for e in (j["detail"].get("events") or []) if e.get("kind") in ("ack", "nack")]
        j["detail"]["rssi"] = {"at_send": rssi_near(j["t"]), "at_response": rssi_near(min(acks)) if acks else None}
    j.setdefault("detail", {})["raw"] = raw_since(j["t"] - 0.5, 200)   # what the ground station printed since the command was queued
    with DBL:
        DB.execute("UPDATE commands SET state=?, detail=?, t_end=? WHERE id=?",
                   (j["state"], json.dumps(j.get("detail", {}), default=str), j.get("t_end"), jid))
        DB.commit()
    BUS.publish("command", job=public_job(j))
    if j["state"] in TERMINAL:
        hook("job_done", public_job(j))


def public_job(j):
    return {k: v for k, v in j.items() if k != "abort"}


def submit(user, cid, args):
    c = next((x for x in CATALOG if x["id"] == cid), None)
    if not c:
        raise ValueError("unknown command")
    a = {}
    for x in c.get("args", []):
        v = args.get(x["name"], x["default"])
        if x["name"] == "count":
            v = max(x["min"], min(x["max"], int(v)))
        elif x["name"] == "text":
            v = str(v)
            if not re.match(r"^[A-Za-z0-9\-]{1,9}>[A-Za-z0-9\-]{1,9}(,[A-Za-z0-9\-*]{1,9}){0,8}:.{0,80}$", v):
                raise ValueError("digipeater text must look like SRC>DEST,VIA:text")
        a[x["name"]] = v
    line = c["gs"].format(**a)
    if not GS.connected():
        raise RuntimeError("ground station not connected")
    with DBL:
        cur = DB.execute("INSERT INTO commands (t,user,cmd,line,state,detail) VALUES (?,?,?,?,?,?)", (now(), user, cid, line, "queued", "{}"))
        DB.commit()
        jid = cur.lastrowid
    JOBS[jid] = {"id": jid, "t": now(), "user": user, "cmd": cid, "label": c["label"], "simple": c["simple"], "icon": c["icon"], "opcode": c["opcode"],
                 "line": line, "state": "queued", "detail": {}, "wait_s": c["wait_s"]}
    JQ.put(jid)
    BUS.publish("command", job=public_job(JOBS[jid]))
    return jid


def _events_since(t0):
    return [e for e in EVENTS.recent(limit=600) if e["t"] >= t0 and e["kind"] in ("ack", "nack", "hk", "beacon", "cw", "file", "ascii")]


def run_job(jid):
    j = JOBS[jid]
    CUR["id"] = jid
    CUR["abort"].clear()
    t0 = now()
    try:
        if j["cmd"] == "digi":                       # one-way: the satellite does not acknowledge a repeated frame
            GS.send(j["line"])
            job_update(jid, state="sent", detail={"note": "frame transmitted; watch the downlink for the repeat"}, t_end=now())
            return
        got, tries = [], 0
        while tries < 8:
            tries += 1
            GS.send(j["line"])
            job_update(jid, state="sending", detail={"attempt": tries, "events": got})
            end = now() + 14
            while now() < end and not CUR["abort"].is_set():
                got = _events_since(t0)
                if any(e["kind"] == "nack" for e in got) or any(e["kind"] == "ack" for e in got):
                    break
                time.sleep(0.2)
            if CUR["abort"].is_set():
                job_update(jid, state="cancelled", detail={"events": got}, t_end=now())
                return
            if any(e["kind"] == "ack" or e["kind"] == "nack" for e in got):
                break
        got = _events_since(t0)
        nack = next((e for e in got if e["kind"] == "nack"), None)
        if nack:
            job_update(jid, state="nack", detail={"attempt": tries, "events": got, "error": f"NACK 0x{nack['err']:02X} {nack['name']}"}, t_end=now())
            return
        if not any(e["kind"] == "ack" for e in got):
            job_update(jid, state="timeout", detail={"attempt": tries, "events": got, "error": "no ACK from the satellite (it listens ~50 s of every 90 s)"}, t_end=now())
            return
        job_update(jid, state="accepted", detail={"attempt": tries, "events": got})
        end = now() + j["wait_s"]
        while now() < end and not CUR["abort"].is_set():
            got = _events_since(t0)
            nack = next((e for e in got if e["kind"] == "nack"), None)
            if nack:
                job_update(jid, state="nack", detail={"attempt": tries, "events": got, "error": f"NACK 0x{nack['err']:02X} {nack['name']}"}, t_end=now())
                return
            if any(e["kind"] == "ack" and e["what"] == "completed" for e in got):
                job_update(jid, state="completed", detail={"attempt": tries, "events": got}, t_end=now())
                return
            time.sleep(0.3)
        job_update(jid, state="cancelled" if CUR["abort"].is_set() else "incomplete",
                   detail={"attempt": tries, "events": got, "error": "" if CUR["abort"].is_set() else "accepted but no completion ACK arrived"}, t_end=now())
    except Exception as e:
        job_update(jid, state="error", detail={"error": str(e)}, t_end=now())
    finally:
        CUR["id"] = None


def worker():
    while True:
        jid = JQ.get()
        if JOBS[jid]["state"] == "cancelled":
            continue
        with TX:
            run_job(jid)


threading.Thread(target=worker, daemon=True).start()


# ----------------------------------------------------------------------------------------------------------------- views
def snapshot():
    lt = STATE["latest"]
    rs = list(STATE["rssi"])
    return {"t": now(), "gs": {"connected": GS.connected(), "port": GS.port, "ports": GS.ports(), "error": GS.error,
                               "last_byte_age": round(now() - GS.last_rx, 1) if GS.last_rx else None},
            "last_contact": STATE["last_contact"] or None, "latest": {k: v for k, v in lt.items()},
            "rssi": rs[-120:], "counters": dict(STATE["counters"]), "images": IMAGES.list()[:12],
            "queue": [public_job(j) for j in JOBS.values() if j["state"] in ("queued", "sending", "accepted")],
            "limits": setting("limits", {})}


def series(kind, n):
    return [e for e in list(SERIES[kind])[-n:]] if kind in SERIES else []


def history_jobs(n=100):
    with DBL:
        rows = DB.execute("SELECT id,t,user,cmd,line,state,detail,t_end FROM commands ORDER BY id DESC LIMIT ?", (n,)).fetchall()
    out = []
    for r in rows:
        c = next((x for x in CATALOG if x["id"] == r[3]), {})
        out.append({"id": r[0], "t": r[1], "user": r[2], "cmd": r[3], "label": c.get("label", r[3]), "simple": c.get("simple", r[3]), "icon": c.get("icon", ""),
                    "opcode": c.get("opcode", ""), "line": r[4], "state": r[5], "detail": json.loads(r[6] or "{}"), "t_end": r[7]})
    return out


def rawlog(n=500, q="", kind=""):
    sql, a = "SELECT t,text FROM raw", []
    cond = []
    if q:
        cond.append("text LIKE ?")
        a.append("%" + q + "%")
    pat = {"beacon": "%CW%", "command": "%UPLINK%", "radio": "%[GS RX]%", "ack": "%ACK%"}.get(kind)
    if pat:
        cond.append("text LIKE ?")
        a.append(pat)
    if cond:
        sql += " WHERE " + " AND ".join(cond)
    sql += " ORDER BY rowid DESC LIMIT ?"
    a.append(max(1, min(int(n), 5000)))
    with DBL:
        rows = DB.execute(sql, a).fetchall()
    return [{"t": r[0], "text": r[1]} for r in rows][::-1]


def csv_export(kind):
    rows = EVENTS.history(kind, 20000)
    if not rows:
        return "t\n"
    keys = ["t"]
    flat = []
    for e in rows:
        f = {}

        def walk(prefix, v):
            if isinstance(v, dict):
                for kk, vv in v.items():
                    walk(prefix + kk + ".", vv)
            elif isinstance(v, list):
                for i, vv in enumerate(v):
                    walk(prefix + str(i) + ".", vv)
            else:
                f[prefix[:-1]] = v
        walk("", {k: v for k, v in e.items() if k not in ("hex", "kind")})
        flat.append(f)
        for k in f:
            if k not in keys:
                keys.append(k)
    out = [",".join(keys)]
    for f in flat:
        out.append(",".join(str(f.get(k, "")) for k in keys))
    return "\n".join(out) + "\n"


# ----------------------------------------------------------------------------------------------------------------- HTTP

def init(gs_port=None):
    """Create the account store and the ground-station link (the serial port is only touched when this is called)."""
    global AUTH, GS
    AUTH = authz.Auth(os.path.join(DATA, "users.json"), True, "Mission Control")
    GS = GSLink(on_gs_line, on_gs_state, want=gs_port)
    from mc import gdrive, link, store                  # imported here: they import core
    store.start()
    gdrive.start()
    link.start()
