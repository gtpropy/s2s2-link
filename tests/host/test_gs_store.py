#!/usr/bin/env python3
"""
Ground-station data store, heartbeat and Google Drive backup (tools/mission_control/mc/{store,link,gdrive,xlsxio,docxgen}.py). No hardware, no network.

A child process runs the real core (command executor, event parser, SQLite) against temporary folders with a fake ground-station serial link that answers with the real GS line formats;
a local fake Google Drive server (token, folders, multipart upload, download, about) checks the uploader, the retention rule and pulling a day.
"""
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
fails = 0


def check(name, ok, extra=""):
    global fails
    print("%s  %s %s" % ("ok  " if ok else "FAIL", name, extra))
    if not ok:
        fails += 1


CHILD = r'''
import datetime, http.server, json, os, re, sys, threading, time, urllib.parse
ROOT = sys.argv[1]
sys.path[:0] = [ROOT + "/tools/common", ROOT + "/tools/mission_control"]
from mc import core, daysum, docxgen, gdrive, link, passes, store, xlsxio
out = {}

class FakeGS:
    """Answers like the real ground station: ACK accepted / completed JSON lines, FLASH chunks, ASCII responses, RSSI lines."""
    port = "fake"; error = ""; last_rx = 0.0
    def connected(self): return True
    def ports(self): return []
    def send(self, text):
        threading.Thread(target=self._reply, args=(text,), daemon=True).start()
    def _reply(self, text):
        L = core.on_gs_line
        def line(s, dt=0.15):
            time.sleep(dt); L(time.time(), s)
        line("[GS RX] Packet #1 | RSSI=-91 dBm | SNR=7 dB | 200 bytes", 0.1)
        mission = {"EPDMFILE": 8, "EPDM": 8, "PING": 255, "HK1": 1}.get(text.split()[0], 255)
        line('{"type":"ACK","msg":"ACK Accepted mission=0x%02x"}' % mission)
        if text.startswith("EPDMFILE"):
            for k in (1, 2):
                body = ("EPDM RAW %d: value=42\n" % k).encode().ljust(128, b"\0")
                line('{"type":"FLASH","pkt":%d,"total":2,"addr":%d,"len":128,"status":0,"hex":"%s"}' % (k, (k - 1) * 128, body.hex().upper()))
        if text.startswith("PING"):
            line('[GS RX] ASCII response: "ACK: PING OK (OBC_ONLINE)"')
        if text.startswith("HK1"):
            line('{"type":"HK","vbat":389,"vsol":420,"vbus":388,"sp1":410,"sp2":390,"sp3":10,"sp4":400,"sp5":20,"t_ant":140,"t_bat":210,"t_bpb":230,"i_bat":-9,"gx":12,"gy":3,"gz":-4,"mx":22,"my":-8,"mz":35,"seq":7,"crc":1,"rssi":-91,"hex":"' + "AB" * 128 + '"}')
        line('{"type":"ACK","msg":"ACK Completed mission=0x%02x"}' % mission)

core.GS = FakeGS()
store.start()
d0 = store.ensure_day()
out["day"] = d0
for jid in [core.submit("tester", "epdm_file", {"count": 2}), core.submit("tester", "health", {}), core.submit("tester", "epdm", {})]:
    t = time.time()
    while core.JOBS[jid]["state"] in ("queued", "sending", "accepted") and time.time() - t < 40:
        time.sleep(0.2)
    out.setdefault("states", []).append(core.JOBS[jid]["state"])
# pass prediction through node (the browser's orbit code) with a stored TLE
core.set_setting("tle", {"name": "TEST", "line1": "1 25544U 98067A   26300.82187419  .00005456  00000+0  10681-3 0  9999", "line2": "2 25544  51.6336   3.7485 0007015 317.3678  42.6766 15.49080514592319", "norad": 25544, "source": "test", "fetched": 0, "error": ""})
pp = passes.predict(time.time(), 24, max_age=0)
out["passes"] = {"n": len(pp["passes"]), "ordered": all(a["aos"] < b["aos"] for a, b in zip(pp["passes"], pp["passes"][1:])), "reason": pp.get("reason")}
out["link_next"] = bool(link.state().get("next_pass"))
link.set_cfg(enabled=True)                       # the heartbeat is off by default (it transmits by itself)
out['hb_default_off'] = link.DEFAULTS['enabled'] is False
# heartbeat: a real PING round trip through the executor lock
rec = link.beat()
out["beat"] = rec
out["link"] = {k: v for k, v in link.state().items() if k in ("responsive", "command_mode", "rssi", "last_ok_t")}
# a camera picture (3 fragments of a tiny JPEG) so the CAM files can be checked
jpeg = b"\xff\xd8\xff\xe0" + bytes(range(1, 250)) + b"\xff\xd9"
frs = [jpeg[i:i + 128].ljust(128, b"\0") for i in range(0, len(jpeg), 128)]
for i, f in enumerate(frs):
    core.IMAGES.ingest("gs", i, len(frs), f)
time.sleep(1.5)
store.flush(force=True)
root = d0["path"]
out["files"] = sorted(os.path.relpath(os.path.join(r, f), root) for r, _, fs in os.walk(root) for f in fs)
out["gs_report"] = {s["name"]: s["rows"] for s in xlsxio.read_xlsx(os.path.join(root, "GS_report.xlsx"))}
out["address"] = xlsxio.read_xlsx(os.path.join(root, "Storage/ADDRESS_DETAILS/Address_Data.xlsx"))[0]["rows"][:4]
out["epdm_xlsx"] = xlsxio.read_xlsx(os.path.join(root, "Storage/Mission_Details/EPDM/epdm.xlsx"))
resp = [f for f in out["files"] if f.startswith("Storage/Mission_Details/EPDM/data/command_response_")]
out["epdm_resp"] = open(os.path.join(root, sorted(resp)[0])).read() if resp else ""
out["epdm_log"] = open(os.path.join(root, "Storage/Mission_Details/EPDM/operation_raw.log")).read()
rawlog = [f for f in out["files"] if f.endswith("_LOG_raw.txt")]
out["rawlog_lines"] = open(os.path.join(root, rawlog[0])).read().count("\n") if rawlog else 0
out["hk_xlsx"] = xlsxio.read_xlsx(os.path.join(root, [f for f in out["files"] if f.endswith("_HK_Data.xlsx")][0]))[0]["rows"][:2]
out["daysum"] = {x["name"]: x["rows"] for x in xlsxio.read_xlsx(os.path.join(root, "Storage/DAY_SUMMARY/Day_Summary.xlsx"))}
out["daysum_txt"] = open(os.path.join(root, "Storage/DAY_SUMMARY/Day_Summary.txt")).read()
out["daysum_env"] = json.load(open(os.path.join(root, "Storage/DAY_SUMMARY/environment.json")))
out["rx_rows"] = core.DB.execute("SELECT COUNT(*) FROM rx").fetchone()[0]
out["audit_gs"] = [r[0] for r in core.DB.execute("SELECT action FROM audit").fetchall()]
os.makedirs(os.path.join(root, "Mission_Details", "ADCS", "data"), exist_ok=True)           # a folder made by the old layout is moved under Storage
open(os.path.join(root, "Mission_Details", "ADCS", "data", "old.txt"), "w").write("x")
store._tree(root, "20000101")
fb = lambda t, h: {"t": t, "hex": "00", "b1": {"system": "OK", "bat_v": 3.9, "hours_since_reset": h}}
out["reboots"] = [w for _, w in daysum._reboots([{"t": 10, "seq": 5}, {"t": 20, "seq": 6}, {"t": 30, "seq": 1}], [fb(1, 7), fb(2, 8), fb(3, 0)])]
out["gaps"] = daysum._seq_gaps([{"seq": 1}, {"seq": 2}, {"seq": 6}, {"seq": 7}, {"seq": 9}])
hr, hb2 = daysum._health([{"t": 1, "bat_v": 3.9}, {"t": 2, "bat_v": 3.2}, {"t": 3, "bat_v": 3.95}], [], {"bat_v": {"lo": 3.4, "hi": 4.2}})
out["breach"] = [r[-1] for r in hr if r[0] == "Battery voltage"] + [len(hb2)]
pv0 = [{"aos": 1000, "los": 1600, "maxEl": 40, "azIn": 0, "azOut": 0}]
cr, win, cn = daysum._contacts(pv0, [(1030, -90, 5), (1100, -95, 4), (9999, -99, 2)], [], [], [], [], 0, 5000)
out["contact"] = [cr[1][4], cr[1][7], cr[1][9], len(win), [x[1] for x in cn]]
out["migrated"] = [os.path.isdir(os.path.join(root, "Mission_Details")), os.path.exists(os.path.join(root, "Storage", "Mission_Details", "ADCS", "data", "old.txt"))]
out["docx_text"] = docxgen.docx_text(os.path.join(root, [f for f in out["files"] if f.endswith(".docx") and "AUTOFILL" not in f][0]))
out["docx_auto"] = docxgen.docx_text(os.path.join(root, [f for f in out["files"] if f.endswith("_AUTOFILL.docx")][0]))
out["cam_img"] = [f for f in out["files"] if "image_stored_" in f or f.endswith("_cam_image.jpeg")]
out["cam_bytes_ok"] = all(open(os.path.join(root, f), "rb").read()[:3] == b"\xff\xd8\xff" for f in out["cam_img"])

# ---- fake Google Drive
TREE = {"root": {"id": "root", "name": "root", "folder": True, "parent": None}}
NEXT = [1]
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, obj, code=200, raw=None):
        b = raw if raw is not None else json.dumps(obj).encode()
        self.send_response(code); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_POST(self): self._do("POST")
    def do_PATCH(self): self._do("PATCH")
    def do_GET(self): self._do("GET")
    def _do(self, m):
        u = urllib.parse.urlparse(self.path); q = urllib.parse.parse_qs(u.query)
        n = int(self.headers.get("Content-Length", 0)); body = self.rfile.read(n) if n else b""
        if u.path == "/token":
            return self._send({"access_token": "tok", "expires_in": 3000})
        if self.headers.get("Authorization") != "Bearer tok":
            return self._send({"error": {"message": "unauthorized"}}, 401)
        if u.path == "/drive/v3/about":
            return self._send({"storageQuota": {"usage": "1234567", "limit": "16106127360"}, "user": {"emailAddress": "ops@example.org"}})
        if u.path == "/drive/v3/files" and m == "GET":
            qq = q["q"][0]; par = re.search(r"'([^']+)' in parents", qq).group(1)
            nm = re.search(r"name = '((?:[^'\\]|\\.)*)'", qq)
            items = [f for f in TREE.values() if f["parent"] == par and (not nm or f["name"] == nm.group(1).replace("\\'", "'"))]
            if "mimeType = 'application/vnd.google-apps.folder'" in qq: items = [f for f in items if f["folder"]]
            return self._send({"files": [{"id": f["id"], "name": f["name"], "mimeType": "application/vnd.google-apps.folder" if f["folder"] else "application/octet-stream", "size": str(len(f.get("data", b"")))} for f in items]})
        if u.path == "/drive/v3/files" and m == "POST":
            meta = json.loads(body); i = "f%d" % NEXT[0]; NEXT[0] += 1
            TREE[i] = {"id": i, "name": meta["name"], "folder": True, "parent": meta["parents"][0]}
            return self._send({"id": i})
        if u.path == "/upload/drive/v3/files" and m == "POST":
            parts = body.split(b"\r\n\r\n", 2); meta = json.loads(parts[1].split(b"\r\n--")[0]); data = parts[2].rsplit(b"\r\n--", 1)[0]
            i = "f%d" % NEXT[0]; NEXT[0] += 1
            TREE[i] = {"id": i, "name": meta["name"], "folder": False, "parent": meta["parents"][0], "data": data}
            return self._send({"id": i})
        mm = re.match(r"/upload/drive/v3/files/(\w+)", u.path)
        if mm and m == "PATCH":
            parts = body.split(b"\r\n\r\n", 2); data = parts[2].rsplit(b"\r\n--", 1)[0]
            TREE[mm.group(1)]["data"] = data
            return self._send({"id": mm.group(1)})
        mm = re.match(r"/drive/v3/files/(\w+)", u.path)
        if mm and m == "GET" and q.get("alt") == ["media"]:
            return self._send(None, raw=TREE[mm.group(1)]["data"])
        self._send({"error": {"message": "unsupported " + u.path}}, 404)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
threading.Thread(target=srv.serve_forever, daemon=True).start()
base = "http://127.0.0.1:%d" % srv.server_address[1]
out["unconfigured"] = gdrive.configured()
core.set_setting("gdrive", {"enabled": True, "mode": "oauth", "client_id": "cid", "client_secret": "sec", "refresh_token": "rt", "api": base, "token_uri": base + "/token"})
out["configured"] = gdrive.configured()
# an older day, so retention has something to delete: copy today's folder under a date 10 days back
import shutil
old = datetime.date.today() - datetime.timedelta(days=10)
name_old = old.strftime("%Y-%m-%d") + ", 10_00AM"
shutil.copytree(root, os.path.join(store.REPORTS, name_old))
before = [d["name"] for d in store.list_days()]
pr_noup = store.prune()                                      # Drive configured, but nothing uploaded yet -> nothing may be deleted
out["prune_before_upload"] = pr_noup
r = gdrive.sync_all()
out["sync"] = {"uploaded": r.get("uploaded"), "errors": r.get("errors")}
r2 = gdrive.sync_all()
out["resync_uploaded"] = r2.get("uploaded")
out["stats"] = gdrive.stats(max_age=0)
out["drive_days"] = [d["name"] for d in gdrive.list_days()]
rid = [f["id"] for f in TREE.values() if f["name"] == "S2S_operational_reports" and f["parent"] == "root"]
out["drive_root"] = {"count": len(rid), "children": sorted(f["name"] for f in TREE.values() if rid and f["parent"] == rid[0])}
out["uploaded_flags"] = {d["name"]: d["uploaded"] for d in store.list_days()}
pr = store.prune()
out["prune_after_upload"] = pr
out["days_after"] = [d["name"] for d in store.list_days()]
dl = gdrive.download_day(name_old, store.PULLED)
out["pull"] = {"files": dl["files"], "has_report": os.path.exists(os.path.join(dl["path"], "GS_report.xlsx")), "image_same": all(
    open(os.path.join(dl["path"], f), "rb").read() == open(os.path.join(root, f), "rb").read() for f in out["cam_img"])}
core.set_setting("gdrive", {})
out["prune_no_drive"] = store.prune()["removed"]
print("RESULT " + json.dumps(out, default=str))
'''

with tempfile.TemporaryDirectory() as tmp:
    env = dict(os.environ, S2S_MC_DATA=os.path.join(tmp, "mc"), S2S_GS_DATA=os.path.join(tmp, "gs"))
    os.makedirs(env["S2S_MC_DATA"])
    r = subprocess.run([sys.executable, "-c", CHILD, ROOT], capture_output=True, text=True, env=env, timeout=240)
    line = [l for l in r.stdout.splitlines() if l.startswith("RESULT ")]
    if r.returncode != 0 or not line:
        print(r.stdout[-1500:], r.stderr[-2500:])
        check("child process ran", False)
        sys.exit(1)
    o = json.loads(line[0][7:])

from_names = lambda files, s: [f for f in files if s in f]
check("command executor ran the three commands against the fake ground station", o["states"] == ["completed", "completed", "completed"], str(o["states"]))
check("heartbeat is off by default and must be switched on", o["hb_default_off"])
check("heartbeat answered and measured", o["beat"] and o["beat"]["ok"] and o["beat"]["rssi"] == -91 and o["link"]["responsive"] and o["link"]["command_mode"], str(o["link"]))
f = o["files"]
for must in ("GS_report.xlsx", "Storage/ADDRESS_DETAILS/Address_Data.xlsx", "Storage/BEACON_DETAIL/Beacon_details.xlsx", "Storage/Mission_Details/EPDM/operation_raw.log", "Storage/Mission_Details/EPDM/epdm.xlsx",
             "Storage/Mission_Details/ADCS/adcs.xlsx", "Storage/Mission_Details/CAM/cam.xlsx", "Storage/Mission_Details/DPM/dpm.xlsx"):
    check("file exists: " + must, must in f)
check("a satellite restart is found from the beacon hours and from the housekeeping sequence", len(o["reboots"]) == 2, str(o["reboots"]))
check("missing housekeeping packets are counted from the sequence", o["gaps"] == [4, 2], str(o["gaps"]))
check("a battery voltage below its limit is reported once with its count", o["breach"] == [1, 1], str(o["breach"]))
check("a pass is heard when signals fall inside it; the AOS offset and packet count are right", o["contact"][:4] == ["YES", 30, 2, 1], str(o["contact"]))
check("Mission_Details lives under Storage, not next to it", not any(x.startswith("Mission_Details/") for x in f))
check("a Mission_Details folder from the old layout is moved under Storage", o["migrated"] == [False, True], str(o["migrated"]))
for must in ("Storage/DAY_SUMMARY/Day_Summary.xlsx", "Storage/DAY_SUMMARY/Day_Summary.txt", "Storage/DAY_SUMMARY/environment.json", "Storage/DAY_SUMMARY/README.txt"):
    check("file exists: " + must, must in f)
ds = o["daysum"]
check("day summary has all sheets", all(n in ds for n in ("At a glance", "Anomalies", "Contacts", "Satellite health", "Link quality", "Commands", "Environment", "Operator actions")), str(list(ds)))
gl = {r[0]: r[1] for r in ds["At a glance"][1:]}
check("day summary counts the packets of the day from the signal log", int(gl["Packets received"]) >= 3 and o["rx_rows"] >= 3, str(gl["Packets received"]))
check("day summary counts commands sent / completed", gl["Commands sent / completed"] == "3 / 3", str(gl["Commands sent / completed"]))
check("day summary lists the passes of the day and says whether each was heard", len(ds["Contacts"]) > 1 and all(r[4] in ("YES", "NO") for r in ds["Contacts"][1:]), str(len(ds["Contacts"])))
check("a predicted pass without any signal is flagged", any(r[3].startswith("Predicted pass") for r in ds["Anomalies"][1:]) or all(r[4] == "YES" or float(r[3]) < 15 for r in ds["Contacts"][1:]))
check("operator actions of the day are recorded (program start)", any(r[2] == "program started" for r in ds["Operator actions"][1:]))
check("environment names the software, orbit data age, station and disk", o["daysum_env"]["orbit"]["name"] == "TEST" and o["daysum_env"]["orbit"]["epoch_age_days"] is not None and "disk" in o["daysum_env"] and "software" in o["daysum_env"])
check("day summary text file starts with the day and lists passes", o["daysum_txt"].startswith("S2S ground station day summary") and "PASSES" in o["daysum_txt"])
for sub in ("Storage/HK_DETAILS/", "Storage/CAM_DETAILS/CAM_LOGS/", "Storage/MSN_LOG/", "Storage/EPDM_DETAILS/", "Storage/ADCS_DETAILS/", "Storage/DIGIPEATER_DETAILS/", "Storage/LOG_DETAILS/"):
    check("folder has files: " + sub, any(x.startswith(sub) for x in f))
check("day folder is directly in GS_Data and named YYYY-MM-DD, HH_MMAM", __import__("re").match(r"\d{4}-\d{2}-\d{2}, \d{2}_\d{2}(AM|PM)$", o["day"]["name"]) and os.path.dirname(o["day"]["path"]) == os.path.join(tmp, "gs"), o["day"]["path"])
check("blank operational report template exists plus the auto-filled copy", any(x.endswith(".docx") and "AUTOFILL" not in x for x in f) and any(x.endswith("_AUTOFILL.docx") for x in f))
check("template has fields for the operator", "Operator(s)" in o["docx_text"] and "Room Temperature and Humidity" in o["docx_text"] and "Anomalies" in o["docx_text"])
check("auto-filled report carries the command details", "53 01 1D D2 F6" in o["docx_auto"])
check("raw log has the ground station lines", o["rawlog_lines"] > 10, str(o["rawlog_lines"]))
check("response file has the file blocks in hex and the decoded text", "45 50 44 4D 20 52 41 57" in o["epdm_resp"] and "EPDM RAW 1: value=42" in o["epdm_resp"] and "AC 00 08" in o["epdm_resp"] and "RSSI at send       : -91" in o["epdm_resp"])
check("operation_raw.log lists the command and the raw ground station lines", "EPDM" in o["epdm_log"] and "FLASH" in o["epdm_log"])
stat = dict((r[0], r[1]) for r in o["epdm_xlsx"][1]["rows"][1:])
check("EPDM statistics: 2 commands, both completed", stat.get("Commands sent") == 2 and stat.get("Completed") == 2, str(stat))
check("GS_report: commands, overpasses, link and signal sheets", all(k in o["gs_report"] for k in ("Summary", "Commands", "Overpasses", "Link (heartbeat)", "Signal at commands")))
check("GS_report: signal strength recorded at command times", len(o["gs_report"]["Signal at commands"]) >= 4 and -91 in [r[2] for r in o["gs_report"]["Signal at commands"][1:]])
check("GS_report: heartbeat logged", len(o["gs_report"]["Link (heartbeat)"]) == 2 and o["gs_report"]["Link (heartbeat)"][1][1] == "YES")
check("Address_Data groups the commands per mission", o["address"][0][4] == "ADCS" and o["address"][0][12] == "EPDM" and "53 01 1D D2 F6" in str(o["address"][2]), str(o["address"][2][:16]))
check("HK data decoded into columns", o["hk_xlsx"][1][3] == 3.89, str(o["hk_xlsx"][1][:6]))
check("camera image stored twice (Storage and Mission_Details) and is a JPEG", len(o["cam_img"]) == 2 and o["cam_bytes_ok"], str(o["cam_img"]))
check("server-side pass prediction (node) returns ordered passes and the link state knows the next one", o["passes"]["n"] >= 3 and o["passes"]["ordered"] and o["link_next"], str(o["passes"]))
check("GS_report lists the overpasses of the day", len(o["gs_report"]["Overpasses"]) >= 2, str(len(o["gs_report"]["Overpasses"])))
check("Drive: not configured by default, configured after settings", o["unconfigured"] is False and o["configured"] is True)
check("retention: nothing deleted while the day is not uploaded", o["prune_before_upload"]["removed"] == [] and len(o["prune_before_upload"]["kept"]) >= 1, str(o["prune_before_upload"]))
check("Drive upload: files uploaded without error", o["sync"]["uploaded"] > 20 and o["sync"]["errors"] == [], str(o["sync"]))
check("Drive upload: a second sync uploads nothing unchanged except the live log", o["resync_uploaded"] <= 3, str(o["resync_uploaded"]))
check("Drive stats: used / limit / folder size", o["stats"].get("used") == 1234567 and o["stats"].get("folder_bytes", 0) > 1000, str({k: o["stats"].get(k) for k in ("used", "limit", "folder_bytes", "folder_files")}))
check("Drive: one folder named S2S_operational_reports holds the day folders directly", o["drive_root"]["count"] == 1 and len(o["drive_root"]["children"]) == 2 and all(n[:2] == "20" for n in o["drive_root"]["children"]), str(o["drive_root"]))
check("Drive lists the day folders", len(o["drive_days"]) == 2, str(o["drive_days"]))
check("retention: the 10-day-old uploaded day is deleted, today's stays", len(o["prune_after_upload"]["removed"]) == 1 and len(o["days_after"]) == 1, str(o["prune_after_upload"]))
check("pull: the deleted day comes back from Drive intact", o["pull"]["has_report"] and o["pull"]["image_same"] and o["pull"]["files"] > 20, str(o["pull"]))
check("retention without Drive deletes nothing (the local copy would be the only one)", o["prune_no_drive"] == [])
print("gs store test: %s" % ("all ok" if fails == 0 else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
