"""
tb.scenarios -- your own test sequences. A scenario is a JSON file with a list of steps; the testbench runs them in order, records each
step as PASS / FAIL and puts the result into the report like any other test. Built-in scenarios live in tools/testbench/scenarios/,
yours in data/testbench/scenarios/.

Step types (every step may carry "name" and "continue": true to go on after a failure):
  {"do": "gs",         "send": "HK1"}                                         send a line to the ground-station firmware
  {"do": "obc",        "cmd": "STATUS", "args": {}, "expect_status": 0}       Nucleo (CPU1) command; expect_status optional
  {"do": "expect",     "event": "ack", "what": "accepted", "within_s": 90,    wait for a decoded event from the GS;
                       "resend": "HK1", "every_s": 14, "tries": 8}              optional: resend a GS line until it arrives
  {"do": "wait",       "s": 5}
  {"do": "test",       "name": "membudget", "cfg": {"target": "sat"}}         run one of the testbench tests (PASS or INFO = ok)
  {"do": "screenshot", "route": "dashboard", "note": "after HK"}
  {"do": "report",     "note": "nightly"}
  {"do": "log",        "msg": "text for the log"}
"""
import json
import os
import re
import time

from tb.bus import BUS, DATA, EVENTS, log
from tb.consoles import obc_cmd
from tb.links import GS, OBC
from tb.registry import TESTS
from tb.reports import save_report, take_shot
from tb.state import STATE, set_result
from tb.util import HERE, now

BUILTIN_DIR = os.path.join(HERE, "scenarios")
USER_DIR = os.path.join(DATA, "scenarios")
REQUIRED = {"gs": ["send"], "obc": ["cmd"], "expect": ["event"], "wait": ["s"], "test": ["name"], "screenshot": [], "report": [], "log": ["msg"]}
EVENTS_OK = ("ack", "nack", "hk", "beacon", "cw", "rssi")


def validate(scn, known_tests=()):
    """List of problems (empty = fine)."""
    errs = []
    if not isinstance(scn, dict) or not isinstance(scn.get("steps"), list) or not scn["steps"]:
        return ["a scenario is an object with a non-empty \"steps\" list"]
    if len(scn["steps"]) > 200:
        errs.append("more than 200 steps")
    for i, st in enumerate(scn["steps"], 1):
        if not isinstance(st, dict) or st.get("do") not in REQUIRED:
            errs.append(f"step {i}: \"do\" must be one of {', '.join(REQUIRED)}")
            continue
        for k in REQUIRED[st["do"]]:
            if k not in st:
                errs.append(f"step {i} ({st['do']}): missing \"{k}\"")
        if st["do"] == "expect" and st.get("event") not in EVENTS_OK:
            errs.append(f"step {i}: event must be one of {', '.join(EVENTS_OK)}")
        if st["do"] == "wait" and not (isinstance(st.get("s"), (int, float)) and 0 <= st["s"] <= 3600):
            errs.append(f"step {i}: wait needs 0 <= s <= 3600")
        if st["do"] == "test" and known_tests and st.get("name") not in known_tests and st.get("name") != "scenario":
            errs.append(f"step {i}: unknown test \"{st.get('name')}\"")
        if st["do"] == "test" and st.get("name") == "scenario":
            errs.append(f"step {i}: a scenario cannot run a scenario")
    return errs


def run_steps(scn, abort, env):
    """Execute the steps with the helper functions in `env` (send_gs, obc, events_since, run_test, shot, report, log, now, sleep).
    Returns (verdict, rows)."""
    rows = []
    stop = False
    for i, st in enumerate(scn["steps"], 1):
        label = st.get("name") or f"{st['do']}" + (f" {st.get('send') or st.get('cmd') or st.get('event') or st.get('name') or ''}".rstrip())
        if stop or abort.is_set():
            rows.append({"name": f"{i}. {label}", "state": "INFO", "detail": "skipped (an earlier step failed or the run was stopped)", "expected": ""})
            continue
        t0 = env.now()
        try:
            ok, detail = _step(st, abort, env)
        except Exception as e:
            ok, detail = False, f"{type(e).__name__}: {e}"
        rows.append({"name": f"{i}. {label}", "state": "PASS" if ok else "FAIL", "detail": f"{detail} ({env.now() - t0:.1f} s)", "expected": ""})
        if not ok and not st.get("continue"):
            stop = True
        env.progress(i, len(scn["steps"]), rows)
    verdict = "FAIL" if any(r["state"] == "FAIL" for r in rows) else ("INFO" if abort.is_set() else "PASS")
    return verdict, rows


def _step(st, abort, env):
    do = st["do"]
    if do == "gs":
        env.send_gs(st["send"])
        return True, f"sent \"{st['send']}\""
    if do == "obc":
        r = env.obc(st["cmd"], st.get("args", {}))
        want = st.get("expect_status")
        ok = r.get("status") is not None and (want is None or r["status"] == want)
        return ok, f"{r.get('meaning')} (status {r.get('status')})" + ("" if want is None else f", expected {want}")
    if do == "wait":
        end = env.now() + float(st["s"])
        while env.now() < end and not abort.is_set():
            env.sleep(min(0.5, end - env.now()))
        return True, f"waited {st['s']} s"
    if do == "expect":
        t0 = env.now()
        within = float(st.get("within_s", 60))
        every = float(st.get("every_s", 14))
        tries = int(st.get("tries", 1))
        sent = 0
        next_send = t0
        while env.now() - t0 < within and not abort.is_set():
            if st.get("resend") and sent < tries and env.now() >= next_send:
                env.send_gs(st["resend"])
                sent += 1
                next_send = env.now() + every
            for e in env.events_since(t0, (st["event"],)):
                if st.get("what") is None or e.get("what") == st["what"]:
                    return True, f"{st['event']}{(' ' + st['what']) if st.get('what') else ''} after {e['t'] - t0:.1f} s ({sent} send(s))"
            env.sleep(0.3)
        return False, f"no {st['event']}{(' ' + st['what']) if st.get('what') else ''} within {within:.0f} s ({sent} send(s))"
    if do == "test":
        v = env.run_test(st["name"], st.get("cfg", {}), abort)
        return v in ("PASS", "INFO"), f"test {st['name']}: {v}"
    if do == "screenshot":
        return True, "saved " + env.shot(st.get("route", "dashboard"), st.get("note", "scenario"))
    if do == "report":
        return True, "saved " + os.path.basename(env.report(st.get("note", "scenario")))
    if do == "log":
        env.log(st["msg"])
        return True, st["msg"]
    raise ValueError(do)


# ---------------------------------------------------------------- files
def _safe(name):
    if not re.match(r"^[A-Za-z0-9_\-]{1,40}$", name or ""):
        raise ValueError("a scenario name has 1-40 letters, digits, - or _")
    return name


def list_scenarios():
    out = []
    for d, builtin in ((BUILTIN_DIR, True), (USER_DIR, False)):
        if os.path.isdir(d):
            for f in sorted(os.listdir(d)):
                if f.endswith(".json"):
                    try:
                        s = json.load(open(os.path.join(d, f)))
                    except (OSError, ValueError):
                        continue
                    out.append({"name": f[:-5], "builtin": builtin, "title": s.get("title", ""), "steps": len(s.get("steps", [])), "scenario": s})
    return out


def load(name):
    _safe(name)
    for d in (USER_DIR, BUILTIN_DIR):
        f = os.path.join(d, name + ".json")
        if os.path.isfile(f):
            return json.load(open(f))
    raise ValueError(f"no scenario named {name}")


def save(name, scn):
    _safe(name)
    errs = validate(scn, TESTS.keys())
    if errs:
        raise ValueError("; ".join(errs))
    os.makedirs(USER_DIR, exist_ok=True)
    json.dump(scn, open(os.path.join(USER_DIR, name + ".json"), "w"), indent=1)


def delete(name):
    _safe(name)
    f = os.path.join(USER_DIR, name + ".json")
    if not os.path.isfile(f):
        raise ValueError("only your own scenarios can be deleted")
    os.remove(f)


# ---------------------------------------------------------------- the test that runs one
class _Env:
    now = staticmethod(now)
    sleep = staticmethod(time.sleep)
    log = staticmethod(log)

    @staticmethod
    def send_gs(line):
        GS.send(line)

    @staticmethod
    def obc(cmd, args):
        return obc_cmd(cmd, args)

    @staticmethod
    def events_since(t0, kinds):
        return [e for e in EVENTS.recent(limit=800) if e["t"] >= t0 and e["kind"] in kinds]

    @staticmethod
    def run_test(name, cfg, abort):
        STATE[name] = None
        TESTS[name]({"fresh": False, **cfg}, abort)
        return (STATE.get(name) or {}).get("verdict", "FAIL")

    shot = staticmethod(take_shot)
    report = staticmethod(lambda note: save_report(note, True))

    @staticmethod
    def progress(done, total, rows):
        BUS.publish("progress", test="scenario", done=done, total=total)


def test_scenario(cfg, abort):
    name = cfg.get("scenario")
    scn = load(name)
    errs = validate(scn, TESTS.keys())
    if errs:
        raise RuntimeError("; ".join(errs))
    if any(s["do"] in ("gs", "expect") for s in scn["steps"]) and not GS.connected():
        raise RuntimeError("this scenario sends to the ground station: connect its serial port first")
    if any(s["do"] == "obc" for s in scn["steps"]) and not OBC.connected():
        raise RuntimeError("this scenario talks to the Nucleo: connect its VCP port first")
    verdict, rows = run_steps(scn, abort, _Env)
    set_result("scenario", {"test": "scenario", "when": now(), "verdict": verdict, "scenario": name, "title": scn.get("title", ""), "rows": rows})
    log(f"SCENARIO {name}: {verdict} ({sum(1 for r in rows if r['state'] == 'PASS')}/{len(rows)} steps)", "ok" if verdict == "PASS" else "err")
