"""mc.link -- link supervision: periodic heartbeat (PING) and automatic sampling during an overpass.

Switched on by the operator (it transmits by itself, so it is off after a fresh install). Every 30 to 40 s the ground station then sends a PING. An answer (ACK accepted, followed by the text "ACK: PING OK (OBC_ONLINE)") proves that the link is up, that the satellite is
listening (command mode) and that it responds. Last contact and RSSI shown by the interface come from these answers. While the satellite is above the horizon (pass prediction from the
TLE) and responsive, a health report (HK) and one block of the ADCS data file are requested every 30 s. Heartbeat and sampling pause while a command of the operator is being served
(the radio is half duplex and the satellite hears one thing at a time).
"""
import collections
import random
import threading
import time

from mc import core, passes

DEFAULTS = {"enabled": False, "autosample": True, "min_s": 30, "max_s": 40, "sample_s": 30}
STATE = {"last_t": 0.0, "last_ok_t": 0.0, "rtt": None, "rssi": None, "snr": None, "next_due": 0.0, "hist": collections.deque(maxlen=60), "sampled_t": 0.0, "sample_basis": "", "busy": False}


def cfg():
    return {**DEFAULTS, **(core.setting("link_cfg", {}) or {})}


def set_cfg(**kw):
    c = cfg()
    for k in ("enabled", "autosample"):
        if k in kw:
            c[k] = bool(kw[k])
    core.set_setting("link_cfg", c)
    STATE["next_due"] = 0.0
    return state()


def _wait(pred, timeout):
    end = time.time() + timeout
    while time.time() < end:
        r = pred()
        if r:
            return r
        time.sleep(0.2)
    return None


def beat():
    """One heartbeat. Returns the record, or None when nothing was sent (no ground station, or a command is being served)."""
    if not core.GS or not core.GS.connected() or not core.JQ.empty() or core.CUR["id"] is not None:
        return None
    if not core.TX.acquire(blocking=False):
        return None
    try:
        STATE["busy"] = True
        t0 = time.time()
        core.GS.send("PING")
        ack = _wait(lambda: next((e for e in core.EVENTS.recent(limit=60) if e["kind"] == "ack" and e["t"] >= t0 - 0.2), None), 12)
        ok = bool(ack and ack.get("what") == "accepted") or bool(ack)
        if ok:
            _wait(lambda: next((e for e in core.EVENTS.recent(limit=60) if e["kind"] == "ascii" and e["t"] >= t0 - 0.2), None), 3)     # the ping text, a moment later
        rs = core.rssi_near(ack["t"] if ok else t0, 4)
        rec = {"t": t0, "ok": ok, "rtt": round(ack["t"] - t0, 2) if ok else None, "rssi": rs["rssi"] if rs else None, "snr": rs["snr"] if rs else None}
        STATE.update(last_t=t0, rtt=rec["rtt"])
        if ok:
            STATE.update(last_ok_t=ack["t"], rssi=rec["rssi"], snr=rec["snr"])
            core.STATE["last_contact"] = ack["t"]
        STATE["hist"].append(rec)
        with core.DBL:
            core.DB.execute("INSERT INTO hb VALUES (?,?,?,?,?)", (t0, int(ok), rec["rtt"], rec["rssi"], rec["snr"]))
            core.DB.commit()
        core.BUS.publish("hb", hb=state())
        return rec
    finally:
        STATE["busy"] = False
        core.TX.release()


def state():
    c = cfg()
    now = time.time()
    h = list(STATE["hist"])[-10:]
    pn = passes.predict(now - 3600, 50)
    cur = next((p for p in pn["passes"] if p["aos"] <= now <= p["los"]), None)
    nxt = next((p for p in pn["passes"] if p["aos"] > now), None)
    responsive = bool(STATE["last_ok_t"]) and now - STATE["last_ok_t"] < c["max_s"] * 2.5 + 12
    return {"enabled": c["enabled"], "autosample": c["autosample"], "responsive": responsive and c["enabled"], "command_mode": responsive and c["enabled"],
            "last_t": STATE["last_t"] or None, "last_ok_t": STATE["last_ok_t"] or None, "rtt": STATE["rtt"], "rssi": STATE["rssi"], "snr": STATE["snr"],
            "success": round(100.0 * sum(1 for r in h if r["ok"]) / len(h)) if h else None, "sent": len(h), "next_due": STATE["next_due"] or None, "busy": STATE["busy"],
            "in_pass": bool(cur), "pass": cur, "next_pass": nxt, "pass_reason": pn.get("reason", ""), "sample_basis": STATE["sample_basis"], "last_sample": STATE["sampled_t"] or None,
            "hist": [{"t": r["t"], "ok": r["ok"]} for r in list(STATE["hist"])[-30:]], "gs": bool(core.GS and core.GS.connected())}


def _sample():
    c = cfg()
    if not c["autosample"] or not core.GS or not core.GS.connected():
        return
    st = state()
    if not st["responsive"]:
        return
    pn = passes.predict(time.time() - 3600, 50)
    if pn["passes"] or not pn.get("reason"):
        STATE["sample_basis"] = "overpass"
        if not st["in_pass"]:
            return
    else:
        STATE["sample_basis"] = "responsive only (%s)" % pn["reason"]
    if not core.JQ.empty() or core.CUR["id"] is not None:
        return
    STATE["sampled_t"] = time.time()
    for cid, args in (("health", {}), ("adcs_file", {"count": 1})):
        try:
            core.submit("autosample", cid, args)
        except Exception as e:
            core.audit("autosample", "skipped", str(e))
            return


def _loop():
    next_sample = 0.0
    while True:
        time.sleep(1)
        try:
            c = cfg()
            now = time.time()
            if c["enabled"] and now >= STATE["next_due"]:
                beat()
                STATE["next_due"] = time.time() + random.uniform(c["min_s"], c["max_s"])
            if now >= next_sample:
                next_sample = now + c["sample_s"]
                _sample()
        except Exception as e:
            print("link loop:", e)


def start():
    STATE["next_due"] = time.time() + 15
    threading.Thread(target=_loop, daemon=True).start()
