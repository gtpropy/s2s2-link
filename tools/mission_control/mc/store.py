"""mc.store -- the ground station's permanent data store (GS_Data/), laid out like the operations team's report folders.

    GS_Data/<YYYY-MM-DD, HH_MMAM>/                                                  one folder per day, created automatically; uploaded to the Google Drive folder "S2S_operational_reports"
        S2S_OPERATIONAL_REPORT_<date>_<time>.docx            blank template the operator fills in by hand (never overwritten)
        S2S_OPERATIONAL_REPORT_<date>_<time>_AUTOFILL.docx   the same report filled with this day's data (regenerated)
        GS_report.xlsx                                       commands sent / received, overpasses, link (heartbeat) log, signal strength at every command
        Storage/ADDRESS_DETAILS/Address_Data.xlsx           commands per mission (time stamp, command bytes, number of packets)
        Storage/BEACON_DETAIL/Beacon_details.xlsx            every CW beacon (raw bytes, type, decoded values)
        Storage/LOG_DETAILS/<YYYYMMDD>/<YYYYMMDD_HHMMSS>_LOG_raw.txt   every line the ground station printed
        Storage/HK_DETAILS, CAM_DETAILS(+CAM_LOGS), MSN_LOG, EPDM_DETAILS, ADCS_DETAILS, DIGIPEATER_DETAILS     per data type, raw bytes and decoded values
        Storage/Mission_Details/{ADCS,EPDM,CAM,DPM}/         operation_raw.log, <mission>.xlsx (commands + statistics), data/command_response_<ts>.txt (+ image_stored_<ts>.jpeg for CAM)
        Storage/DAY_SUMMARY/                                 Day_Summary.xlsx / .txt (anomalies, passes heard, link quality, satellite health, environment), environment.json, README.txt: mc/daysum.py

Everything except the raw log is regenerated from the databases (commands, events, heartbeat), so a restart loses nothing and a file is never half written.
File time stamps (TS) are UTC like in the reference reports; folder names use local time.
"""
import datetime
import json
import os
import re
import shutil
import threading
import time

from mc import core, daysum, docxgen, passes, xlsxio

DATA_ROOT = os.environ.get("S2S_GS_DATA") or os.path.join(core.ROOT, "GS_Data")
REPORTS = DATA_ROOT                                      # the day folders live directly in GS_Data
PULLED = os.path.join(DATA_ROOT, "_pulled")
DEFAULTS = {"retention_days": 7, "delete_requires_upload": True}
MISSIONS = ["ADCS", "EPDM", "CAM", "DPM"]
MISSION_OF = {"adcs": "ADCS", "adcs_file": "ADCS", "epdm": "EPDM", "epdm_file": "EPDM", "cam_run": "CAM", "cam_dl": "CAM", "dummycam": "CAM", "digi": "DPM"}
GROUP_OF = {"health": "HK Data", "hk2": "HK Data", "hkdl": "HK Data", "adcs": "ADCS", "adcs_file": "ADCS", "epdm": "EPDM", "epdm_file": "EPDM",
            "cam_run": "Camera", "cam_dl": "Camera", "dummycam": "Camera", "digi": "Digipeater"}
FIXED = {"health": "53 01 1D D1 F2 00 F2 00 00 00 00 00 01", "ping": "53 01 1D D1 F5 00 00 00 00 00 00 00 01", "hk2": "53 01 1D D1 F8 00 F8 00 00 00 00 00 01",
         "adcs": "53 03 A0 53 CF 00 00 00 00 00 00 00 00", "epdm": "53 05 EC CF CF 00 00 00 00 00 00 00 00", "cam_run": "53 04 CC 5E BD 00 00 00 00 00 00 00 00",
         "cam_dl": "53 01 1D D2 F5 00 00 00 00 00 00 00 0A", "dummycam": "53 04 CC 5E BE 00 D0 00 00 00 00 00 45", "antenna": "53 01 DE EF AA 00 00 00 00 00 00 00 00"}
FILES = {"hkdl": ("D1", "F8"), "adcs_file": ("D2", "F7"), "epdm_file": ("D2", "F6"), "satlog": ("D1", "F3"), "flags": ("D1", "F1"), "rsvtable": ("D1", "F4")}
_lock = threading.RLock()
_rx, _rxl = [], threading.Lock()
_state = {"dirty": False, "log_path": None, "log_f": None, "last_flush": 0.0, "last_error": ""}


def cfg():
    return {**DEFAULTS, **(core.setting("store_cfg", {}) or {})}


def utc_dt(t):
    return datetime.datetime.fromtimestamp(t, datetime.timezone.utc)


def ts_file(t):
    return utc_dt(t).strftime("%Y%m%d_%H%M%S")


def stamp(t):
    return utc_dt(t).strftime("%Y/%m/%d %H:%M:%S")


def hexs(b):
    return " ".join("%02X" % x for x in b)


def command_bytes(cmd, line=""):
    """The 13 command bytes the ground station sends for a catalogue command (as defined in the GS firmware)."""
    if cmd in FIXED:
        return FIXED[cmd]
    if cmd in FILES:
        d, f = FILES[cmd]
        m = re.search(r"(\d+)\s*$", line or "")
        n = int(m.group(1)) if m else 5
        return "53 01 1D %s %s 00 %s 00 00 00 00 %02X %02X" % (d, f, f, n >> 8, n & 255)
    return ""


def packets_of(cmd, line):
    m = re.search(r"(\d+)\s*$", line or "")
    if cmd in FILES and m:
        return "%s (DEC)" % m.group(1)
    return {"cam_dl": "69 (DEC)", "dummycam": "69 (DEC)"}.get(cmd, "")


# ----------------------------------------------------------------------------------------------------------------- day folders
def _tree(path, compact):
    for sub in ("Storage/ADDRESS_DETAILS", "Storage/BEACON_DETAIL", "Storage/LOG_DETAILS/" + compact, "Storage/HK_DETAILS", "Storage/CAM_DETAILS/CAM_LOGS", "Storage/MSN_LOG",
                "Storage/EPDM_DETAILS", "Storage/ADCS_DETAILS", "Storage/DIGIPEATER_DETAILS", "Storage/DAY_SUMMARY"):
        os.makedirs(os.path.join(path, sub), exist_ok=True)
    for m in MISSIONS:
        os.makedirs(os.path.join(path, "Storage", "Mission_Details", m, "data"), exist_ok=True)
    old = os.path.join(path, "Mission_Details")                         # folders made before 2026-11-02 kept it next to Storage: move it in
    if os.path.isdir(old):
        shutil.copytree(old, os.path.join(path, "Storage", "Mission_Details"), dirs_exist_ok=True)
        shutil.rmtree(old, ignore_errors=True)


def ensure_day(now=None):
    """The folder of today (local date); created together with its whole tree and the blank report template on first use of a new day."""
    now = now or time.time()
    c = _state.get("day")
    if c and c["t0"] <= now < c["t1"]:
        return c
    ldt = datetime.datetime.fromtimestamp(now).astimezone()
    today = ldt.strftime("%Y-%m-%d")
    with _lock:
        d = core.setting("store_day", None)
        if d and d.get("date") == today and os.path.isdir(d["path"]):
            _tree(d["path"], ldt.strftime("%Y%m%d"))                  # idempotent: adds new sub folders and moves old ones into place
            _state["day"] = d
            return d
        name = ldt.strftime("%Y-%m-%d, %I_%M%p")
        path = os.path.join(REPORTS, name)
        t0 = ldt.replace(hour=0, minute=0, second=0, microsecond=0)
        d = {"date": today, "name": name, "path": path, "ts": ts_file(now), "t0": t0.timestamp(), "t1": (t0 + datetime.timedelta(days=1)).timestamp()}
        _tree(path, ldt.strftime("%Y%m%d"))
        core.set_setting("store_day", d)
        _state["day"] = d
        _close_log()
        try:
            _write_template(d, ldt)
        except OSError as e:
            _state["last_error"] = str(e)
        _state["dirty"] = True
        core.audit("system", "day folder created", d["name"])
        return d


# ----------------------------------------------------------------------------------------------------------------- raw log (live)
def _close_log():
    f = _state.get("log_f")
    if f:
        try:
            f.close()
        except OSError:
            pass
    _state["log_f"] = _state["log_path"] = None


def on_raw(r):
    d = ensure_day(r["t"])
    with _lock:
        if _state["log_f"] is None:
            compact = datetime.datetime.fromtimestamp(d["t0"]).strftime("%Y%m%d")
            p = os.path.join(d["path"], "Storage", "LOG_DETAILS", compact, ts_file(time.time()) + "_LOG_raw.txt")
            os.makedirs(os.path.dirname(p), exist_ok=True)
            _state["log_path"], _state["log_f"] = p, open(p, "a", encoding="utf-8")
        _state["log_f"].write("#(%s) %s\n" % (stamp(r["t"]), r["text"]))
        _state["log_f"].flush()


def on_event(e):
    _state["dirty"] = True
    if e.get("kind") == "rssi" and isinstance(e.get("rssi"), (int, float)):
        with _rxl:
            _rx.append((e["t"], e["rssi"], e.get("snr")))


def _save_rx():
    """Received-signal samples are kept in memory by the event log only; the day summary needs them after a restart, so they go to the database."""
    with _rxl:
        rows, _rx[:] = list(_rx), []
    if rows:
        with core.DBL:
            core.DB.executemany("INSERT INTO rx VALUES (?,?,?)", rows)
            core.DB.commit()


def on_job_done(job):
    _state["dirty"] = True
    flush()


# ----------------------------------------------------------------------------------------------------------------- building the files
def _jobs(t0, t1):
    with core.DBL:
        rows = core.DB.execute("SELECT id,t,user,cmd,line,state,detail,t_end FROM commands WHERE t >= ? AND t < ? ORDER BY id", (t0, t1)).fetchall()
    out = []
    for r in rows:
        c = next((x for x in core.CATALOG if x["id"] == r[3]), {})
        try:
            det = json.loads(r[6] or "{}")
        except ValueError:
            det = {}
        out.append({"id": r[0], "t": r[1], "user": r[2], "cmd": r[3], "line": r[4], "state": r[5], "detail": det, "t_end": r[7], "label": c.get("label", r[3]), "simple": c.get("simple", r[3])})
    return out


def _responses(job, evs):
    """Events that belong to a job (between queueing and a few seconds after the end), oldest first."""
    end = (job["t_end"] or job["t"] + 600) + 8
    kinds = ("ack", "nack", "hk", "ascii", "file", "digi")
    got = [e for e in evs if kinds and e["kind"] in kinds and job["t"] - 0.5 <= e["t"] <= end]
    for e in (job["detail"].get("events") or []):
        if e.get("kind") in kinds and not any(x["kind"] == e["kind"] and abs(x["t"] - e["t"]) < 0.01 for x in got):
            got.append(e)
    return sorted(got, key=lambda e: e["t"])


def _file_bytes(resp):
    ch = {e["pkt"]: e for e in resp if e["kind"] == "file"}
    out = bytearray()
    for k in sorted(ch):
        c = ch[k]
        out += bytes.fromhex(c.get("hex", ""))[: c.get("len") or 128]
    while out and out[-1] == 0:
        out.pop()
    return bytes(out)


def _text(b):
    return "".join(chr(x) if (x in (9, 10, 13) or 32 <= x < 127) else ("" if x == 0 else "·") for x in b)


def _ack_bytes(e):
    if e["kind"] == "ack":
        return "AC %02X %02X" % (0 if e.get("what") == "accepted" else 1, e.get("mission") or 0)
    return "EE %02X %02X" % (e.get("err") or 0, e.get("mission") or 0)


def _response_text(job, resp, t_utc=True):
    """command_response_<ts>.txt: what was sent, what came back (hex + decoded), the result and the signal strength."""
    det = job["detail"]
    rs = det.get("rssi") or {}
    L = ["S2S ground station command response", "=" * 60, "Command            : %s [%s]" % (job["label"], job["cmd"]),
         "Command bytes      : %s" % (command_bytes(job["cmd"], job["line"]) or "(AX.25 UI frame, see ground station line)"), "Ground station line: %s" % job["line"],
         "Sent (UTC)         : %s" % stamp(job["t"]), "Operator           : %s" % job["user"],
         "Result             : %s%s" % (job["state"], " (%d attempt(s), %.1f s)" % (det.get("attempt", 1), (job["t_end"] or job["t"]) - job["t"]) if job["t_end"] else ""),
         "RSSI at send       : %s" % ("%s dBm, SNR %s dB" % (rs["at_send"]["rssi"], rs["at_send"]["snr"]) if rs.get("at_send") else "n/a"),
         "RSSI at response   : %s" % ("%s dBm, SNR %s dB" % (rs["at_response"]["rssi"], rs["at_response"]["snr"]) if rs.get("at_response") else "n/a"), "", "Responses (UTC, hex)", "-" * 60]
    for e in resp:
        tt = utc_dt(e["t"]).strftime("%H:%M:%S")
        if e["kind"] in ("ack", "nack"):
            L.append("%s  %s  %s" % (tt, "ACK " + e.get("what", "") if e["kind"] == "ack" else "NACK 0x%02X %s" % (e.get("err") or 0, e.get("name", "")), _ack_bytes(e)))
        elif e["kind"] == "hk":
            L.append("%s  HK packet seq %s crc %s" % (tt, e.get("seq"), "ok" if e.get("crc_ok") else "FAIL"))
            L.append("    " + e.get("hex", ""))
        elif e["kind"] == "ascii":
            L.append("%s  TEXT  %s" % (tt, e["text"]))
            L.append("    " + hexs(e["text"].encode()))
        elif e["kind"] == "file":
            L.append("%s  FILE block %s/%s addr %s len %s status %s" % (tt, e.get("pkt"), e.get("total"), e.get("addr"), e.get("len"), e.get("status")))
            L.append("    " + hexs(bytes.fromhex(e.get("hex", ""))))
        elif e["kind"] == "digi":
            L.append("%s  DIGI  %s" % (tt, e.get("text")))
    if not resp:
        L.append("(no response received)")
    fb = _file_bytes(resp)
    if fb:
        L += ["", "File content decoded as text (unprocessed)", "-" * 60, _text(fb)]
    if det.get("error"):
        L += ["", "Error: %s" % det["error"]]
    return "\n".join(L) + "\n"


def _stats(jobs):
    n = len(jobs)
    c = lambda s: sum(1 for j in jobs if j["state"] == s)
    durs = [(j["t_end"] - j["t"]) for j in jobs if j["t_end"] and j["state"] in ("completed", "sent")]
    return [["Commands sent", n], ["Completed", c("completed") + c("sent")], ["Rejected (NACK)", c("nack")], ["No response (timeout)", c("timeout")], ["Accepted, not completed", c("incomplete")],
            ["Cancelled / error", c("cancelled") + c("error")], ["Success rate %", round(100.0 * (c("completed") + c("sent")) / n, 1) if n else None],
            ["Average duration s", round(sum(durs) / len(durs), 1) if durs else None]]


def _cmd_rows(jobs, evs):
    rows = [["SN", "Time Stamp (UTC)", "Mission", "Command", "Command bytes", "Packets", "State", "Attempts", "ACK accepted", "ACK completed", "NACK", "Response bytes", "RSSI at send (dBm)", "RSSI at response (dBm)", "Duration (s)", "Operator"]]
    for i, j in enumerate(jobs, 1):
        resp = _responses(j, evs)
        acc = next((e for e in resp if e["kind"] == "ack" and e.get("what") == "accepted"), None)
        fin = next((e for e in resp if e["kind"] == "ack" and e.get("what") == "completed"), None)
        nk = next((e for e in resp if e["kind"] == "nack"), None)
        nbytes = sum(len(bytes.fromhex(e.get("hex", ""))) if e["kind"] in ("hk", "file") else (len(e["text"]) if e["kind"] == "ascii" else 0) for e in resp)
        rs = j["detail"].get("rssi") or {}
        rows.append([i, stamp(j["t"]), MISSION_OF.get(j["cmd"], GROUP_OF.get(j["cmd"], "General")), j["label"], command_bytes(j["cmd"], j["line"]), packets_of(j["cmd"], j["line"]), j["state"],
                     j["detail"].get("attempt", 1), utc_dt(acc["t"]).strftime("%H:%M:%S") if acc else "", utc_dt(fin["t"]).strftime("%H:%M:%S") if fin else "",
                     ("0x%02X %s" % (nk.get("err") or 0, nk.get("name", ""))) if nk else "", nbytes, (rs.get("at_send") or {}).get("rssi"), (rs.get("at_response") or {}).get("rssi"),
                     round((j["t_end"] - j["t"]), 1) if j["t_end"] else "", j["user"]])
    return rows


def _w(day, rel, fn, *a):
    p = os.path.join(day["path"], rel)
    os.makedirs(os.path.dirname(p), exist_ok=True)
    tmp = p + ".tmp"
    fn(tmp, *a)
    os.replace(tmp, p)
    return p


def _wtxt(p, text):
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)


def _data_sheet(kind_code, resp_by_job):
    rows = [["Packet Type", "Packet Number", "Count Packet", "Raw Data", "Decoded Text", "Command time (UTC)"]]
    for j, resp in resp_by_job:
        for e in resp:
            if e["kind"] == "file":
                b = bytes.fromhex(e.get("hex", ""))[: e.get("len") or 128]
                rows.append([kind_code, "%02d" % (e.get("pkt") or 0), e.get("pkt"), hexs(b), _text(b), stamp(j["t"])])
    return rows


def flush(force=False):
    """Regenerate every file of today's folder from the databases. Safe to call at any time and from any thread."""
    with _lock:
        if not force and not _state["dirty"]:
            return
        _state["dirty"] = False
        _save_rx()
        d = ensure_day()
        try:
            _build(d)
            _state["last_flush"] = time.time()
            _state["last_error"] = ""
        except Exception as e:                                           # keep recording even if one file cannot be written
            _state["last_error"] = "%s: %s" % (type(e).__name__, e)
            print("store flush failed:", _state["last_error"])


def _build(d):
    t0, t1 = d["t0"], d["t1"]
    jobs = _jobs(t0, t1)
    evs = core.EVENTS.between(t0 - 5, t1 + 60, ("ack", "nack", "hk", "ascii", "file", "digi", "beacon", "cw"))
    hk = [e for e in evs if e["kind"] == "hk" and t0 <= e["t"] < t1]
    bc = [e for e in evs if e["kind"] == "beacon" and t0 <= e["t"] < t1]
    ts = d["ts"]
    with core.DBL:
        hb = core.DB.execute("SELECT t,ok,rtt,rssi,snr FROM hb WHERE t >= ? AND t < ? ORDER BY t", (t0, t1)).fetchall()
    # ---- Address_Data.xlsx : commands per mission group, side by side like the reference
    groups = ["HK Data", "ADCS", "Camera", "EPDM", "Digipeater"]
    per = {g: [j for j in jobs if GROUP_OF.get(j["cmd"]) == g] for g in groups}
    nrows = max([len(v) for v in per.values()] + [0])
    rows = [[x for g in groups for x in (g, "", "", "")], ["SN", "Time Stamp", "Command", "No of Packets"] * len(groups)]
    for i in range(nrows):
        r = []
        for g in groups:
            j = per[g][i] if i < len(per[g]) else None
            r += [i + 1, "#(%s)" % stamp(j["t"]), command_bytes(j["cmd"], j["line"]) or j["line"], packets_of(j["cmd"], j["line"])] if j else ["", "", "", ""]
        rows.append(r)
    _w(d, "Storage/ADDRESS_DETAILS/Address_Data.xlsx", xlsxio.write_xlsx, [("Address_Information", rows)])
    # ---- Beacon_details.xlsx
    raw = [["SN", "Date", "Time", "Raw Data", "Beacon Type"]]
    b1 = [["SN", "DATE", "TIME", "SYSTEM", "MISSION", "HOURS_SINCE_RESET", "SOL_TOT_I", "SOL_TOT_V (0.01 V)", "BATT_V (V)", "BATT_C (A)", "RAW_DATA"]]
    b2 = [["SN", "DATE", "TIME", "GYRO_X", "GYRO_Y", "GYRO_Z", "MAG_X", "MAG_Y", "MAG_Z", "SOLAR_CELL_BITS", "BATT_T (C)", "RAW_DATA"]]
    for i, e in enumerate(bc, 1):
        u = utc_dt(e["t"])
        h8 = hexs(bytes.fromhex(e["hex"]))
        kind = 1 if e["b1"]["system"] != "?" and 2 <= e["b1"]["bat_v"] <= 10 else 2
        raw.append(["%02d" % i, u.strftime("%Y-%m-%d"), u.strftime("%H:%M:%S"), h8, "TYPE_%d" % kind])
        if kind == 1:
            x = e["b1"]
            b1.append([len(b1), u.strftime("%Y-%m-%d"), u.strftime("%H:%M:%S"), x["system"], x["mission"], x["hours_since_reset"], x["sol_total_i"], x["sol_total_v"], x["bat_v"], x["bat_i"], h8])
        else:
            x = e["b2"]
            b2.append([len(b2), u.strftime("%Y-%m-%d"), u.strftime("%H:%M:%S"), *x["gyro"], *x["magneto"], "".join("1" if v else "0" for v in x["solar_cells"].values()), x["bat_temp_c"], h8])
    _w(d, "Storage/BEACON_DETAIL/Beacon_details.xlsx", xlsxio.write_xlsx, [("Raw_Data", raw), ("Beacon_Type_1", b1), ("Beacon_Type_2", b2)])
    # ---- HK
    hr = [["Packet Type", "Packet Number", "Count Packet", "Raw Data"]] + [["DA", "%02d" % (i % 100), i, hexs(bytes.fromhex(e.get("hex", "")))] for i, e in enumerate(hk, 1)]
    hd = [["SN", "Time Stamp (UTC)", "Sequence", "BAT_V", "SOL_V", "BUS_V", "SP1_V", "SP2_V", "SP3_V", "SP4_V", "SP5_V", "ANT_T", "BAT_T", "BPB_T", "UNREG_I", "I_3V3", "I_5V", "BAT_I", "BUS_I",
           "GYRO_X", "GYRO_Y", "GYRO_Z", "MAG_X", "MAG_Y", "MAG_Z", "CRC_OK", "Raw Data"]]
    for i, e in enumerate(hk, 1):
        pv = e.get("panels_v", {})
        hd.append([i, stamp(e["t"]), e.get("seq"), e.get("bat_v"), e.get("sol_v"), e.get("bus_v"), pv.get("SP1"), pv.get("SP2"), pv.get("SP3"), pv.get("SP4"), pv.get("SP5"), e.get("ant_t"), e.get("bat_t"), e.get("bpb_t"),
                   e.get("unreg_i"), e.get("i_3v3"), e.get("i_5v"), e.get("bat_i"), e.get("bus_i"), *(e.get("gyro") or [None] * 3), *(e.get("mag") or [None] * 3), e.get("crc_ok"), hexs(bytes.fromhex(e.get("hex", "")))])
    _w(d, "Storage/HK_DETAILS/%s_HK_Raw_Data.xlsx" % ts, xlsxio.write_xlsx, [("Raw Data", hr)])
    _w(d, "Storage/HK_DETAILS/%s_HK_Data.xlsx" % ts, xlsxio.write_xlsx, [("Extracted_Data", hd)])
    # ---- file based data types
    for cmds, folder, fname, code, sheet in (({"epdm_file"}, "EPDM_DETAILS", "EPDM_Data_Verification", "F6", "Data"), ({"adcs_file"}, "ADCS_DETAILS", "ADCS_Data_Verification", "F7", "Data"),
                                             ({"satlog", "flags", "rsvtable"}, "MSN_LOG", "MSN_LOG_Data_Verification", "F3", "Raw_Data")):
        rb = [(j, _responses(j, evs)) for j in jobs if j["cmd"] in cmds]
        _w(d, "Storage/%s/%s_%s.xlsx" % (folder, ts, fname), xlsxio.write_xlsx, [(sheet, _data_sheet(code, rb))])
    dj = [j for j in jobs if j["cmd"] == "digi"]
    drows = [["Packet Type", "Packet Number", "Count Packet", "Raw Data", "Decoded Text", "Command time (UTC)"]]
    dtxt = []
    for j in dj:
        for e in _responses(j, evs):
            if e["kind"] in ("digi", "ascii"):
                drows.append(["0A", "%02d" % (len(drows)), len(drows), hexs(e["text"].encode()), e["text"], stamp(j["t"])])
        dtxt.append("#(%s) CMD: %s" % (stamp(j["t"]), j["line"]))
    _w(d, "Storage/DIGIPEATER_DETAILS/%s_Digipeater_Raw_Data_Verification.xlsx" % ts, xlsxio.write_xlsx, [("Raw_Data", drows)])
    _w(d, "Storage/DIGIPEATER_DETAILS/%s_Digipeater_raw.txt" % ts, _wtxt, "\n".join(dtxt) + ("\n" if dtxt else ""))
    # ---- CAM: image files and fragments
    cam_rows = [["Packet Type", "Packet Number", "Count Packet", "Raw Data"]]
    for s in sorted(core.IMAGES.list(), key=lambda x: x.get("t_first") or 0):
        if not (t0 <= (s.get("t_first") or 0) < t1):
            continue
        got = core.IMAGES.image(s["id"])
        if got:
            ext = "jpeg" if got[1] == "image/jpeg" else got[1].split("/")[-1].replace("octet-stream", "bin")
            its = ts_file(s["t_first"])
            _w(d, "Storage/CAM_DETAILS/%s_cam_image.%s" % (its, ext), lambda p, b: open(p, "wb").write(b), got[0])
            _w(d, "Storage/Mission_Details/CAM/data/image_stored_%s.%s" % (its, ext), lambda p, b: open(p, "wb").write(b), got[0])
        for k in range(1, (s.get("total") or 0) + 1):
            fr = core.IMAGES.fragment(s["id"], k - 1)
            if fr:
                cam_rows.append(["08", "%02d" % (k % 100), k, hexs(fr)])
    _w(d, "Storage/CAM_DETAILS/%s_CAM_Data_Verification.xlsx" % ts, xlsxio.write_xlsx, [("Data", cam_rows)])
    camlog = [["SN", "Time Stamp (UTC)", "Command", "State", "Response bytes", "Decoded Comments"]]
    for i, j in enumerate([j for j in jobs if j["cmd"] in ("cam_run", "cam_dl", "dummycam")], 1):
        camlog.append([i, stamp(j["t"]), j["label"], j["state"], "", j["detail"].get("error", "")])
    _w(d, "Storage/CAM_DETAILS/CAM_LOGS/%s_CAM_LOG_Data.xlsx" % ts, xlsxio.write_xlsx, [("Data", camlog)])
    # ---- Mission_Details
    for m in MISSIONS:
        mj = [j for j in jobs if MISSION_OF.get(j["cmd"]) == m]
        log = ["S2S GS %s operation log, %s" % (m, d["name"]), "Commands: %d" % len(mj), ""]
        for j in mj:
            resp = _responses(j, evs)
            log.append(_response_text(j, resp))
            log.append("Ground station output during this command (text)")
            for r in (j["detail"].get("raw") or []):
                log.append("  %s  %s" % (utc_dt(r["t"]).strftime("%H:%M:%S"), r["text"]))
            log.append("")
            _w(d, "Storage/Mission_Details/%s/data/command_response_%s.txt" % (m, ts_file(j["t"])), _wtxt, _response_text(j, resp))
        _w(d, "Storage/Mission_Details/%s/operation_raw.log" % m, _wtxt, "\n".join(log) + "\n")
        _w(d, "Storage/Mission_Details/%s/%s.xlsx" % (m, m.lower()), xlsxio.write_xlsx, [("Commands", _cmd_rows(mj, evs)), ("Statistics", [["Item", "Value"]] + _stats(mj))])
    # ---- GS_report.xlsx
    pv = passes.predict(t0, 24)["passes"]
    prow = [["AOS (UTC)", "LOS (UTC)", "Duration (min)", "Max elevation (deg)", "Azimuth AOS", "Azimuth LOS", "Commands in pass", "Heartbeats", "Heartbeats answered", "Mean RSSI (dBm)"]]
    for p in pv:
        if not (t0 <= p["aos"] < t1):
            continue
        hbs = [h for h in hb if p["aos"] <= h[0] <= p["los"]]
        rs = [h[3] for h in hbs if h[1] and h[3] is not None]
        prow.append([stamp(p["aos"]), stamp(p["los"]), round((p["los"] - p["aos"]) / 60, 1), round(p["maxEl"], 1), round(p["azIn"]), round(p["azOut"]), sum(1 for j in jobs if p["aos"] <= j["t"] <= p["los"]),
                     len(hbs), sum(1 for h in hbs if h[1]), round(sum(rs) / len(rs), 1) if rs else ""])
    link = [["Time (UTC)", "Heartbeat answered", "Round trip (s)", "RSSI (dBm)", "SNR (dB)"]] + [[stamp(h[0]), "YES" if h[1] else "NO", h[2], h[3], h[4]] for h in hb]
    sig = [["Time (UTC)", "Event", "RSSI (dBm)", "SNR (dB)", "Sample age (s)"]]
    for j in jobs:
        for lab, key in (("command sent: " + j["label"], "at_send"), ("response: " + j["label"], "at_response")):
            s = (j["detail"].get("rssi") or {}).get(key)
            if s:
                sig.append([stamp(j["t"]), lab, s["rssi"], s["snr"], round(abs(s["t"] - j["t"]), 1)])
    nhb = len(hb)
    summ = [["Item", "Value"], ["Folder", d["name"]], ["Generated (UTC)", stamp(time.time())], ["Beacons received", len(bc)], ["Housekeeping packets", len(hk)], ["Heartbeats sent", nhb],
            ["Heartbeats answered", sum(1 for h in hb if h[1])], ["Heartbeat success %", round(100.0 * sum(1 for h in hb if h[1]) / nhb, 1) if nhb else None], ["Passes predicted", len(prow) - 1]] + _stats(jobs)
    _w(d, "GS_report.xlsx", xlsxio.write_xlsx, [("Summary", summ), ("Commands", _cmd_rows(jobs, evs)), ("Overpasses", prow), ("Link (heartbeat)", link), ("Signal at commands", sig)])
    # ---- day summary: what to look at first
    try:
        _state["summary"] = daysum.build(d, jobs, evs, hk, bc, hb, pv, lambda rel, fn, *a: _w(d, rel, fn, *a), _state["last_error"])
    except Exception as e:                                                # the summary must never stop the other files
        _state["summary"] = {"error": "%s: %s" % (type(e).__name__, e)}
        print("day summary failed:", _state["summary"]["error"])
    # ---- auto-filled operational report
    _write_autofill(d, jobs, evs, bc, hk, hb)


def _op_blocks(d, ldt, hk, bc, hb, jobs=None, evs=None, blank=True):
    last = hk[-1] if hk else {}
    b1 = next((e for e in reversed(bc) if e["b1"]["system"] != "?" and 2 <= e["b1"]["bat_v"] <= 10), None)
    up = "YES" if hb and hb[-1][1] else ("NO" if hb else "")
    tz = ldt.strftime("%z")
    off = "GMT%s%s:%s" % (tz[0], tz[1:3].lstrip("0") or "0", tz[3:]) if tz else ""
    bat_v = last.get("bat_v", b1["b1"]["bat_v"] if b1 else "")
    blocks = [("title", "Antarikchya Pratisthan Nepal and"), ("subtitle", "Amateur Radio Digital Communications"), ("subtitle", "Slippers2Sat 1U CubeSat"),
              ("title", "S2S Operational Report"), ("subtitle", "Test Logs"),
              ("kv", [("Time Stamp [%s]" % off, ldt.strftime("%Y-%m-%d, %I:%M%p")), ("Operator(s)", ""), ("Room Temperature and Humidity", ""), ("Satellite Status", ""),
                      ("Uplink Status", up), ("Downlink Status", up), ("Battery Voltage (V)", bat_v), ("Battery Temperature", last.get("bat_t", "")),
                      ("Battery Current (A)", last.get("bat_i", b1["b1"]["bat_i"] if b1 else "")), ("Solar Panel Total Current / Voltage", last.get("sol_v", ""))]),
              ("h", "Beacon Details")]
    t1 = [["DATE", "TIME (UTC)", "TYPE", "SYSTEM", "MISSION", "HOURS", "BATT_V", "BATT_C", "SOL_TOT_V", "RAW"]]
    t2 = [["DATE", "TIME (UTC)", "TYPE", "GYRO", "MAG", "BATT_T", "RAW"]]
    for e in bc[-8:]:
        u = utc_dt(e["t"])
        if e["b1"]["system"] != "?" and 2 <= e["b1"]["bat_v"] <= 10:
            x = e["b1"]
            t1.append([u.strftime("%Y-%m-%d"), u.strftime("%H:%M:%S"), "B1", x["system"], x["mission"], x["hours_since_reset"], x["bat_v"], x["bat_i"], x["sol_total_v"], hexs(bytes.fromhex(e["hex"]))])
        else:
            x = e["b2"]
            t2.append([u.strftime("%Y-%m-%d"), u.strftime("%H:%M:%S"), "B2", str(x["gyro"]), str(x["magneto"]), x["bat_temp_c"], hexs(bytes.fromhex(e["hex"]))])
    if len(t1) == 1:
        t1.append([""] * len(t1[0]))
    if len(t2) == 1:
        t2.append([""] * len(t2[0]))
    blocks += [("p", "Beacon_Type_1"), ("table", t1[0], t1[1:]), ("p", "Beacon_Type_2"), ("table", t2[0], t2[1:]), ("h", "Command Details")]
    for g in ("HK Data", "ADCS", "Camera", "EPDM", "Digipeater"):
        rows = [[i, "#(%s)" % stamp(j["t"]), command_bytes(j["cmd"], j["line"]) or j["line"], packets_of(j["cmd"], j["line"])] for i, j in enumerate([j for j in (jobs or []) if GROUP_OF.get(j["cmd"]) == g], 1)] or [["", "", "", ""]]
        blocks += [("p", g), ("table", ["SN", "Time Stamp (UTC)", "Command", "No of Packets"], rows)]
    blocks += [("h", "Operation Details"), ("kv", [("Command Send", ""), ("Remark", ""), ("ACK Received", ""), ("Task Completed", ""), ("Results", ""), ("Anomalies / observations", "")]),
               ("p", "Prepared by: ______________________      Verified by: ______________________")]
    return blocks


def _write_template(d, ldt):
    base = "S2S_OPERATIONAL_REPORT_%s_%s" % (ldt.strftime("%Y-%m-%d"), ldt.strftime("%I_%M%p"))
    p = os.path.join(d["path"], base + ".docx")
    if not os.path.exists(p):
        docxgen.write_docx(p, _op_blocks(d, ldt, [], [], []))


def _write_autofill(d, jobs, evs, bc, hk, hb):
    ldt = datetime.datetime.fromtimestamp(time.time()).astimezone()
    name = re.sub(r"[ ,]", "_", d["name"])
    p = os.path.join(d["path"], "S2S_OPERATIONAL_REPORT_%s_AUTOFILL.docx" % name.replace("__", "_"))
    docxgen.write_docx(p + ".tmp", _op_blocks(d, ldt, hk, bc, hb, jobs, evs))
    os.replace(p + ".tmp", p)


# ----------------------------------------------------------------------------------------------------------------- sizes, listing, retention
def dir_size(path):
    tot = n = 0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                tot += os.path.getsize(os.path.join(root, f))
                n += 1
            except OSError:
                pass
    return tot, n


def list_days():
    """Local day folders, newest first: [{name, date, bytes, files, uploaded, local}]"""
    out = []
    if not os.path.isdir(REPORTS):
        return out
    up = core.setting("drive_uploaded", {}) or {}
    for name in os.listdir(REPORTS):
        dp = os.path.join(REPORTS, name)
        m = re.match(r"(\d{4}-\d{2}-\d{2})", name)
        if os.path.isdir(dp) and m and not name.startswith("_"):
            b, n = dir_size(dp)
            out.append({"name": name, "date": m.group(1), "bytes": b, "files": n, "uploaded": bool(up.get(name)), "local": True})
    return sorted(out, key=lambda x: x["name"], reverse=True)


def prune(now=None):
    """Delete local day folders older than the retention period (default 7 days). With Drive configured (and delete_requires_upload) only folders already uploaded are deleted;
    without Drive nothing is deleted, because the local folder would then be the only copy."""
    c = cfg()
    now = now or time.time()
    from mc import gdrive
    dr = gdrive.configured()
    removed, kept = [], []
    cut = datetime.datetime.fromtimestamp(now).date() - datetime.timedelta(days=c["retention_days"])
    cur = (core.setting("store_day", {}) or {}).get("name")
    for dday in list_days():
        if dday["name"] == cur or datetime.date.fromisoformat(dday["date"]) >= cut:
            continue
        if c["delete_requires_upload"] and not (dr and dday["uploaded"]):
            kept.append(dday["name"])
            continue
        shutil.rmtree(os.path.join(REPORTS, dday["name"]), ignore_errors=True)
        removed.append(dday["name"])
        core.audit("system", "day folder deleted (retention)", dday["name"])
    return {"removed": removed, "kept": kept}


def tree(day_path):
    """File listing of a day folder: [{path, bytes}] relative paths, for the report viewer."""
    out = []
    for root, _, files in os.walk(day_path):
        for f in sorted(files):
            if f.startswith(".") or f.endswith(".tmp"):
                continue
            p = os.path.join(root, f)
            out.append({"path": os.path.relpath(p, day_path), "bytes": os.path.getsize(p)})
    return sorted(out, key=lambda x: x["path"])


def day_path(name, pulled=False):
    base = PULLED if pulled else REPORTS
    p = os.path.normpath(os.path.join(base, name))
    if not p.startswith(base + os.sep) or not os.path.isdir(p):
        return None
    return p


def _loop():
    last_prune = 0.0
    while True:
        time.sleep(15)
        try:
            ensure_day()
            if _state["dirty"] or time.time() - _state["last_flush"] > 300:
                _state["dirty"] = True
                flush()
            _save_rx()
            if time.time() - last_prune > 3600:
                last_prune = time.time()
                prune()
                with core.DBL:                                            # the received-signal log keeps 30 days
                    core.DB.execute("DELETE FROM rx WHERE t < ?", (time.time() - 30 * 86400,))
                    core.DB.commit()
        except Exception as e:
            _state["last_error"] = "%s: %s" % (type(e).__name__, e)


def start():
    """Register the recording hooks and start the background thread (called once by core.init)."""
    core.HOOKS["raw"].append(on_raw)
    core.HOOKS["event"].append(on_event)
    core.HOOKS["job_done"].append(on_job_done)
    os.makedirs(REPORTS, exist_ok=True)
    ensure_day()
    core.audit("system", "program started", "")
    threading.Thread(target=_loop, daemon=True).start()


def status():
    d = core.setting("store_day", None) or {}
    b, n = dir_size(DATA_ROOT) if os.path.isdir(DATA_ROOT) else (0, 0)
    tb, tn = dir_size(d["path"]) if d.get("path") and os.path.isdir(d["path"]) else (0, 0)
    return {"root": DATA_ROOT, "bytes": b, "files": n, "today": {"name": d.get("name"), "path": d.get("path"), "bytes": tb, "files": tn},
            "last_flush": _state["last_flush"], "error": _state["last_error"], "cfg": cfg(), "summary": _state.get("summary")}
