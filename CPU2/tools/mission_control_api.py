#!/usr/bin/env python3
# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-09-01
"""
mission_control_api.py -- web API + dashboard server for the S2S2 ground
station. Talks to the GS board's real UART (same port s2s2_tx_gui.py /
satellite/gs_main.c use), parses its real decoded output (gs_protocol.py),
persists it to a local SQLite file, and serves a REST + WebSocket API
plus the static dashboard frontend (mission_control_static/).

No demod/RS decode happens here -- the GS board already did that
on-board. This is a parser + persistence + API layer on top of its real
output, same relationship response_analyzer.py has, just for a web
frontend instead of a desktop one.

Install:
    pip install fastapi uvicorn[standard] pyserial

Run:
    python3 mission_control_api.py
    -> dashboard at http://localhost:8765/
    -> first run prints a generated admin password ONCE -- save it, it
       is never shown again (only its PBKDF2 hash is stored, see auth.py)

    python3 mission_control_api.py create_admin_creds [username]
    -> creates the account (default "admin") if missing, or resets its
       password if it exists; prints the new password once, same as above

HONESTY NOTE: most housekeeping/ADCS fields in beacon1_t/beacon2_t are
currently firmware placeholders (memset 0, no sensor wired up yet -- see
gs_protocol.py's docstring and BEACON1_PLACEHOLDER_FIELDS/
BEACON2_PLACEHOLDER_FIELDS). The API returns them as-is (real wire
values, currently always 0) plus a `placeholder_fields` list so the
frontend can label them honestly instead of presenting zeros as if a
sensor produced them.

AUTH: every endpoint below except /api/auth/login, static files, and
the index page requires a valid session cookie (auth.py). Sending a
command additionally requires the account to be an admin or have that
opcode in its allowed_opcodes list, and CRITICAL_OPCODES (mission-
affecting: abort, camera, download, digipeater) additionally require a
`secret` field whose sha256 matches the admin-configured fingerprint
(see /api/settings/critical_secret) -- the plaintext secret itself is
never stored, only its hash, same principle as s2s2_auth.h's SipHash
key on the satellite side.
"""
import asyncio
import json
import os
import re
import secrets as secrets_mod
import sqlite3
import sys
import threading
import time

from fastapi import Cookie, Depends, FastAPI, Response, WebSocket, WebSocketDisconnect, HTTPException
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel
import uvicorn

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import auth
import gs_protocol as gp

HERE = os.path.dirname(os.path.abspath(__file__))
DB_PATH = os.path.join(HERE, "mission_control.db")
STATIC_DIR = os.path.join(HERE, "mission_control_static")

# Mission-affecting opcodes (s2s2_commands.h cubus_cmd_t): these require
# the admin-configured critical-secret on top of normal permission --
# CMD_ABORT_MISSION, CMD_RUN_CAMERA, CMD_DOWNLOAD_FILE, CMD_DIGIPEATER.
CRITICAL_OPCODES = {0x03, 0x10, 0x11, 0x17}

# CLI keyword -> opcode, for permission/critical-secret checks on the
# canned-preset path too (same opcodes Send_Command_Ex uses on-board,
# see gs_main.c's CLI dispatch read during the architecture investigation).
CLI_OPCODES = {
    "COMMAND": 0x01, "HEARTBEAT": 0x02, "HEALTH": 0x12, "HOUSEKEEPING": 0x13,
    "BEACON1": 0x14, "BEACON2": 0x15, "ADCS": 0x16, "CAMERA": 0x10, "DOWNLOAD": 0x11,
    "SATLOG": 0x18, "DIGIPEATER": 0x17, "RETRANSMIT": 0x19, "ABORT": 0x03,
}

_RE_CYCLE_PHASE = re.compile(r"CYCLE PHASE:\s*(CW1|MISSION1|CW2|MISSION2)")
_RE_DIGI_DUP = re.compile(r"DIGI: duplicate, not re-relaying")
_RE_DIGI_RELAYED = re.compile(
    r"DIGI: relayed via hop (\d+) \(([^)]+)\), (\d+) info byte\(s\), rs_corrected=(-?\d+)")
_RE_DIGI_FAILED = re.compile(r"DIGI: relay TX failed")

# =====================================================================
# SQLite persistence
# =====================================================================

_DB_LOCK = threading.Lock()


def _connect_db():
    conn = sqlite3.connect(DB_PATH, check_same_thread=False)
    conn.row_factory = sqlite3.Row
    return conn


def init_db():
    conn = _connect_db()
    conn.executescript("""
    CREATE TABLE IF NOT EXISTS events (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts REAL NOT NULL,
        kind TEXT NOT NULL,
        raw_line TEXT,
        version INTEGER, type INTEGER, src INTEGER, dst INTEGER, seq INTEGER,
        session INTEGER, flags INTEGER, rs_corrected INTEGER, rssi INTEGER, snr INTEGER,
        data_id INTEGER, status INTEGER, status_name TEXT,
        payload_text TEXT, payload_hex TEXT,
        ok_count INTEGER, bad_count INTEGER, misaddressed_count INTEGER,
        opcode INTEGER, result TEXT, missing INTEGER
    );
    CREATE TABLE IF NOT EXISTS beacon1_readings (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        event_id INTEGER, ts REAL NOT NULL,
        type INTEGER, tim_day INTEGER, tim_hour INTEGER,
        bat_v INTEGER, bat_i INTEGER, bat_temp INTEGER, raw_v INTEGER, raw_i INTEGER,
        sol_tot_v INTEGER, sol_tot_i INTEGER, unreg_i INTEGER, main_3v3_i INTEGER,
        msn_3v3_i INTEGER, v5_i INTEGER, adc_3v32_i INTEGER,
        temp1 INTEGER, temp2 INTEGER, temp3 INTEGER, temp4 INTEGER, temp5 INTEGER,
        bpb_t INTEGER, ant_p_t INTEGER, obc_com_t INTEGER,
        system_stat INTEGER, mcu_stat INTEGER, adc_stat INTEGER
    );
    CREATE TABLE IF NOT EXISTS beacon2_readings (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        event_id INTEGER, ts REAL NOT NULL,
        tim_day INTEGER,
        sol_p1_v INTEGER, sol_p2_v INTEGER, sol_p3_v INTEGER, sol_p4_v INTEGER, sol_p5_v INTEGER,
        sol_p1_i INTEGER, sol_p2_i INTEGER, sol_p3_i INTEGER, sol_p4_i INTEGER, sol_p5_i INTEGER,
        gyro_x INTEGER, gyro_y INTEGER, gyro_z INTEGER,
        mag_x INTEGER, mag_y INTEGER, mag_z INTEGER,
        power_stat INTEGER
    );
    CREATE TABLE IF NOT EXISTS commands_sent (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts REAL NOT NULL,
        opcode INTEGER, subsystem INTEGER, data_id INTEGER, address INTEGER, num_packets INTEGER,
        raw_line TEXT, result TEXT, sent_by TEXT
    );
    CREATE TABLE IF NOT EXISTS users (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        username TEXT UNIQUE NOT NULL,
        password_hash TEXT NOT NULL,
        role TEXT NOT NULL DEFAULT 'user',
        allowed_opcodes TEXT NOT NULL DEFAULT '[]',
        created_ts REAL NOT NULL
    );
    CREATE TABLE IF NOT EXISTS settings (
        key TEXT PRIMARY KEY,
        value TEXT
    );
    CREATE TABLE IF NOT EXISTS satellite_events (
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        ts REAL NOT NULL,
        kind TEXT NOT NULL,
        raw_line TEXT,
        phase TEXT,
        hop_index INTEGER, hop_addr TEXT, info_bytes INTEGER, rs_corrected INTEGER
    );
    """)
    conn.commit()
    conn.close()
    _bootstrap_admin()


def _bootstrap_admin():
    """First run only: creates the single admin account with a randomly
    generated password, printed once. Never a hardcoded default -- this
    will eventually be reachable from a real domain (per the user), and
    a hardcoded admin/admin is exactly the kind of thing that gets found."""
    conn = _connect_db()
    count = conn.execute("SELECT COUNT(*) AS c FROM users").fetchone()["c"]
    if count == 0:
        password = secrets_mod.token_urlsafe(12)
        conn.execute(
            "INSERT INTO users (username, password_hash, role, allowed_opcodes, created_ts) "
            "VALUES (?,?,?,?,?)",
            ("admin", auth.hash_password(password), "admin", "[]", time.time()),
        )
        conn.commit()
        print("=" * 72)
        print(" FIRST RUN -- admin account created")
        print("   username: admin")
        print(f"   password: {password}")
        print(" This is shown ONCE. Only its hash is stored -- save it now.")
        print("=" * 72)
    conn.close()


def create_admin_creds(username: str = "admin"):
    """CLI: create the account if missing, or reset its password if it
    exists -- either way makes it an admin with a fresh random password,
    printed once (same one-time-print convention as _bootstrap_admin).
    Use to recover from a lost admin password without wiping the DB."""
    init_db()
    password = secrets_mod.token_urlsafe(16)
    conn = _connect_db()
    row = conn.execute("SELECT id FROM users WHERE username=?", (username,)).fetchone()
    if row:
        conn.execute("UPDATE users SET password_hash=?, role='admin' WHERE id=?",
                     (auth.hash_password(password), row["id"]))
    else:
        conn.execute(
            "INSERT INTO users (username, password_hash, role, allowed_opcodes, created_ts) "
            "VALUES (?,?,?,?,?)",
            (username, auth.hash_password(password), "admin", "[]", time.time()),
        )
    conn.commit()
    conn.close()
    print("=" * 72)
    print(" ADMIN CREDENTIALS RESET" if row else " ADMIN ACCOUNT CREATED")
    print(f"   username: {username}")
    print(f"   password: {password}")
    print(" This is shown ONCE. Only its hash is stored -- save it now.")
    print("=" * 72)


def get_setting(key: str):
    conn = _connect_db()
    row = conn.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
    conn.close()
    return row["value"] if row else None


def set_setting(key: str, value: str):
    with _DB_LOCK:
        conn = _connect_db()
        conn.execute(
            "INSERT INTO settings (key, value) VALUES (?, ?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            (key, value),
        )
        conn.commit()
        conn.close()


def persist_satellite_event(event: dict):
    with _DB_LOCK:
        conn = _connect_db()
        conn.execute(
            "INSERT INTO satellite_events (ts, kind, raw_line, phase, hop_index, hop_addr, "
            "info_bytes, rs_corrected) VALUES (?,?,?,?,?,?,?,?)",
            (event["ts"], event["kind"], event.get("raw_line"), event.get("phase"),
             event.get("hop_index"), event.get("hop_addr"), event.get("info_bytes"),
             event.get("rs_corrected")),
        )
        conn.commit()
        conn.close()


def persist_event(event: dict) -> int:
    with _DB_LOCK:
        conn = _connect_db()
        cur = conn.execute(
            """INSERT INTO events
               (ts, kind, raw_line, version, type, src, dst, seq, session, flags,
                rs_corrected, rssi, snr, data_id, status, status_name,
                payload_text, payload_hex, ok_count, bad_count, misaddressed_count,
                opcode, result, missing)
               VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)""",
            (
                event.get("ts", time.time()), event.get("kind"), event.get("raw_line"),
                event.get("version"), event.get("type"), event.get("src"), event.get("dst"),
                event.get("seq"), event.get("session"), event.get("flags"),
                event.get("rs_corrected"), event.get("rssi"), event.get("snr"),
                event.get("data_id"), event.get("status"), event.get("status_name"),
                event.get("payload_text"), event.get("payload_hex"),
                event.get("ok"), event.get("bad"), event.get("misaddressed"),
                event.get("opcode"), event.get("result"), event.get("missing"),
            ),
        )
        event_id = cur.lastrowid

        b1 = event.get("beacon1")
        if b1:
            conn.execute(
                f"""INSERT INTO beacon1_readings (event_id, ts, {",".join(gp.BEACON1_FIELDS)})
                    VALUES (?, ?, {",".join("?" * len(gp.BEACON1_FIELDS))})""",
                (event_id, event["ts"], *[b1[f] for f in gp.BEACON1_FIELDS]),
            )
        b2 = event.get("beacon2")
        if b2:
            conn.execute(
                f"""INSERT INTO beacon2_readings (event_id, ts, {",".join(gp.BEACON2_FIELDS)})
                    VALUES (?, ?, {",".join("?" * len(gp.BEACON2_FIELDS))})""",
                (event_id, event["ts"], *[b2[f] for f in gp.BEACON2_FIELDS]),
            )
        conn.commit()
        conn.close()
        return event_id


def persist_command(opcode, subsystem, data_id, address, num_packets, raw_line, sent_by="?"):
    with _DB_LOCK:
        conn = _connect_db()
        conn.execute(
            """INSERT INTO commands_sent (ts, opcode, subsystem, data_id, address, num_packets,
               raw_line, result, sent_by) VALUES (?,?,?,?,?,?,?,?,?)""",
            (time.time(), opcode, subsystem, data_id, address, num_packets, raw_line, "pending", sent_by),
        )
        conn.commit()
        conn.close()


# =====================================================================
# Serial connection + background reader, feeding the shared parser
# =====================================================================

class GroundStationLink:
    def __init__(self, broadcast_fn):
        self.ser = None
        self._thread = None
        self._running = False
        self._line_buf = ""
        self.parser = gp.GsProtocolParser()
        self._broadcast_fn = broadcast_fn
        self.port = None
        self.baud = None
        self.last_contact_ts = None
        self.last_beacon1 = None
        self.last_beacon2 = None
        self.stats = {"ok": 0, "bad": 0, "misaddressed": 0}

    @property
    def connected(self) -> bool:
        return self.ser is not None

    def connect(self, port: str, baud: int):
        import serial
        if self.ser is not None:
            self.disconnect()
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.port, self.baud = port, baud
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def disconnect(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=1.5)
            self._thread = None
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
        self.port, self.baud = None, None

    def send_line(self, text: str):
        if self.ser is None:
            raise RuntimeError("not connected")
        self.ser.write((text + "\r\n").encode("ascii"))

    def _run(self):
        while self._running:
            try:
                n = self.ser.in_waiting
                data = self.ser.read(n if n else 1)
            except Exception:
                break
            if not data:
                continue
            text = data.decode("latin-1", errors="replace")
            self._line_buf += text
            parts = self._line_buf.replace("\r\n", "\n").replace("\r", "\n").split("\n")
            self._line_buf = parts[-1]
            for line in parts[:-1]:
                for event in self.parser.feed(line):
                    self._on_event(event)

    def _on_event(self, event: dict):
        self.last_contact_ts = event["ts"]
        if event["kind"] == "STATS":
            self.stats = {"ok": event["ok"], "bad": event["bad"], "misaddressed": event["misaddressed"]}
        if "beacon1" in event:
            self.last_beacon1 = event["beacon1"]
        if "beacon2" in event:
            self.last_beacon2 = event["beacon2"]
        persist_event(event)
        self._broadcast_fn(event)


class SatelliteLink:
    """Optional second serial connection, to the SATELLITE board's own
    debug UART (not the GS uplink/downlink link above) -- this is the
    only place cycle-phase and digipeater activity are visible, since
    those lines are printed by ipcc_m0plus_main.c itself, not relayed
    over RF (see satellite_gui.py, which this reuses the exact same
    regexes from)."""

    def __init__(self, broadcast_fn):
        self.ser = None
        self._thread = None
        self._running = False
        self._line_buf = ""
        self._broadcast_fn = broadcast_fn
        self.port = None
        self.baud = None
        self.cycle_phase = None
        self.digi_relayed = 0
        self.digi_duplicate = 0

    @property
    def connected(self) -> bool:
        return self.ser is not None

    def connect(self, port: str, baud: int):
        import serial
        if self.ser is not None:
            self.disconnect()
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.port, self.baud = port, baud
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def disconnect(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=1.5)
            self._thread = None
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
        self.port, self.baud = None, None

    def send_line(self, text: str):
        if self.ser is None:
            raise RuntimeError("not connected")
        self.ser.write((text + "\r\n").encode("ascii"))

    def _run(self):
        while self._running:
            try:
                n = self.ser.in_waiting
                data = self.ser.read(n if n else 1)
            except Exception:
                break
            if not data:
                continue
            text = data.decode("latin-1", errors="replace")
            self._line_buf += text
            parts = self._line_buf.replace("\r\n", "\n").replace("\r", "\n").split("\n")
            self._line_buf = parts[-1]
            for line in parts[:-1]:
                self._handle_line(line)

    def _handle_line(self, line: str):
        stripped = line.rstrip()
        if not stripped:
            return
        event = {"kind": "SAT_RAW", "ts": time.time(), "raw_line": line}

        m = _RE_CYCLE_PHASE.search(stripped)
        if m:
            self.cycle_phase = m.group(1)
            event = {"kind": "CYCLE_PHASE", "ts": time.time(), "raw_line": line, "phase": self.cycle_phase}
        elif _RE_DIGI_DUP.search(stripped):
            self.digi_duplicate += 1
            event = {"kind": "DIGI_DUPLICATE", "ts": time.time(), "raw_line": line}
        else:
            m = _RE_DIGI_RELAYED.search(stripped)
            if m:
                self.digi_relayed += 1
                event = {"kind": "DIGI_RELAYED", "ts": time.time(), "raw_line": line,
                          "hop_index": int(m.group(1)), "hop_addr": m.group(2),
                          "info_bytes": int(m.group(3)), "rs_corrected": int(m.group(4))}
            elif _RE_DIGI_FAILED.search(stripped):
                event = {"kind": "DIGI_FAILED", "ts": time.time(), "raw_line": line}

        persist_satellite_event(event)
        self._broadcast_fn(event)


# =====================================================================
# FastAPI app
# =====================================================================

app = FastAPI(title="S2S2 Mission Control API")

_ws_clients: set = set()
_loop: asyncio.AbstractEventLoop = None  # set on startup, used to hop threads -> event loop


def broadcast(event: dict):
    if _loop is None:
        return
    asyncio.run_coroutine_threadsafe(_broadcast_async(event), _loop)


async def _broadcast_async(event: dict):
    dead = []
    for ws in list(_ws_clients):
        try:
            await ws.send_text(json.dumps(event))
        except Exception:
            dead.append(ws)
    for ws in dead:
        _ws_clients.discard(ws)


link = GroundStationLink(broadcast)
sat_link = SatelliteLink(broadcast)


@app.on_event("startup")
async def on_startup():
    global _loop
    _loop = asyncio.get_event_loop()
    init_db()


@app.on_event("shutdown")
async def on_shutdown():
    link.disconnect()
    sat_link.disconnect()


# =====================================================================
# Auth dependencies
# =====================================================================

def get_current_user(session: str = Cookie(default=None)) -> dict:
    uid = auth.verify_session_token(session) if session else None
    if uid is None:
        raise HTTPException(status_code=401, detail="not authenticated")
    conn = _connect_db()
    row = conn.execute("SELECT * FROM users WHERE id=?", (uid,)).fetchone()
    conn.close()
    if row is None:
        raise HTTPException(status_code=401, detail="not authenticated")
    return dict(row)


def require_admin(user: dict = Depends(get_current_user)) -> dict:
    if user["role"] != "admin":
        raise HTTPException(status_code=403, detail="admin only")
    return user


def user_can_send(user: dict, opcode: int) -> bool:
    if user["role"] == "admin":
        return True
    try:
        allowed = json.loads(user["allowed_opcodes"])
    except (TypeError, ValueError):
        allowed = []
    return opcode in allowed


def check_critical_secret(opcode: int, secret: str):
    if opcode not in CRITICAL_OPCODES:
        return
    fp = get_setting("critical_fingerprint")
    if not fp:
        raise HTTPException(status_code=403,
                             detail="critical command fingerprint not configured yet (admin must set one)")
    if not secret or auth.fingerprint(secret) != fp:
        raise HTTPException(status_code=403, detail="invalid or missing secret for a critical command")


# --- serial connection management -------------------------------------

class ConnectRequest(BaseModel):
    port: str
    baud: int = 115200


@app.get("/api/serial/ports")
def list_ports(user: dict = Depends(get_current_user)):
    import serial.tools.list_ports
    return [{"device": p.device, "description": p.description} for p in serial.tools.list_ports.comports()]


@app.post("/api/serial/connect")
def connect_serial(req: ConnectRequest, admin: dict = Depends(require_admin)):
    try:
        link.connect(req.port, req.baud)
    except Exception as e:
        raise HTTPException(status_code=400, detail=str(e))
    return {"connected": True, "port": req.port, "baud": req.baud}


@app.post("/api/serial/disconnect")
def disconnect_serial(admin: dict = Depends(require_admin)):
    link.disconnect()
    return {"connected": False}


@app.post("/api/satellite/connect")
def connect_satellite(req: ConnectRequest, admin: dict = Depends(require_admin)):
    try:
        sat_link.connect(req.port, req.baud)
    except Exception as e:
        raise HTTPException(status_code=400, detail=str(e))
    return {"connected": True, "port": req.port, "baud": req.baud}


@app.post("/api/satellite/disconnect")
def disconnect_satellite(admin: dict = Depends(require_admin)):
    sat_link.disconnect()
    return {"connected": False}


@app.get("/api/satellite/status")
def satellite_status(user: dict = Depends(get_current_user)):
    return {
        "connected": sat_link.connected, "port": sat_link.port, "baud": sat_link.baud,
        "cycle_phase": sat_link.cycle_phase, "digi_relayed": sat_link.digi_relayed,
        "digi_duplicate": sat_link.digi_duplicate,
    }


@app.get("/api/status")
def get_status(user: dict = Depends(get_current_user)):
    return {
        "connected": link.connected,
        "port": link.port,
        "baud": link.baud,
        "last_contact_ts": link.last_contact_ts,
        "seconds_since_contact": (time.time() - link.last_contact_ts) if link.last_contact_ts else None,
        "session_stats": link.stats,
    }


# --- telemetry ----------------------------------------------------------

@app.get("/api/telemetry/latest")
def telemetry_latest(user: dict = Depends(get_current_user)):
    return {
        "beacon1": link.last_beacon1,
        "beacon1_placeholder_fields": sorted(gp.BEACON1_PLACEHOLDER_FIELDS),
        "beacon2": link.last_beacon2,
        "beacon2_placeholder_fields": sorted(gp.BEACON2_PLACEHOLDER_FIELDS),
    }


@app.get("/api/telemetry/history")
def telemetry_history(limit: int = 200, user: dict = Depends(get_current_user)):
    conn = _connect_db()
    rows = conn.execute(
        "SELECT * FROM events ORDER BY id DESC LIMIT ?", (limit,)
    ).fetchall()
    conn.close()
    return [dict(r) for r in rows]


@app.get("/api/telemetry/beacon1_history")
def beacon1_history(limit: int = 200, user: dict = Depends(get_current_user)):
    conn = _connect_db()
    rows = conn.execute(
        "SELECT * FROM beacon1_readings ORDER BY id DESC LIMIT ?", (limit,)
    ).fetchall()
    conn.close()
    return [dict(r) for r in rows]


@app.get("/api/telemetry/beacon2_history")
def beacon2_history(limit: int = 200, user: dict = Depends(get_current_user)):
    conn = _connect_db()
    rows = conn.execute(
        "SELECT * FROM beacon2_readings ORDER BY id DESC LIMIT ?", (limit,)
    ).fetchall()
    conn.close()
    return [dict(r) for r in rows]


@app.get("/api/stats/link_quality")
def link_quality(limit: int = 200, user: dict = Depends(get_current_user)):
    """RSSI/SNR/RS-corrected history for the frontend's link-quality chart."""
    conn = _connect_db()
    rows = conn.execute(
        """SELECT ts, rssi, snr, rs_corrected FROM events
           WHERE rssi IS NOT NULL ORDER BY id DESC LIMIT ?""", (limit,)
    ).fetchall()
    conn.close()
    return [dict(r) for r in reversed(rows)]


# --- analytics -----------------------------------------------------------

@app.get("/api/analytics/session_stats_history")
def session_stats_history(limit: int = 100, user: dict = Depends(get_current_user)):
    conn = _connect_db()
    rows = conn.execute(
        """SELECT ts, ok_count, bad_count, misaddressed_count FROM events
           WHERE kind='STATS' ORDER BY id DESC LIMIT ?""", (limit,)
    ).fetchall()
    conn.close()
    return [dict(r) for r in reversed(rows)]


@app.get("/api/analytics/ack_status_distribution")
def ack_status_distribution(user: dict = Depends(get_current_user)):
    conn = _connect_db()
    rows = conn.execute(
        "SELECT status, status_name, COUNT(*) AS count FROM events WHERE kind='ACK' GROUP BY status"
    ).fetchall()
    conn.close()
    return [dict(r) for r in rows]


@app.get("/api/analytics/commands_per_hour")
def commands_per_hour(limit_hours: int = 48, user: dict = Depends(get_current_user)):
    conn = _connect_db()
    rows = conn.execute(
        """SELECT CAST(ts / 3600 AS INTEGER) * 3600 AS hour_bucket, COUNT(*) AS count
           FROM commands_sent GROUP BY hour_bucket ORDER BY hour_bucket DESC LIMIT ?""",
        (limit_hours,),
    ).fetchall()
    conn.close()
    return [dict(r) for r in reversed(rows)]


@app.get("/api/analytics/uptime")
def uptime_history(limit: int = 50, user: dict = Depends(get_current_user)):
    """beacon1_t's tim_day/tim_hour are the only currently-real fields
    (see gs_protocol.py) -- real satellite uptime, not placeholder."""
    conn = _connect_db()
    rows = conn.execute(
        "SELECT ts, tim_day, tim_hour FROM beacon1_readings ORDER BY id DESC LIMIT ?", (limit,)
    ).fetchall()
    conn.close()
    return [dict(r) for r in reversed(rows)]


@app.get("/api/analytics/digipeater")
def digipeater_analytics(user: dict = Depends(get_current_user)):
    conn = _connect_db()
    rows = conn.execute(
        "SELECT ts, kind FROM satellite_events WHERE kind LIKE 'DIGI_%' ORDER BY id DESC LIMIT 200"
    ).fetchall()
    conn.close()
    return {
        "relayed_total": sat_link.digi_relayed,
        "duplicate_total": sat_link.digi_duplicate,
        "recent": [dict(r) for r in reversed(rows)],
    }


# --- mission history / commands -----------------------------------------

@app.get("/api/missions")
def mission_history(limit: int = 100, user: dict = Depends(get_current_user)):
    conn = _connect_db()
    rows = conn.execute(
        "SELECT * FROM commands_sent ORDER BY id DESC LIMIT ?", (limit,)
    ).fetchall()
    conn.close()
    return [dict(r) for r in rows]


class CustomCommand(BaseModel):
    opcode: int
    subsystem: int = 0
    data_id: int = 0
    address: int = 0
    num_packets: int = 0
    secret: str = None


@app.post("/api/command")
def send_command(cmd: CustomCommand, user: dict = Depends(get_current_user)):
    for name, val, maxval in (("opcode", cmd.opcode, 0xFF), ("subsystem", cmd.subsystem, 0xFF),
                               ("data_id", cmd.data_id, 0xFF), ("address", cmd.address, 0xFFFFFFFF),
                               ("num_packets", cmd.num_packets, 0xFFFF)):
        if not (0 <= val <= maxval):
            raise HTTPException(status_code=400, detail=f"{name} out of range")
    if not user_can_send(user, cmd.opcode):
        raise HTTPException(status_code=403, detail="account not permitted to send this opcode")
    check_critical_secret(cmd.opcode, cmd.secret)

    line = f"CMD {cmd.opcode:X} {cmd.subsystem:X} {cmd.data_id:X} {cmd.address:X} {cmd.num_packets}"
    try:
        link.send_line(line)
    except Exception as e:
        raise HTTPException(status_code=400, detail=str(e))
    persist_command(cmd.opcode, cmd.subsystem, cmd.data_id, cmd.address, cmd.num_packets, line,
                     sent_by=user["username"])
    return {"sent": line}


class CliCommand(BaseModel):
    keyword: str
    secret: str = None


@app.post("/api/cli")
def send_cli(cmd: CliCommand, user: dict = Depends(get_current_user)):
    keyword = cmd.keyword.strip().upper()
    if keyword not in CLI_OPCODES:
        raise HTTPException(status_code=400, detail=f"unknown CLI keyword: {keyword!r}")
    opcode = CLI_OPCODES[keyword]
    if not user_can_send(user, opcode):
        raise HTTPException(status_code=403, detail="account not permitted to send this command")
    check_critical_secret(opcode, cmd.secret)
    try:
        link.send_line(keyword)
    except Exception as e:
        raise HTTPException(status_code=400, detail=str(e))
    persist_command(opcode, 0, 0, 0, 0, keyword, sent_by=user["username"])
    return {"sent": keyword}


class DpmTestRequest(BaseModel):
    secret: str = None


@app.post("/api/dpm/test")
def test_dpm(req: DpmTestRequest, user: dict = Depends(get_current_user)):
    """Starts the real digipeater mission (CMD_DIGIPEATER over real RF,
    the same as the GS console's DIGIPEATER button) -- not a simulation."""
    opcode = 0x17  # CMD_DIGIPEATER
    if not user_can_send(user, opcode):
        raise HTTPException(status_code=403, detail="account not permitted to test the digipeater")
    check_critical_secret(opcode, req.secret)
    try:
        link.send_line("DIGIPEATER")
    except Exception as e:
        raise HTTPException(status_code=400, detail=str(e))
    persist_command(opcode, 0x02, 0x09, 0, 0, "DIGIPEATER", sent_by=user["username"])
    return {"sent": "DIGIPEATER"}


# --- auth ------------------------------------------------------------------

class LoginRequest(BaseModel):
    username: str
    password: str


@app.post("/api/auth/login")
def login(req: LoginRequest, response: Response):
    conn = _connect_db()
    row = conn.execute("SELECT * FROM users WHERE username=?", (req.username,)).fetchone()
    conn.close()
    if row is None or not auth.verify_password(req.password, row["password_hash"]):
        raise HTTPException(status_code=401, detail="invalid username or password")
    token = auth.create_session_token(row["id"])
    response.set_cookie("session", token, httponly=True, samesite="lax", max_age=auth.SESSION_TTL_SECONDS)
    return {"username": row["username"], "role": row["role"]}


@app.post("/api/auth/logout")
def logout(response: Response):
    response.delete_cookie("session")
    return {"ok": True}


@app.get("/api/auth/me")
def me(user: dict = Depends(get_current_user)):
    return {"username": user["username"], "role": user["role"],
            "allowed_opcodes": json.loads(user["allowed_opcodes"])}


# --- user management (admin only) ------------------------------------------

class NewUser(BaseModel):
    username: str
    password: str
    role: str = "user"
    allowed_opcodes: list[int] = []


@app.get("/api/users")
def list_users(admin: dict = Depends(require_admin)):
    conn = _connect_db()
    rows = conn.execute(
        "SELECT id, username, role, allowed_opcodes, created_ts FROM users ORDER BY id"
    ).fetchall()
    conn.close()
    return [dict(r) for r in rows]


@app.post("/api/users")
def create_user(u: NewUser, admin: dict = Depends(require_admin)):
    if u.role not in ("admin", "user"):
        raise HTTPException(status_code=400, detail="role must be 'admin' or 'user'")
    if len(u.password) < 8:
        raise HTTPException(status_code=400, detail="password must be at least 8 characters")
    conn = _connect_db()
    try:
        conn.execute(
            "INSERT INTO users (username, password_hash, role, allowed_opcodes, created_ts) "
            "VALUES (?,?,?,?,?)",
            (u.username, auth.hash_password(u.password), u.role, json.dumps(u.allowed_opcodes), time.time()),
        )
        conn.commit()
    except sqlite3.IntegrityError:
        raise HTTPException(status_code=400, detail="username already exists")
    finally:
        conn.close()
    return {"created": u.username}


@app.delete("/api/users/{user_id}")
def delete_user(user_id: int, admin: dict = Depends(require_admin)):
    conn = _connect_db()
    row = conn.execute("SELECT role FROM users WHERE id=?", (user_id,)).fetchone()
    if row is None:
        conn.close()
        raise HTTPException(status_code=404, detail="user not found")
    if row["role"] == "admin":
        admin_count = conn.execute("SELECT COUNT(*) AS c FROM users WHERE role='admin'").fetchone()["c"]
        if admin_count <= 1:
            conn.close()
            raise HTTPException(status_code=400, detail="cannot remove the last admin")
    conn.execute("DELETE FROM users WHERE id=?", (user_id,))
    conn.commit()
    conn.close()
    return {"deleted": user_id}


class PermissionsUpdate(BaseModel):
    allowed_opcodes: list[int]


@app.put("/api/users/{user_id}/permissions")
def update_permissions(user_id: int, body: PermissionsUpdate, admin: dict = Depends(require_admin)):
    conn = _connect_db()
    conn.execute("UPDATE users SET allowed_opcodes=? WHERE id=?", (json.dumps(body.allowed_opcodes), user_id))
    conn.commit()
    conn.close()
    return {"updated": user_id}


class CriticalSecretUpdate(BaseModel):
    secret: str


@app.post("/api/settings/critical_secret")
def set_critical_secret(body: CriticalSecretUpdate, admin: dict = Depends(require_admin)):
    if len(body.secret) < 8:
        raise HTTPException(status_code=400, detail="secret must be at least 8 characters")
    set_setting("critical_fingerprint", auth.fingerprint(body.secret))
    return {"ok": True}


@app.get("/api/settings/critical_secret_configured")
def critical_secret_configured(user: dict = Depends(get_current_user)):
    return {"configured": get_setting("critical_fingerprint") is not None,
            "critical_opcodes": sorted(CRITICAL_OPCODES)}


# --- websocket for live push --------------------------------------------

@app.websocket("/ws")
async def ws_endpoint(websocket: WebSocket):
    await websocket.accept()
    _ws_clients.add(websocket)
    try:
        while True:
            await websocket.receive_text()  # ignore inbound, this is push-only
    except WebSocketDisconnect:
        pass
    finally:
        _ws_clients.discard(websocket)


# --- static frontend -----------------------------------------------------

if os.path.isdir(STATIC_DIR):
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.get("/")
def index():
    index_path = os.path.join(STATIC_DIR, "index.html")
    if os.path.isfile(index_path):
        return FileResponse(index_path)
    raise HTTPException(status_code=404, detail="dashboard not built yet")


def main():
    init_db()
    uvicorn.run(app, host="0.0.0.0", port=7909)


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "create_admin_creds":
        create_admin_creds(sys.argv[2] if len(sys.argv) > 2 else "admin")
    else:
        main()
