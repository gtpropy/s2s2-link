#!/usr/bin/env python3
"""Logic of the testbench features that need no hardware: trends / report diff, scenario validation and engine, log store, alerts."""
import os
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "testbench"))
import tb                                              # noqa: E402,F401  (sets sys.path)
from tb import scenarios, trends                       # noqa: E402
from tb.bus import Bus                                 # noqa: E402
from tb.logstore import LogStore                       # noqa: E402

bad = 0


def check(c, msg):
    global bad
    if not c:
        print("FAIL", msg)
        bad += 1


# ---- trends
ra = {"dir": "A", "generated": "t1", "results": {"image": {"verdict": "PASS", "elapsed_s": 100, "received": 69, "match": True}, "digi": {"verdict": "PASS", "rows": [{"ok": True}, {"ok": True}]}, "link": None}}
rb = {"dir": "B", "generated": "t2", "results": {"image": {"verdict": "FAIL", "elapsed_s": 130, "received": 60}, "digi": {"verdict": "PASS", "rows": [{"ok": True}, {"ok": True}]}, "soak": {"verdict": "INFO", "rows": [{"state": "PASS"}, {"state": "INFO"}]}}}
m = trends.metrics_of(ra["results"]["image"])
check(m == {"elapsed_s": 100, "received": 69}, f"metrics skip booleans: {m}")
check(trends.metrics_of(rb["results"]["soak"]) == {"rows_pass": 1, "rows_fail": 0, "rows_info": 1}, "row counts")
d = {x["test"]: x for x in trends.diff(ra, rb)}
check(d["image"]["status"] == "regressed" and {x["name"]: x["delta"] for x in d["image"]["moved"]}["elapsed_s"] == 30, "image regressed, +30 s")
check(d["digi"]["status"] == "same" and not d["digi"]["moved"], "digi same")
check(d["soak"]["a"] is None and d["soak"]["b"] == "INFO", "soak appeared")
t = trends.build([ra, rb])
check([x["verdict"] for x in t["image"]] == ["PASS", "FAIL"] and "link" not in t, "build keeps order, drops empty")
real = trends.load_reports()
check(isinstance(real, list), "real reports load")

# ---- scenarios: validation
good = {"steps": [{"do": "gs", "send": "HK1"}, {"do": "wait", "s": 1}]}
check(scenarios.validate(good, ["membudget"]) == [], "valid scenario")
errs = scenarios.validate({"steps": [{"do": "fly"}, {"do": "gs"}, {"do": "expect", "event": "xyz"}, {"do": "wait", "s": -1}, {"do": "test", "name": "nope"}]}, ["membudget"])
check(len(errs) == 5, f"five problems found: {errs}")
check(scenarios.validate({}) and scenarios.validate({"steps": []}), "empty scenario rejected")
check(scenarios.validate({"steps": [{"do": "test", "name": "scenario"}]}, ["membudget"]), "no recursion")
for s in scenarios.list_scenarios():
    check(scenarios.validate(s["scenario"], ["membudget", "fwcheck", "bootcfg", "vcpfuzz", "cmdconf", "beacontime"]) == [], f"built-in scenario {s['name']} is valid")


# ---- scenarios: engine with stand-in helper functions (tests the control flow only)
class Env:
    def __init__(self):
        self.t = 1000.0
        self.sent, self.events, self.ran = [], [], []

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += s

    def log(self, m):
        pass

    def send_gs(self, line):
        self.sent.append(line)
        if line == "HK1" and len(self.sent) >= 2:                           # the satellite answers the second send
            self.events.append({"kind": "ack", "what": "accepted", "t": self.t + 1})

    def obc(self, cmd, args):
        return {"status": 0 if cmd == "STATUS" else 7, "meaning": "x"}

    def events_since(self, t0, kinds):
        return [e for e in self.events if e["t"] >= t0 and e["kind"] in kinds]

    def run_test(self, name, cfg, abort):
        self.ran.append(name)
        return "PASS" if name != "bad" else "FAIL"

    def shot(self, route, note):
        return "a.png"

    def report(self, note):
        return "/x/20261010_000000"

    def progress(self, *a):
        pass


ev = threading.Event()
v, rows = scenarios.run_steps({"steps": [{"do": "expect", "event": "ack", "what": "accepted", "resend": "HK1", "every_s": 5, "tries": 4, "within_s": 60},
                                         {"do": "obc", "cmd": "STATUS", "expect_status": 0}, {"do": "test", "name": "membudget"}, {"do": "screenshot"}, {"do": "report"}, {"do": "log", "msg": "hi"}]}, ev, Env())
check(v == "PASS" and [r["state"] for r in rows] == ["PASS"] * 6, f"all steps pass: {v} {[r['detail'] for r in rows]}")
e = Env()
v, rows = scenarios.run_steps({"steps": [{"do": "obc", "cmd": "RESET", "expect_status": 0}, {"do": "test", "name": "membudget"}]}, ev, e)
check(v == "FAIL" and rows[0]["state"] == "FAIL" and rows[1]["state"] == "INFO" and not e.ran, "a failed step stops the run, the rest is marked skipped")
e = Env()
v, rows = scenarios.run_steps({"steps": [{"do": "test", "name": "bad", "continue": True}, {"do": "test", "name": "membudget"}]}, ev, e)
check(v == "FAIL" and e.ran == ["bad", "membudget"], "continue keeps going but the verdict stays FAIL")
v, rows = scenarios.run_steps({"steps": [{"do": "expect", "event": "hk", "within_s": 3}]}, ev, Env())
check(v == "FAIL" and "no hk within 3 s" in rows[0]["detail"], "expect times out")
stop = threading.Event()
stop.set()
v, rows = scenarios.run_steps({"steps": [{"do": "wait", "s": 5}]}, stop, Env())
check(v == "INFO" and rows[0]["state"] == "INFO", "stopped before the first step")

# ---- log store
bus = Bus()
path = os.path.join(tempfile.mkdtemp(), "logs.db")
ls = LogStore(path, bus)
for i in range(30):
    bus.publish("gs", msg=f"GS line {i} RSSI -{80 + i}")
bus.publish("sat", msg="CPU1 says hello_world 100%")
bus.publish("log", level="err", msg="bench failure")
bus.publish("chunk", pkt=1)                                                  # not a log kind
time.sleep(1.8)
check(ls.stats()["lines"] == 32, f"32 lines stored, got {ls.stats()}")
check(ls.search(q="line 1")["total"] == 11, "substring search (line 1, 10..19)")
check(ls.search(q="100%")["total"] == 1 and ls.search(q="hello_world")["total"] == 1, "LIKE wildcards are escaped")
check(ls.search(srcs=["VCP"])["rows"][0]["msg"].startswith("CPU1"), "source filter")
check(ls.search(q=r"RSSI -8[0-2]\b", regex=True)["total"] == 3, "regex search")
check(ls.search(srcs=["BENCH"])["rows"][0]["level"] == "err", "level kept for bench lines")
check(len(ls.search(limit=5)["rows"]) == 5 and ls.search(limit=5, offset=30)["rows"], "paging")
csv_text = ls.csv(q="GS line 2")
check(csv_text.splitlines()[0].startswith("time_utc") and len(csv_text.splitlines()) == 12, "csv export")

# ---- alerts
from tb.watchdog import Watchdog                                              # noqa: E402
w = Watchdog()
a1 = w.raise_alert("k", "bad", "first")
check(a1 and w.raise_alert("k", "bad", "again") is None, "same key is not raised twice while unacknowledged")
w.ack(a1["id"])
check(w.raise_alert("k", "bad", "after ack") is not None, "raised again after the acknowledgement")
try:
    w.set({"nonsense": 1})
    check(False, "unknown setting must be refused")
except ValueError:
    pass

print("testbench features: FAILED" if bad else "testbench features OK (trends diff, scenario validation + engine, log store search/regex/csv, alerts)")
sys.exit(1 if bad else 0)
