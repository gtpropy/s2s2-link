"""mc.daysum -- the day summary: what an operator or a developer needs first when something about a day has to be understood.

Written into every day folder as Storage/DAY_SUMMARY/ (regenerated with the rest of the folder):

    Day_Summary.xlsx    sheets: At a glance, Contacts (one row per predicted pass: heard or not, first / last signal, offset to the prediction, signal strength,
                        beacons expected / received), Anomalies (everything that deserves a look, with a severity), Satellite health (min / max / last of every value,
                        limit breaches, reboots, sequence gaps), Link quality (per hour), Commands (per command type), Environment (software, orbit data, station,
                        limits, disk, Google Drive), Operator actions (audit log of the day)
    Day_Summary.txt     the same top items as plain text
    environment.json    machine readable environment (what produced this day)
    README.txt          which file answers which question

Everything is computed from the databases (events, commands, heartbeats, received-signal log, audit), never from the screen, so it can be rebuilt at any time.
"""
import datetime
import json
import os
import platform
import shutil
import subprocess
import sys
import time

from mc import core, gdrive, xlsxio

BEACON_PERIOD = 90.0                                     # the satellite's CW beacon period (s)
PAD = 120.0                                              # a signal this close to a predicted pass still belongs to it (s)
HK_KEYS = [("bat_v", "Battery voltage", "V"), ("bus_v", "Bus voltage", "V"), ("sol_v", "Solar voltage", "V"), ("bat_i", "Battery current", "A"), ("bat_t", "Battery temperature", "C"),
           ("ant_t", "Antenna temperature", "C"), ("bpb_t", "Backplane temperature", "C"), ("unreg_i", "Unregulated current", "A"), ("i_3v3", "3.3 V current", "A"), ("i_5v", "5 V current", "A")]
SEV = {"ERROR": 0, "WARN": 1, "INFO": 2}
_git = {"t": 0.0, "v": None}

README = """S2S GS day folder -- where to look

GS_report.xlsx                       commands, overpasses, heartbeat log and signal at each command of the day (the operations team's report)
Storage/DAY_SUMMARY/Day_Summary.xlsx  START HERE. 'At a glance', 'Anomalies' (everything odd, with severity), 'Contacts' (every predicted pass: heard or not, offset to the
                                      prediction), 'Satellite health', 'Link quality', 'Commands', 'Environment', 'Operator actions'
Storage/DAY_SUMMARY/Day_Summary.txt   the same top items as text
Storage/DAY_SUMMARY/environment.json  software version, orbit data (TLE age), station, limits, disk: what produced this day
Storage/LOG_DETAILS/<day>/*_LOG_raw.txt  every line the ground station printed, time stamped (UTC): the ground truth when anything is unclear
Storage/Mission_Details/<mission>/    per mission: operation_raw.log (each command with the ground station output of that moment), <mission>.xlsx, data/command_response_*
Storage/HK_DETAILS, BEACON_DETAIL, ...  raw bytes and decoded values of every packet

Reading hints
- Pass 'NOT HEARD' with signals outside any pass window  ->  the orbit data (TLE) is old or the PC clock / station position is wrong.
- 'AOS offset' of more than a minute  ->  same cause.
- Satellite reboot (hours-since-reset falls, HK sequence restarts)  ->  the replay counters restart: send RESET before the next authenticated command.
- Many 'no response' and NACK 0x07 (busy)  ->  commands were sent during the CW beacon (about 30 s of every 90 s the satellite does not listen).
- Time stamps in the files are UTC; folder names are local time.
"""


def _utc(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc)


def stamp(t):
    return _utc(t).strftime("%Y/%m/%d %H:%M:%S") if t else ""


def _stats(v):
    return (min(v), sum(v) / len(v), max(v)) if v else (None, None, None)


def _r(x, n=1):
    return round(x, n) if isinstance(x, (int, float)) else x


def git_info():
    if time.time() - _git["t"] < 600 and _git["v"]:
        return _git["v"]
    v = {"commit": "", "dirty_files": None}
    g = shutil.which("git")
    if g:
        try:
            v["commit"] = subprocess.run([g, "rev-parse", "--short", "HEAD"], cwd=core.ROOT, capture_output=True, text=True, timeout=5).stdout.strip()
            v["dirty_files"] = len([x for x in subprocess.run([g, "status", "--porcelain"], cwd=core.ROOT, capture_output=True, text=True, timeout=5).stdout.splitlines() if x.strip()])
        except (OSError, subprocess.SubprocessError):
            pass
    _git.update(t=time.time(), v=v)
    return v


def tle_epoch(tle):
    try:
        l1 = tle["line1"]
        yy = int(l1[18:20])
        d0 = datetime.datetime(2000 + yy if yy < 57 else 1900 + yy, 1, 1, tzinfo=datetime.timezone.utc)
        return (d0 + datetime.timedelta(days=float(l1[20:32]) - 1)).timestamp()
    except (KeyError, TypeError, ValueError):
        return None


def environment(d):
    tle = core.setting("tle", None) or {}
    st = core.setting("station", None)
    ep = tle_epoch(tle)
    try:
        du = shutil.disk_usage(core.ROOT)
        disk = {"free_gb": round(du.free / 1e9, 2), "total_gb": round(du.total / 1e9, 1)}
    except OSError:
        disk = {}
    gs = core.GS
    with core.DBL:                                                       # what the ground station printed when it last started: frequencies, board, key fingerprint
        boot = [r[0] for r in core.DB.execute("SELECT t || ' ' || text FROM raw WHERE t > ? AND (text LIKE '%Radio RX =%' OR text LIKE '%Radio TX =%' OR text LIKE '%[BOARD]%' OR text LIKE '% key %') "
                                              "ORDER BY t DESC LIMIT 8", (time.time() - 3 * 86400,)).fetchall()]
    tz = datetime.datetime.now().astimezone()
    return {"generated_utc": stamp(time.time()), "day_folder": d["name"], "software": {"git": git_info(), "python": sys.version.split()[0], "platform": platform.platform()},
            "clock": {"timezone": tz.strftime("%Z %z"), "note": "time stamps in the files are UTC; the PC clock is trusted (check it against a time server when a pass was missed)"},
            "ground_station": {"connected": bool(gs and gs.connected()), "port": getattr(gs, "port", None), "last_error": getattr(gs, "error", None), "last_boot_lines": boot[::-1]},
            "orbit": {"name": tle.get("name"), "norad": tle.get("norad"), "source": tle.get("source"), "fetched_utc": stamp(tle.get("fetched")) if tle.get("fetched") else None,
                      "epoch_utc": stamp(ep) if ep else None, "epoch_age_days": round((time.time() - ep) / 86400, 1) if ep else None, "error": tle.get("error") or None},
            "station": st or "default position (not set by the operator)", "launch_utc": stamp(core.setting("launch", None)) if core.setting("launch", None) else None,
            "limits": core.setting("limits", {}), "link_supervision": core.setting("link", {}), "disk": disk,
            "google_drive": {"configured": gdrive.configured(), "uploaded": bool((core.setting("drive_uploaded", {}) or {}).get(d["name"]))},
            "counters_since_program_start": dict(core.STATE["counters"])}


def _health(hk, bc, limits):
    rows = [["Value", "Unit", "Samples", "Min", "Mean", "Max", "Last", "Last at (UTC)", "Limit low", "Limit high", "Samples outside limits"]]
    breaches = []
    for k, label, unit in HK_KEYS:
        pts = [(e["t"], e[k]) for e in hk if isinstance(e.get(k), (int, float))]
        vals = [v for _, v in pts]
        lim = limits.get(k) or {}
        lo, hi = lim.get("lo"), lim.get("hi")
        out = [(t, v) for t, v in pts if (lo is not None and v < lo) or (hi is not None and v > hi)]
        mn, av, mx = _stats(vals)
        rows.append([label, unit, len(vals), _r(mn, 2), _r(av, 2), _r(mx, 2), _r(vals[-1], 2) if vals else None, stamp(pts[-1][0]) if pts else "", lo, hi, len(out)])
        if out:
            breaches.append((out[0][0], label, "%d sample(s) outside the limit (%s..%s %s); first %.2f at %s" % (len(out), lo, hi, unit, out[0][1], stamp(out[0][0]))))
    for e in hk[-1:]:
        for fl in ("flag1", "flag2"):
            if e.get(fl):
                rows.append(["Status word " + fl, "", 1, None, None, None, e[fl], stamp(e["t"]), None, None, ""])
    return rows, breaches


def _reboots(hk, bc):
    """Times at which the satellite restarted: the hours-since-reset of the beacon fell, or the housekeeping sequence number restarted."""
    out = []
    prev = None
    for e in bc:
        b1 = e.get("b1") or {}
        if b1.get("system") != "?" and 2 <= b1.get("bat_v", 0) <= 10:
            h = b1.get("hours_since_reset")
            if prev is not None and isinstance(h, (int, float)) and h < prev[1]:
                out.append((e["t"], "beacon: hours since reset fell from %s to %s" % (prev[1], h)))
            if isinstance(h, (int, float)):
                prev = (e["t"], h)
    ps = None
    for e in hk:
        s = e.get("seq")
        if isinstance(s, int):
            if ps is not None and s < ps and ps - s > 1:
                out.append((e["t"], "housekeeping sequence restarted (%d after %d)" % (s, ps)))
            ps = s
    return sorted(out)


def _seq_gaps(hk):
    miss, runs, ps = 0, 0, None
    for e in hk:
        s = e.get("seq")
        if isinstance(s, int):
            if ps is not None and 1 < s - ps < 1000:
                miss += s - ps - 1
                runs += 1
            ps = s
    return miss, runs


def _contacts(pv, rx, hb, bc, hk, jobs, t0, t1):
    rows = [["Pass AOS (UTC)", "Pass LOS (UTC)", "Predicted length (min)", "Max elevation (deg)", "Heard", "First signal (UTC)", "Last signal (UTC)", "AOS offset (s)", "Heard for (min)",
             "Packets", "RSSI min", "RSSI mean", "RSSI max", "SNR mean", "Beacons expected", "Beacons received", "Housekeeping packets", "Heartbeats", "Heartbeats answered", "Commands", "Commands completed"]]
    inside, notes = [], []
    for p in pv:
        if not (t0 <= p["aos"] < t1):
            continue
        a, b = p["aos"] - PAD, p["los"] + PAD
        inside.append((a, b))
        s = [x for x in rx if a <= x[0] <= b]
        hbs = [h for h in hb if p["aos"] <= h[0] <= p["los"]]
        pj = [j for j in jobs if p["aos"] <= j["t"] <= p["los"]]
        nb = sum(1 for e in bc if a <= e["t"] <= b)
        first, last = (s[0][0], s[-1][0]) if s else (None, None)
        rs = [x[1] for x in s]
        mn, av, mx = _stats(rs)
        sn = [x[2] for x in s if x[2] is not None]
        rows.append([stamp(p["aos"]), stamp(p["los"]), _r((p["los"] - p["aos"]) / 60), _r(p["maxEl"]), "YES" if s else "NO", stamp(first), stamp(last), _r(first - p["aos"], 0) if first else "",
                     _r((last - first) / 60) if first else "", len(s), mn, _r(av), mx, _r(sum(sn) / len(sn)) if sn else "", int((p["los"] - p["aos"]) // BEACON_PERIOD), nb,
                     sum(1 for e in hk if a <= e["t"] <= b), len(hbs), sum(1 for h in hbs if h[1]), len(pj), sum(1 for j in pj if j["state"] in ("completed", "sent"))])
        if not s and p["maxEl"] >= 15 and p["los"] < time.time():
            notes.append((p["aos"], "WARN", "Contact", "Predicted pass (max %.0f deg) without any signal" % p["maxEl"], "%s .. %s UTC" % (stamp(p["aos"]), stamp(p["los"]))))
        elif s and abs(first - p["aos"]) > 90 and p["maxEl"] >= 15:
            notes.append((first, "INFO", "Contact", "Signal started %+d s from the predicted AOS" % round(first - p["aos"]), "orbit data age, PC clock or station position may be off"))
    return rows, inside, notes


def _link_hours(rx, hb):
    rows = [["Hour (UTC)", "Packets received", "RSSI min", "RSSI mean", "RSSI max", "SNR mean", "Heartbeats", "Heartbeats answered"]]
    byh = {}
    for t, r, s in rx:
        byh.setdefault(int(t // 3600), []).append((r, s))
    hbh = {}
    for h in hb:
        hbh.setdefault(int(h[0] // 3600), []).append(h[1])
    for k in sorted(set(byh) | set(hbh)):
        v = byh.get(k, [])
        rs = [x[0] for x in v]
        sn = [x[1] for x in v if x[1] is not None]
        mn, av, mx = _stats(rs)
        rows.append([_utc(k * 3600).strftime("%Y/%m/%d %H:00"), len(v), mn, _r(av), mx, _r(sum(sn) / len(sn)) if sn else "", len(hbh.get(k, [])), sum(1 for o in hbh.get(k, []) if o)])
    return rows


def _commands(jobs, evs):
    rows = [["Command", "Sent", "Completed", "NACK", "No response", "Needed repeats", "Median duration (s)", "Slowest (s)"]]
    names = sorted({j["label"] for j in jobs})
    for n in names:
        js = [j for j in jobs if j["label"] == n]
        ok = [j for j in js if j["state"] in ("completed", "sent")]
        du = sorted((j["t_end"] - j["t"]) for j in ok if j["t_end"])
        rows.append([n, len(js), len(ok), sum(1 for j in js if j["state"] == "nack"), sum(1 for j in js if j["state"] == "timeout"), sum(1 for j in js if (j["detail"].get("attempt") or 1) > 1),
                     _r(du[len(du) // 2]) if du else "", _r(du[-1]) if du else ""])
    return rows


def _anomalies(jobs, evs, hk, bc, breaches, reboots, audit, tle_env, store_err, images, out_of_pass):
    a = []
    for j in jobs:
        det = j["detail"] or {}
        if j["state"] == "nack":
            nk = next((e for e in (det.get("events") or []) if e.get("kind") == "nack"), None)
            a.append((j["t"], "WARN", "Command", "%s rejected by the satellite" % j["label"], ("NACK 0x%02X %s" % (nk.get("err") or 0, nk.get("name", ""))) if nk else ""))
        elif j["state"] == "timeout":
            a.append((j["t"], "WARN", "Command", "%s: no response" % j["label"], "attempts %s; the satellite does not listen during its CW beacon" % (det.get("attempt") or 1)))
        elif j["state"] in ("incomplete", "error"):
            a.append((j["t"], "ERROR", "Command", "%s %s" % (j["label"], j["state"]), str(det.get("error", ""))))
        elif (det.get("attempt") or 1) >= 3:
            a.append((j["t"], "INFO", "Command", "%s needed %s attempts" % (j["label"], det.get("attempt")), ""))
    for t, w, d in breaches:
        a.append((t, "WARN", "Limit", w, d))
    for t, what in reboots:
        a.append((t, "ERROR", "Satellite", "Satellite restarted", what))
    bad = [e for e in hk if e.get("crc_ok") is False]
    if bad:
        a.append((bad[0]["t"], "WARN", "Housekeeping", "%d housekeeping packet(s) with a wrong checksum" % len(bad), "first at " + stamp(bad[0]["t"])))
    miss, runs = _seq_gaps(hk)
    if miss:
        a.append((hk[0]["t"], "INFO", "Housekeeping", "%d housekeeping packet(s) missing in the sequence (%d gaps)" % (miss, runs), "lost packets or silent stretches between passes"))
    for t, act, det in audit:
        if act in ("ground station disconnected",):
            a.append((t, "ERROR", "Ground station", "Ground station disconnected", det))
        elif act == "program started":
            a.append((t, "INFO", "Program", "Mission Control started", det))
        elif act in ("login failed",):
            a.append((t, "WARN", "Security", "Failed login", det))
    for s in images:
        if s.get("total") and s.get("got", 0) < s["total"]:
            a.append((s.get("t_last") or s.get("t_first") or 0, "WARN", "Camera", "Picture %s incomplete: %d of %d fragments" % (s["id"], s["got"], s["total"]), "missing %s" % ((s.get("missing") or [])[:12],)))
        elif s.get("complete") and s.get("matches_sent") is False:
            a.append((s.get("t_last") or 0, "ERROR", "Camera", "Picture %s does not match the announced size / checksum" % s["id"], ""))
    if out_of_pass:
        a.append((out_of_pass[0], "WARN", "Contact", "%d packet(s) received outside every predicted pass" % len(out_of_pass), "first at %s; orbit data (TLE), PC clock or station position is probably wrong" % stamp(out_of_pass[0])))
    ep = tle_env.get("epoch_age_days")
    if ep is None:
        a.append((time.time(), "WARN", "Orbit", "No orbit data (TLE): passes cannot be predicted", ""))
    elif ep > 7:
        a.append((time.time(), "WARN", "Orbit", "Orbit data is %.1f days old" % ep, "predictions get worse; refresh it in Settings"))
    if store_err:
        a.append((time.time(), "ERROR", "Store", "The data store reported an error", store_err))
    return sorted(a, key=lambda x: (SEV[x[1]], x[0]))


def build(d, jobs, evs, hk, bc, hb, pv, write, flush_error=""):
    """Compute the summary of day d and write the files with write(rel_path, fn, *args) (store._w)."""
    t0, t1 = d["t0"], d["t1"]
    with core.DBL:
        rx = core.DB.execute("SELECT t,rssi,snr FROM rx WHERE t >= ? AND t < ? ORDER BY t", (t0, t1)).fetchall()
        audit = core.DB.execute("SELECT t,action,detail FROM audit WHERE t >= ? AND t < ? ORDER BY t", (t0, t1)).fetchall()
        who = core.DB.execute("SELECT t,user,action,detail FROM audit WHERE t >= ? AND t < ? ORDER BY t", (t0, t1)).fetchall()
    counts = core.EVENTS.counts(t0, t1)
    limits = core.setting("limits", {}) or {}
    env = environment(d)
    health, breaches = _health(hk, bc, limits)
    reboots = _reboots(hk, bc)
    contacts, windows, cnotes = _contacts(pv, rx, hb, bc, hk, jobs, t0, t1)
    out_of_pass = [x[0] for x in rx if not any(a <= x[0] <= b for a, b in windows)] if windows else []
    images = [s for s in core.IMAGES.list() if t0 <= (s.get("t_first") or 0) < t1]
    anomalies = _anomalies(jobs, evs, hk, bc, breaches, reboots, audit, env["orbit"], flush_error, images, out_of_pass if windows else [])
    anomalies += [(t, sv, src, w, dd) for t, sv, src, w, dd in cnotes]
    anomalies.sort(key=lambda x: (SEV[x[1]], x[0]))
    heard = sum(1 for r in contacts[1:] if r[4] == "YES")
    npass = len(contacts) - 1
    rs = [x[1] for x in rx]
    mn, av, mx = _stats(rs)
    done = [j for j in jobs if j["state"] in ("completed", "sent")]
    bv = [e["bat_v"] for e in hk if isinstance(e.get("bat_v"), (int, float))]
    last_rx = rx[-1][0] if rx else None
    nerr, nwarn = sum(1 for x in anomalies if x[1] == "ERROR"), sum(1 for x in anomalies if x[1] == "WARN")
    glance = [["Item", "Value", "Why it matters"],
              ["Day folder", d["name"], ""],
              ["Needs attention", "%d error(s), %d warning(s)" % (nerr, nwarn) if (nerr or nwarn) else "nothing flagged", "see sheet Anomalies"],
              ["Passes predicted today", npass, "needs orbit data and the station position"],
              ["Passes heard", "%d of %d" % (heard, npass) if npass else "", "a pass not heard although the satellite was up points to the antenna, the radio or the orbit data"],
              ["Last signal from the satellite", stamp(last_rx), "time of the last received packet today"],
              ["Packets received", len(rx), "every packet the ground station reported with a signal strength"],
              ["Signal strength min / mean / max (dBm)", "%s / %s / %s" % (mn, _r(av), mx) if rs else "", "a drop of several dB from one day to the next means antenna / cable / pointing"],
              ["Satellite restarts seen", len(reboots), "a restart resets the replay counters and the satellite clock"],
              ["Commands sent / completed", "%d / %d" % (len(jobs), len(done)), ""],
              ["Commands rejected / no answer", "%d / %d" % (sum(1 for j in jobs if j["state"] == "nack"), sum(1 for j in jobs if j["state"] == "timeout")), "no answer is usually a command sent during the CW beacon"],
              ["Beacons received", len(bc), "expected about one per 90 s while in contact"],
              ["Housekeeping packets", len(hk), ""],
              ["Battery voltage min / max (V)", "%.2f / %.2f" % (min(bv), max(bv)) if bv else "", "from the housekeeping packets"],
              ["Pictures started / complete", "%d / %d" % (len(images), sum(1 for s in images if s.get("complete"))), ""],
              ["Packets by type (stored)", ", ".join("%s %d" % (k, v) for k, v in sorted(counts.items())), "what the database holds for this day"],
              ["Orbit data age (days)", env["orbit"]["epoch_age_days"], "over 7 days predictions degrade"],
              ["Software", "%s%s" % (env["software"]["git"]["commit"] or "unknown", " + %s changed files" % env["software"]["git"]["dirty_files"] if env["software"]["git"]["dirty_files"] else ""), "which code produced this day"],
              ["Google Drive", "uploaded" if env["google_drive"]["uploaded"] else ("not uploaded yet" if env["google_drive"]["configured"] else "not configured"), "the local folder is the only copy until uploaded"]]
    arows = [["Severity", "Time (UTC)", "Source", "What", "Detail"]] + [[sv, stamp(t), src, w, dd] for t, sv, src, w, dd in anomalies]
    erows = [["Item", "Value"]]

    def flat(prefix, v):
        if isinstance(v, dict):
            for k, x in v.items():
                flat(prefix + "." + str(k) if prefix else str(k), x)
        else:
            erows.append([prefix, json.dumps(v) if isinstance(v, (list, tuple)) else v])
    flat("", env)
    oprows = [["Time (UTC)", "User", "Action", "Detail"]] + [[stamp(t), u, a, dd] for t, u, a, dd in who]
    sheets = [("At a glance", glance), ("Anomalies", arows), ("Contacts", contacts), ("Satellite health", health), ("Link quality", _link_hours(rx, hb)),
              ("Commands", _commands(jobs, evs)), ("Environment", erows), ("Operator actions", oprows)]
    write("Storage/DAY_SUMMARY/Day_Summary.xlsx", xlsxio.write_xlsx, sheets)
    txt = ["S2S ground station day summary, %s" % d["name"], "generated %s UTC" % stamp(time.time()), ""]
    txt += ["%-42s %s" % (r[0], r[1]) for r in glance[1:]]
    txt += ["", "ANOMALIES (%d)" % len(anomalies)] + ["  [%s] %s  %s: %s%s" % (x[1], stamp(x[0]), x[2], x[3], (" -- " + x[4]) if x[4] else "") for x in anomalies[:80]]
    txt += ["", "PASSES"] + ["  %s  max %s deg  heard %s%s" % (r[0], r[3], r[4], ("  first signal %s (offset %s s), %s packets" % (r[5], r[7], r[9])) if r[4] == "YES" else "") for r in contacts[1:]]
    write("Storage/DAY_SUMMARY/Day_Summary.txt", _wtxt, "\n".join(txt) + "\n")
    write("Storage/DAY_SUMMARY/environment.json", _wtxt, json.dumps(env, indent=2, default=str) + "\n")
    write("Storage/DAY_SUMMARY/README.txt", _wtxt, README)
    return {"anomalies": len(anomalies), "errors": nerr, "warnings": nwarn, "heard": heard, "passes": npass}


def _wtxt(p, text):
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)
