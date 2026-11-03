#!/usr/bin/env python3
"""
Mission Control, parts that need no hardware and no network: TLE checking (mc/orbit.py) and the orbit maths of the browser (static/orbit.js,
run in node with satellite.js). The TLE below is a real public one (ISS) used only as a test vector for the maths.
"""
import importlib.util
import os
import subprocess
import sys
import types

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
fails = 0


def check(name, ok, extra=""):
    global fails
    print("%s  %s %s" % ("ok  " if ok else "FAIL", name, extra))
    if not ok:
        fails += 1


L1 = "1 25544U 98067A   26300.82187419  .00005456  00000+0  10681-3 0  9999"
L2 = "2 25544  51.6336   3.7485 0007015 317.3678  42.6766 15.49080514592319"

# CW beacon 1 as the COM firmware sends it (Cw_Beacon1_Hex): BAT_I is the OBC's int16, so FFF5 is -0.11 A, not 655.25 A
sys.path.insert(0, os.path.join(ROOT, "tools", "common"))
import events  # noqa: E402
b = events.decode_beacon("C03101AA0185FFF5")["b1"]
check("beacon 1: layout and signed battery current", b["system"] == "normal" and b["bat_v"] == 3.89 and b["bat_i"] == -0.11 and b["sol_total_v"] == 426, str(b))
check("beacon text parser: real CW line", events.parse_gs_line('{"type":"CWRX","text":"S2S2 9N2SJ C064 0190 0190 0064","callsign":1}')[0]["text"] == "S2S2 9N2SJ C064 0190 0190 0064")
check("ping answer parser", events.parse_gs_line('[GS RX] ASCII response: "ACK: PING OK (OBC_ONLINE)"')[0] == {"kind": "ascii", "text": "ACK: PING OK (OBC_ONLINE)"})

# orbit.py imports mc.core (which opens the real database): give it a stub
mc = types.ModuleType("mc")
_store = {}
mc.core = types.SimpleNamespace(setting=lambda k, d=None: _store.get(k, d), set_setting=lambda k, v: _store.__setitem__(k, v))
sys.modules["mc"], sys.modules["mc.core"] = mc, mc.core
spec = importlib.util.spec_from_file_location("orbit", os.path.join(ROOT, "tools", "mission_control", "mc", "orbit.py"))
orbit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(orbit)

check("valid TLE accepted", orbit.check_tle(L1, L2) == (L1, L2))
for name, a, b in (("one digit changed", L1[:20] + "7" + L1[21:], L2), ("short line", L1[:60], L2), ("wrong order", L2, L1),
                   ("different satellites", L1, L2.replace("25544", "25545", 1))):
    try:
        orbit.check_tle(a, b)
        check("TLE rejected: " + name, False)
    except ValueError:
        check("TLE rejected: " + name, True)
try:
    orbit.set_station({"lat": 91, "lon": 0})
    check("latitude 91 rejected", False)
except (ValueError, KeyError):
    check("latitude 91 rejected", True)

# COSPAR id, TLE text / JSON answers and the refresh from a custom address or a CelesTrak-like server (local http server, no internet)
check("COSPAR id forms", orbit.norm_cospar("2026-123a") == "2026-123A" and orbit.norm_cospar("26123A") == "2026-123A" and orbit.norm_cospar("98-067A") == "1998-067A")
try:
    orbit.norm_cospar("hello")
    check("bad COSPAR id rejected", False)
except ValueError:
    check("bad COSPAR id rejected", True)
check("TLE text, 3 lines", orbit._tle_from_text("ISS (ZARYA)\n%s\n%s\n" % (L1, L2)) == ("ISS (ZARYA)", L1, L2))
check("TLE text, 2 lines", orbit._tle_from_text("%s\n%s" % (L1, L2)) == ("", L1, L2))
check("GP json with TLE lines", orbit._tle_from_text('[{"OBJECT_NAME":"S2S2","TLE_LINE1":"%s","TLE_LINE2":"%s"}]' % (L1, L2)) == ("S2S2", L1, L2))
import http.server, threading
class _H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if "999" in self.path:
            self.send_response(404); self.end_headers(); self.wfile.write(b"No GP data found"); return
        self.send_response(200); self.end_headers(); self.wfile.write(("S2S2\n%s\n%s\n" % (L1, L2)).encode())
    def log_message(self, *a):
        pass
_srv = http.server.HTTPServer(("127.0.0.1", 0), _H)
threading.Thread(target=_srv.serve_forever, daemon=True).start()
_base = "http://127.0.0.1:%d" % _srv.server_address[1]
r = orbit.refresh(url=_base + "/tle")
check("refresh from a custom address", not r["error"] and r["norad"] == 25544 and r["query"] == {"url": _base + "/tle"} and r["source"] == "custom address", str(r.get("error")))
r = orbit.refresh()
check("refresh without arguments reuses the stored address", not r["error"] and r["query"] == {"url": _base + "/tle"})
orbit.CELESTRAK_BY = _base + "/gp?%s=%s"
r = orbit.refresh(cospar="2026-123A")
check("refresh by COSPAR id", not r["error"] and r["query"] == {"cospar": "2026-123A"} and "COSPAR" in r["source"], str(r.get("error")))
r = orbit.refresh(norad=98452)
check("refresh by NORAD number", not r["error"] and r["query"] == {"norad": 98452})
r = orbit.refresh(cospar="2026-999A")
check("404 from CelesTrak gives a clear message and keeps the old TLE", "no orbit data" in r["error"] and "COSPAR" in r["error"] and r["line1"] == L1, r["error"][:80])
r = orbit.refresh(url="ftp://x/y")
check("only http(s) addresses", "http" in r["error"] and r["line1"] == L1, r["error"][:60])

# launch time of the mission clock
import calendar, time as _t
orbit.set_launch("2026-10-29T06:30")
check("launch time stored as UTC unix seconds", orbit.get()["launch"] == calendar.timegm(_t.strptime("2026-10-29 06:30", "%Y-%m-%d %H:%M")))
try:
    orbit.set_launch("tomorrow")
    check("bad launch time rejected", False)
except ValueError:
    check("bad launch time rejected", True)
orbit.set_launch("")
check("launch time cleared", orbit.get()["launch"] is None)

# browser orbit maths in node
js = r"""
const fs = require('fs'), vm = require('vm');
const root = process.argv[1];
const ctx = { console, Math, Date, window: {} }; ctx.globalThis = ctx; vm.createContext(ctx);
vm.runInContext(fs.readFileSync(root + '/tools/common/web/vendor/satellite.min.js', 'utf8'), ctx);
vm.runInContext('var satellite = (typeof satellite !== "undefined" ? satellite : globalThis.satellite);', ctx);
vm.runInContext(fs.readFileSync(root + '/tools/mission_control/static/orbit.js', 'utf8'), ctx);
const O = ctx.window.Orbit;
O.set({ line1: process.argv[2], line2: process.argv[3] }, { lat: 27.7172, lon: 85.324, alt: 1.4 });
const t0 = new Date(Date.UTC(2026, 9, 28, 12, 0, 0));
const p = O.at(t0), lk = O.look(t0), ps = O.passes(t0, 24, 5), tr = O.track(t0, 10, 10, 60), fp = O.footprint(p.lat, p.lon, p.alt);
const np = O.nextPass(t0.getTime() / 1000), sx = O.stats(t0, true);
const out = { aosEl: O.look(new Date(np.aos * 1000)).el, losEl: O.look(new Date(np.los * 1000)).el, npOk: np.los > np.aos && np.los > t0.getTime() / 1000, incl: sx.incl, period: sx.period, ecl: sx.eclipse, beta: sx.beta, alt: p.alt, lat: p.lat, speed: p.speed, passes: ps.length, firstOrder: ps.every((q, i) => i === 0 || q.aos > ps[i - 1].aos), maxEl: Math.max(...ps.map(q => q.maxEl)), tr: tr.length, fp: fp.length, elOk: lk.el > -90 && lk.el < 90, age: O.tleAgeDays() };
console.log(JSON.stringify(out));
"""
r = subprocess.run(["node", "-e", js, ROOT, L1, L2], capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[-800:])
    check("node orbit script runs", False)
else:
    import json
    o = json.loads(r.stdout.strip().splitlines()[-1])
    check("altitude of a 51.6 deg / 15.5 rev-per-day orbit is about 400-440 km", 380 < o["alt"] < 450, "(%.0f km)" % o["alt"])
    check("latitude never exceeds the inclination", abs(o["lat"]) <= 51.7)
    check("speed about 7.66 km/s", 7.5 < o["speed"] < 7.8, "(%.2f)" % o["speed"])
    check("passes over Kathmandu in 24 h are found, in time order, max elevation sane", o["passes"] >= 1 and o["firstOrder"] and 5 <= o["maxEl"] <= 90, "(%d passes, max %.0f deg)" % (o["passes"], o["maxEl"]))
    check("AOS / LOS are the exact horizon crossings (elevation within 0.05 deg)", abs(o["aosEl"]) < 0.05 and abs(o["losEl"]) < 0.05 and o["npOk"], "(%.3f, %.3f deg)" % (o["aosEl"], o["losEl"]))
    check("orbit figures: inclination, period 90-95 min, eclipse 0-40 min, |beta| <= 90", abs(o["incl"] - 51.6) < 0.5 and 90 < o["period"] < 95 and 0 <= o["ecl"] <= 40 and abs(o["beta"]) <= 90, "(%.1f min period, %.1f min eclipse)" % (o["period"], o["ecl"]))
    check("ground track and footprint produce lines", o["tr"] >= 1 and o["fp"] >= 1)
    check("look angles are valid", o["elOk"])
# chart data preparation: dense data is bucketed, silence is not bridged by a line
js2 = r"""
const fs = require('fs'), vm = require('vm');
const ctx = { console, Math, Date, window: {}, MC: { h: () => null, useState: () => [0, () => {}], useRef: () => ({}), useEffect: () => {}, fx: x => x, clock: x => x }, MCP: { getPath: (o, p) => o[p] } };
ctx.globalThis = ctx; vm.createContext(ctx);
vm.runInContext(fs.readFileSync(process.argv[1] + '/tools/mission_control/static/charts.js', 'utf8'), ctx);
const P = ctx.window.MCH.prepare;
const dense = Array.from({ length: 5000 }, (_, i) => ({ t: 1000 + i * 17, v: Math.sin(i / 50) }));          // 5000 points over 85000 s
const a = P(dense, 1000, 1000 + 5000 * 17, 600);
const burst = (t0, n) => Array.from({ length: n }, (_, i) => ({ t: t0 + i * 30, v: 4 }));                   // two passes 3 hours apart, 30 s spacing
const b = P(burst(0, 20).concat(burst(10800, 20)), 0, 12000, 600);
const single = P([{ t: 5, v: 1 }], 0, 100, 600);
console.log(JSON.stringify({ n: a.all.length, runsDense: a.runs.length, runsBurst: b.runs.length, rawKept: b.all.length, single: single.runs.length, hasEnv: a.all[3].hi >= a.all[3].lo }));
"""
r2 = subprocess.run(["node", "-e", js2, ROOT], capture_output=True, text=True)
if r2.returncode != 0:
    print(r2.stderr[-600:])
    check("chart preparation script runs", False)
else:
    import json as _j
    o2 = _j.loads(r2.stdout.strip().splitlines()[-1])
    check("5000 dense points are reduced to at most 200 buckets", o2["n"] <= 200 and o2["hasEnv"], "(%d)" % o2["n"])
    check("continuous data stays one line", o2["runsDense"] == 1)
    check("two bursts 3 h apart are two lines, not joined", o2["runsBurst"] == 2 and o2["rawKept"] == 40)
    check("a single sample is kept as a dot", o2["single"] == 1)
print("mission control test: %s" % ("all ok" if fails == 0 else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
