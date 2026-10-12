"""tb.commands"""

import os
import threading
import time
from tb.bus import BUS, EVENTS, add_event, log
from tb.compat import CORE
from tb.links import GS
from tb.util import now

# --------------------------------------------------------------------------------------
# commands: every command of the Packet Format sheet and the console's own tools, sent through the ground station
# --------------------------------------------------------------------------------------
COMMANDS = [
    {"id": "health", "group": "Telemetry", "label": "HEALTH SAT", "gs": "HK1", "opcode": "53 01 1D D1 F2",
     "help": "Health housekeeping: ACK accepted, one 128-byte housekeeping packet, ACK completed."},
    {"id": "hkdl", "group": "Telemetry", "label": "Housekeeping download", "gs": "D {count}", "opcode": "53 01 1D D1 F8",
     "args": [{"name": "count", "label": "records", "default": 1}], "help": "Stored housekeeping records from CPU1's flash."},
    {"id": "cam_run", "group": "Camera", "label": "Run camera mission", "gs": "SNAP", "opcode": "53 04 CC 5E BD", "critical": True,
     "help": "Camera capture on the OBC."},
    {"id": "cam_dl", "group": "Camera", "label": "Camera download", "gs": "CAM", "opcode": "53 01 1D D2 F5", "critical": True,
     "help": "Downlink the stored image as camera fragments; the console stores and rebuilds them while they arrive."},
    {"id": "antenna", "group": "Deployment", "label": "Antenna deployment", "gs": "ANT", "opcode": "53 01 DE EF AA", "critical": True,
     "help": "Burns the antenna wire on the real satellite. Do not send on the bench with hardware attached."},
    {"id": "ping", "group": "Link", "label": "Ping", "gs": "PING", "opcode": "53 01 1D D1 F2", "help": "Same as HEALTH SAT."},
    {"id": "hk2", "group": "Telemetry", "label": "Housekeeping 2", "gs": "HK2", "help": "Currents and flags."},
    {"id": "adcs", "group": "Subsystems", "label": "ADCS telecommand", "gs": "ADCS", "help": ""},
    {"id": "epdm", "group": "Subsystems", "label": "EPDM telecommand", "gs": "EPDM", "help": ""},
    {"id": "bad", "group": "Link", "label": "Invalid opcode (expect NACK)", "gs": "HEX 53 01 FF FF FF 00 00 00 00 00 00 00 00",
     "opcode": "53 01 FF FF FF", "help": "The satellite must answer NACK 0xEE 0x09 (invalid command)."},
    {"id": "digi", "group": "Digipeater", "label": "Digipeater test frame", "gs": "DIGI {text}",
     "args": [{"name": "text", "label": "SRC>DEST,VIA:text", "default": "K1ABC>APZS2S,ARISS:>hello via the ground station"}],
     "help": "One UI frame with that path; the satellite repeats it on the downlink."},
    {"id": "status", "group": "Ground station", "label": "GS status", "gs": "S", "help": "Counters of the ground station."},
]
JOBS = {}
_job_seq = [0]


def run_command(cmd_id, args, abort=None, attempts=10, every=14.0):
    """Send a command through the GS and collect the events that come back until it completed, was refused, or time ran out."""
    c = next((x for x in COMMANDS if x["id"] == cmd_id), None)
    if not c:
        raise RuntimeError(f"unknown command {cmd_id}")
    if not GS.connected():
        raise RuntimeError("connect the ground station's serial port first")
    text = c["gs"].format(**{a["name"]: args.get(a["name"], a["default"]) for a in c.get("args", [])})
    t0 = now()
    got, verdict = [], "no answer"
    for attempt in range(1, attempts + 1):
        GS.send(text)
        add_event({"kind": "command", "id": cmd_id, "text": text, "attempt": attempt})
        end = now() + every
        while now() < end:
            if abort and abort.is_set():
                return {"command": cmd_id, "sent": text, "events": got, "verdict": "aborted"}
            got = [e for e in EVENTS.recent(limit=400) if e["t"] >= t0 and e["kind"] in ("ack", "nack", "hk", "frame", "beacon")]
            if any(e["kind"] == "nack" for e in got):
                verdict = "NACK"
                return {"command": cmd_id, "sent": text, "events": got, "attempts": attempt, "verdict": verdict, "seconds": round(now() - t0, 1)}
            if any(e["kind"] == "ack" and e["what"] == "completed" for e in got):
                return {"command": cmd_id, "sent": text, "events": got, "attempts": attempt, "verdict": "completed", "seconds": round(now() - t0, 1)}
            if cmd_id in ("digi", "status", "hkdl") and any(e["kind"] == "ack" for e in got) and cmd_id != "hkdl":
                break
            time.sleep(0.2)
        if cmd_id in ("status", "digi"):
            return {"command": cmd_id, "sent": text, "events": got, "attempts": attempt, "verdict": "sent", "seconds": round(now() - t0, 1)}
        if any(e["kind"] == "ack" and e["what"] == "accepted" for e in got):
            verdict = "accepted (no completion yet)"
            end2 = now() + 60
            while now() < end2:
                got = [e for e in EVENTS.recent(limit=400) if e["t"] >= t0 and e["kind"] in ("ack", "nack", "hk", "frame", "beacon")]
                if any(e["kind"] == "ack" and e["what"] == "completed" for e in got):
                    return {"command": cmd_id, "sent": text, "events": got, "attempts": attempt, "verdict": "completed", "seconds": round(now() - t0, 1)}
                time.sleep(0.3)
            break
    return {"command": cmd_id, "sent": text, "events": got, "attempts": attempt, "verdict": verdict, "seconds": round(now() - t0, 1)}


def start_job(cmd_id, args):
    _job_seq[0] += 1
    jid = _job_seq[0]
    job = {"id": jid, "command": cmd_id, "args": args, "state": "running", "t": now(), "result": None}
    JOBS[jid] = job
    BUS.publish("job", **job)

    def work():
        try:
            job["result"] = run_command(cmd_id, args)
            job["state"] = "done"
        except Exception as e:
            job["result"] = {"verdict": "error", "error": str(e)}
            job["state"] = "error"
        BUS.publish("job", **job)

    threading.Thread(target=work, daemon=True).start()
    return jid


def config_summary():
    cfg = CORE.load_config()
    key = CORE.read_key()
    return {"config": cfg, "defaults": CORE.DEFAULT_CONFIG,
            "menus": {k: {"title": t, "items": [i for i in items if i["kind"] not in ("action", "menu", "info")]} for k, (t, items) in CORE.MENUS.items()},
            "key_fingerprint": CORE.key_fingerprint(key) if key else None,
            "files": {n: os.path.exists(f) for n, f in (("secrets.h", CORE.SECRETS_H), ("s2s2_config.h", CORE.CONFIG_H), ("s2s2.mk", CORE.CONFIG_MK))}}


def config_set(changes):
    cfg = CORE.load_config()
    trial = dict(cfg)
    for k, v in changes.items():
        if k not in CORE.DEFAULT_CONFIG:
            raise RuntimeError(f"unknown option {k}")
        trial[k] = v
    errs = CORE.validate_config(trial)
    if errs:
        raise RuntimeError("; ".join(errs))
    CORE.save_config(trial)
    changed = CORE.generate(trial)
    log(f"configuration saved ({', '.join(changes)}); generated: {', '.join(changed) or 'unchanged'}  -> rebuild and reflash to apply", "ok")
    return config_summary()
