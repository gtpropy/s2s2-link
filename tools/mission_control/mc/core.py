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
DATA = os.path.join(ROOT, "data", "mission_control")


def now():
    return time.time()


# ----------------------------------------------------------------------------------------------------------------- event bus (SSE)
class Bus:
    def __init__(self):
        self.lock = threading.Lock()
        self.subs = []

    def publish(self, kind, **kw):
        ev = {"t": round(now(), 3), "kind": kind, **kw}
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
IMAGES = imagestore.ImageStore(os.path.join(DATA, "images"), lambda kind, **kw: BUS.publish(kind, **kw))
AUTH = None                                          # created by init()

# ----------------------------------------------------------------------------------------------------------------- database
DB = sqlite3.connect(os.path.join(DATA, "mission.db"), check_same_thread=False)
DBL = threading.Lock()
with DBL:
    DB.execute("CREATE TABLE IF NOT EXISTS commands (id INTEGER PRIMARY KEY AUTOINCREMENT, t REAL, user TEXT, cmd TEXT, line TEXT, "
               "state TEXT, detail TEXT, t_end REAL)")
    DB.execute("CREATE TABLE IF NOT EXISTS audit (t REAL, user TEXT, action TEXT, detail TEXT)")
    DB.execute("CREATE TABLE IF NOT EXISTS settings (k TEXT PRIMARY KEY, v TEXT)")
    DB.commit()


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


def on_gs_line(t, txt):
    for e in ev_mod.parse_gs_line(txt):
        k = e["kind"]
        if k == "camera":
            try:
                IMAGES.ingest("gs", int(e["pkt"]) - 1, int(e["total"]), bytes.fromhex(e["hex"]))
                STATE["last_contact"] = t
            except (TypeError, ValueError):
                pass
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


def on_gs_state(up, port, err):
    BUS.publish("link", connected=up, port=port, error=err)


GS = None                                            # created in main(): the serial port is only touched once the program is started

# ----------------------------------------------------------------------------------------------------------------- commands
# Mission commands of the Packet Format sheet. `gs` is the line the ground-station firmware understands; debug-only GS commands (raw HEX,
# PA, PWR, CARRIER, BURST, RATE, AUTO, dummy camera) are deliberately not offered here.
CATALOG = [
    {"id": "health", "label": "Health / housekeeping 1", "group": "Telemetry", "gs": "HK1", "role": "operator", "wait_s": 90,
     "help": "Voltages, temperatures and gyro / magnetometer (128-byte housekeeping packet)."},
    {"id": "hk2", "label": "Housekeeping 2", "group": "Telemetry", "gs": "HK2", "role": "operator", "wait_s": 90, "help": "Currents and flags."},
    {"id": "hkdl", "label": "Stored housekeeping download", "group": "Telemetry", "gs": "D {count}", "role": "operator", "wait_s": 240,
     "args": [{"name": "count", "label": "records", "default": 5, "min": 1, "max": 100}], "help": "Records stored in the OBC flash."},
    {"id": "ping", "label": "Ping the OBC", "group": "Link", "gs": "PING", "role": "operator", "wait_s": 90, "help": "Is the on-board computer answering?"},
    {"id": "adcs", "label": "ADCS telecommand", "group": "Subsystems", "gs": "ADCS", "role": "operator", "wait_s": 90, "help": ""},
    {"id": "epdm", "label": "EPDM telecommand", "group": "Subsystems", "gs": "EPDM", "role": "operator", "wait_s": 90, "help": ""},
    {"id": "cam_run", "label": "Camera: take a picture", "group": "Camera", "gs": "SNAP", "role": "operator", "critical": True, "wait_s": 240,
     "help": "Camera ON, real photo through the OBC."},
    {"id": "cam_dl", "label": "Camera: downlink the stored picture", "group": "Camera", "gs": "CAM", "role": "operator", "critical": True, "wait_s": 600,
     "help": "Fragments are stored and the picture is rebuilt while they arrive (Imagery page)."},
    {"id": "digi", "label": "Digipeater message", "group": "Digipeater", "gs": "DIGI {text}", "role": "operator", "critical": True, "wait_s": 5,
     "args": [{"name": "text", "label": "SRC>DEST,VIA:text", "default": "9N2SJ>APZS2S,ARISS:>S2S2 test"}],
     "help": "One AX.25 UI frame the satellite digipeater repeats on the downlink."},
    {"id": "antenna", "label": "Antenna deployment", "group": "Deployment", "gs": "ANT", "role": "admin", "critical": True, "wait_s": 240,
     "help": "Burns the antenna release wire. Irreversible."},
]
JOBS = {}
JQ = queue.Queue()
CUR = {"id": None, "abort": threading.Event()}


def job_update(jid, **kw):
    j = JOBS[jid]
    j.update(kw)
    with DBL:
        DB.execute("UPDATE commands SET state=?, detail=?, t_end=? WHERE id=?",
                   (j["state"], json.dumps(j.get("detail", {}), default=str), j.get("t_end"), jid))
        DB.commit()
    BUS.publish("command", job=public_job(j))


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
    JOBS[jid] = {"id": jid, "t": now(), "user": user, "cmd": cid, "label": c["label"], "line": line, "state": "queued", "detail": {}, "wait_s": c["wait_s"]}
    JQ.put(jid)
    BUS.publish("command", job=public_job(JOBS[jid]))
    return jid


def _events_since(t0):
    return [e for e in EVENTS.recent(limit=600) if e["t"] >= t0 and e["kind"] in ("ack", "nack", "hk", "beacon")]


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
    return [{"id": r[0], "t": r[1], "user": r[2], "cmd": r[3], "line": r[4], "state": r[5], "detail": json.loads(r[6] or "{}"), "t_end": r[7]} for r in rows]


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
