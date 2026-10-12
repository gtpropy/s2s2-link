"""tb.web -- HTTP routes of the testbench (tools/common/httpx.py provides the server machinery)."""
import glob
import os
import threading

import httpx
from tb.build import do_build, do_flash
from tb.bus import BUS, EVENTS, IMAGES, log
from tb import cubus
from tb.compat import cubus_on
from tb.commands import COMMANDS, JOBS, config_set, config_summary, start_job
from tb.compat import imagestore
from tb.consoles import GS_PALETTE, OBC_PALETTE, console_lines, obc_cmd, obc_raw
from tb.devices import SDR
from tb.digi import digi_inject, saf_read
from tb import scenarios, trends
from tb.imaging import IMG, kind_of, set_image
from tb.links import GS, OBC, sat_fresh_start
from tb.logstore import LOGS
from tb.registry import TESTS
from tb.profiling import MON, PROF, monitor_start, monitor_stop, prof_start, prof_targets
from tb.reports import SHOT_DIR, list_reports, list_shots, save_report, take_shot
from tb.runner import start_test
from tb.state import RUN
from tb.status import get_status, live_state
from tb.watchdog import WATCH
from tb.util import GS_DIR, HERE, OBC_SAF, REPORT_DIR, ROOT
from httpx import HTTPError, Raw

router = httpx.Router()
get, post = router.get, router.post
STATIC_DIRS = [("", os.path.join(HERE, "static")), ("", os.path.join(ROOT, "tools", "common", "web"))]
REPORT_TYPES = {"zip": "application/zip", "pdf": "application/pdf", "html": "text/html", "json": "application/json", "png": "image/png"}


def _need_idle():
    if RUN["name"]:
        raise RuntimeError(f"test '{RUN['name']}' is running")


def _monitor():
    return {"on": bool(MON["cap"]), "freq": MON["freq"], "gain": MON["gain"], "since": MON["since"]}


# ---------------------------------------------------------------- status, history, consoles
@get("/api/status")
def r_status(req):
    return get_status(fresh="fresh" in req.q)


@get("/api/live")
def r_live(req):
    return live_state()


@get("/api/commands")
def r_commands(req):
    return {"commands": COMMANDS, "gs": GS_PALETTE, "obc": OBC_PALETTE}


@get("/api/job")
def r_job(req):
    return JOBS.get(int(req.q.get("id", 0))) or {}


@get("/api/history")
def r_history(req):
    return EVENTS.history(req.q.get("kind"), int(req.q.get("limit", 500)))


@get("/api/console")
def r_console(req):
    return console_lines(req.q.get("src", "gs"), int(req.q.get("since", 0)))


@post("/api/gs/connect")
def r_gs_connect(req):
    c = req.json()
    GS.connect(c.get("port", ""), int(c.get("baud", 115200)))
    return {"ok": True, "port": GS.port}


@post("/api/gs/disconnect")
def r_gs_disconnect(req):
    GS.disconnect()


@post("/api/gs/send")
def r_gs_send(req):
    GS.send(req.json()["text"])


@post("/api/obc/connect")
def r_obc_connect(req):
    c = req.json()
    OBC.connect(c.get("port", ""), int(c.get("baud", 115200)), text_only=cubus_on())     # CUBUS: the satellite's debug UART, text only
    return {"ok": True, "port": OBC.port, "text_only": OBC.text_only}


@post("/api/obc/disconnect")
def r_obc_disconnect(req):
    OBC.disconnect()


@post("/api/obc/cmd")
def r_obc_cmd(req):
    c = req.json()
    if RUN["name"] and RUN["name"] != "e2e":
        raise RuntimeError(f"test '{RUN['name']}' is running")
    return obc_cmd(c["name"], c.get("args", {}))


@post("/api/obc/raw")
def r_obc_raw(req):
    return obc_raw(req.json()["hex"])


@post("/api/cmd")
def r_cmd(req):
    c = req.json()
    return {"job": start_job(c["id"], c.get("args", {}))}


@post("/api/sat/reset")
def r_sat_reset(req):
    threading.Thread(target=lambda: sat_fresh_start(RUN["abort"]), daemon=True).start()


# ---------------------------------------------------------------- images
@get("/api/images")
def r_images(req):
    return IMAGES.list()


@get(r"^/api/images/([A-Za-z0-9_]+)/(image|meta|frag/(\d+))$")
def r_image_item(req):
    sid, what, frag = req.m.group(1), req.m.group(2), req.m.group(3)
    if what == "meta":
        return IMAGES.get(sid) or {}
    if frag:
        d = IMAGES.fragment(sid, int(frag) - 1)
        return Raw(d) if d else (404, {"error": "no such fragment"})
    r = IMAGES.image(sid)
    if not r:
        return (404, {"error": "no such picture"})
    hd = {"Content-Disposition": f'attachment; filename="{sid}.{imagestore.sniff(r[0])[0]}"'} if "download" in req.q else None
    return Raw(r[0], r[1], headers=hd)


@post("/api/images/delete")
def r_images_delete(req):
    return {"ok": IMAGES.delete(req.json()["id"])}


@get("/api/image/source")
def r_image_source(req):
    return Raw(IMG["bytes"], kind_of(IMG["bytes"])[1]) if IMG["bytes"] else (404, {"error": "no picture selected"})


@get("/api/image/received")
def r_image_received(req):
    return Raw(IMG["rx"], kind_of(IMG["bytes"])[1]) if IMG.get("rx") else (404, {"error": "nothing received yet"})


@get("/api/samples")
def r_samples(req):
    out = []
    for pat in (os.path.join(HERE, "samples", "*.png"), os.path.join(HERE, "samples", "*.jpg"), os.path.join(GS_DIR, "*.png"), os.path.join(GS_DIR, "*.jpg")):
        out += [os.path.relpath(f, ROOT) for f in sorted(glob.glob(pat))]
    return sorted(out, key=lambda p: (0 if "antarikshya" in p else 1, p))


@post("/api/image/select")
def r_image_select(req):
    c = req.json()
    f = os.path.abspath(os.path.join(ROOT, c["path"]))
    if not f.startswith(ROOT + os.sep):
        raise RuntimeError("path outside the repository")
    return {"info": set_image(open(f, "rb").read(), os.path.basename(f), c.get("fit", "flash"))}


@post("/api/image/refit")
def r_image_refit(req):
    if IMG.get("orig") is None:                      # re-process the file already loaded (upload or sample) with another fit mode
        raise RuntimeError("no image loaded yet")
    return {"info": set_image(IMG["orig"], IMG["name"], req.json().get("fit", "flash"))}


@post("/api/image/upload")
def r_image_upload(req):
    return {"info": set_image(req.body(), req.q.get("name", "upload"), req.q.get("fit", "flash"))}


# ---------------------------------------------------------------- digipeater
@post("/api/digi/inject")
def r_digi_inject(req):
    _need_idle()
    st, why = digi_inject(req.json()["text"])
    return {"status": st, "result": why}


@post("/api/digi/store")
def r_digi_store(req):
    _need_idle()
    line, recs = saf_read()
    return {"journal": line, "objects": recs, "stored": len(recs), "bytes": sum(x["len"] for x in recs)}


@post("/api/digi/clear")
def r_digi_clear(req):
    mk = OBC.mark()
    OBC.send(OBC_SAF, 0xFFFF)
    return {"status": OBC.wait_ack(OBC_SAF, 0xFFFF, mk, 8), "note": "journal emptied; reset CPU2 so it forgets its RAM copy"}


# ---------------------------------------------------------------- profiler
@get("/api/prof")
def r_prof(req):
    PROF.targets = prof_targets()
    return {"running": list(PROF.sessions), "layout": {t: PROF.layout(t) for t in PROF.targets},
            "probes": {t: v["serial"] for t, v in PROF.targets.items()}, "state": {t: s.state() for t, s in PROF.sessions.items()}}


@get("/api/prof/ramsyms")
def r_prof_ramsyms(req):
    PROF.targets = prof_targets()
    return PROF.ramsyms(req.q["target"])


@get("/api/prof/symbols")
def r_prof_symbols(req):
    return PROF.symbols(req.q["target"], req.q["core"], req.q.get("q", ""))


@post("/api/prof/start")
def r_prof_start(req):
    prof_start(req.json()["target"])


@post("/api/prof/stop")
def r_prof_stop(req):
    PROF.stop(req.json().get("target"))


def _session(c):
    s = PROF.sessions.get(c["target"])
    if not s:
        raise RuntimeError("start the profiler for that board first")
    return s


@post("/api/prof/watch")
def r_prof_watch(req):
    c = req.json()
    _session(c).add_watch(c["name"], c["core"])


@post("/api/prof/unwatch")
def r_prof_unwatch(req):
    c = req.json()
    _session(c).watch.pop(c["name"], None)


@post("/api/prof/peek")
def r_prof_peek(req):
    c = req.json()
    addr = int(str(c["addr"]), 0)
    return {"addr": addr, "hex": _session(c).peek(addr, int(c.get("len", 64))).hex()}


@post("/api/prof/stack")
def r_prof_stack(req):
    c = req.json()
    s = _session(c)
    return s.stack_arm(c["core"]) if c["action"] == "arm" else s.stack_read(c["core"])


# ---------------------------------------------------------------- tests, build, flash
@post("/api/test/start")
def r_test_start(req):
    c = req.json()
    start_test(c["name"], c)


@post("/api/test/stop")
def r_test_stop(req):
    RUN["abort"].set()
    SDR.stop()


@post("/api/build")
def r_build(req):
    c = req.json()
    threading.Thread(target=do_build, args=(c["target"], c.get("cubus")), daemon=True).start()


@post("/api/flash")
def r_flash(req):
    c = req.json()
    threading.Thread(target=do_flash, args=(c["target"], c), daemon=True).start()


@get("/api/monitor")
def r_monitor(req):
    return _monitor()


@post("/api/monitor")
def r_monitor_set(req):
    c = req.json()
    monitor_start(c.get("freq"), c.get("gain")) if c.get("on") else monitor_stop()
    return _monitor()


@get("/api/config")
def r_config(req):
    return config_summary()


@post("/api/config")
def r_config_set(req):
    return config_set(req.json()["changes"])


# ---------------------------------------------------------------- CUBUS / DEBUG section
@get("/api/cubus/status")
def r_cubus_status(req):
    cubus.SWD.reap()
    return {"profile": cubus.profile(), "swd_open": cubus.SWD.open, "blocked": cubus.CUBUS_BLOCKED, "uart": cubus.uart_report(),
            "uart_ports": OBC.uart_ports(), "result": STATE_RESULT("cubusrx")}


def STATE_RESULT(name):
    from tb.state import STATE
    return STATE.get(name)


@post("/api/cubus/snapshot")
def r_cubus_snapshot(req):
    _need_idle()
    return cubus.snapshot_report()


@post("/api/cubus/swd/close")
def r_cubus_swd_close(req):
    cubus.SWD.close()


@post("/api/cubus/pins")
def r_cubus_pins(req):
    return cubus.run_pin_check()


# ---------------------------------------------------------------- screenshots and reports
@get("/api/shots")
def r_shots(req):
    return list_shots()


@get(r"^/shots/([\w\-.]+\.png)$")
def r_shot_file(req):
    f = os.path.join(SHOT_DIR, req.m.group(1))
    return Raw(open(f, "rb").read(), "image/png") if os.path.isfile(f) else (404, {"error": "no such screenshot"})


@post("/api/shot")
def r_shot(req):
    c = req.json()
    return {"file": take_shot(c.get("route", "dashboard"), c.get("note", ""))}


@post("/api/shots/delete")
def r_shot_delete(req):
    f = os.path.basename(req.json()["file"])
    for x in (f, f + ".json"):
        try:
            os.remove(os.path.join(SHOT_DIR, x))
        except OSError:
            pass


@get("/api/reports")
def r_reports(req):
    return list_reports()


@get(r"^/reports/(\d{8}_\d{6})/([\w\-.]+)$")
def r_report_file(req):
    f = os.path.join(REPORT_DIR, req.m.group(1), req.m.group(2))
    if not os.path.isfile(f):
        raise HTTPError(404, "no such report file")
    return Raw(open(f, "rb").read(), REPORT_TYPES.get(f.rsplit(".", 1)[-1], "text/plain"))


@post("/api/report")
def r_report(req):
    c = req.json()
    return {"dir": save_report(c.get("note", ""), c.get("pdf", True))}


# ---------------------------------------------------------------- logs, trends, scenarios, alerts
def _log_args(q):
    srcs = [x for x in q.get("src", "").split(",") if x] or None
    return {"q": q.get("q", ""), "srcs": srcs, "t0": float(q["t0"]) if q.get("t0") else None, "t1": float(q["t1"]) if q.get("t1") else None, "regex": q.get("regex") == "1"}


@get("/api/logs")
def r_logs(req):
    r = LOGS.search(limit=min(int(req.q.get("limit", 300)), 2000), offset=int(req.q.get("offset", 0)), **_log_args(req.q))
    r["stats"] = LOGS.stats()
    return r


@get("/api/logs/export")
def r_logs_export(req):
    return Raw(LOGS.csv(**_log_args(req.q)).encode(), "text/csv", headers={"Content-Disposition": 'attachment; filename="s2s2_testbench_log.csv"'})


@get("/api/trends")
def r_trends(req):
    reps = trends.load_reports()
    return {"reports": [{"dir": r["dir"], "generated": r["generated"], "note": r["note"]} for r in reps], "tests": trends.build(reps)}


@get("/api/trends/diff")
def r_trends_diff(req):
    reps = {r["dir"]: r for r in trends.load_reports()}
    a, b = reps.get(req.q.get("a")), reps.get(req.q.get("b"))
    if not a or not b:
        raise ValueError("choose two saved reports")
    return {"a": a["dir"], "b": b["dir"], "diff": trends.diff(a, b)}


@get("/api/scenarios")
def r_scenarios(req):
    return scenarios.list_scenarios()


@post("/api/scenario/save")
def r_scenario_save(req):
    c = req.json()
    scenarios.save(c["name"], c["scenario"])


@post("/api/scenario/validate")
def r_scenario_validate(req):
    return {"errors": scenarios.validate(req.json().get("scenario"), TESTS.keys())}


@post("/api/scenario/delete")
def r_scenario_delete(req):
    scenarios.delete(req.json()["name"])


@get("/api/alerts")
def r_alerts(req):
    return {"alerts": WATCH.listing(), "settings": WATCH.settings}


@post("/api/alerts/ack")
def r_alerts_ack(req):
    WATCH.ack(req.json().get("id"))


@post("/api/watch")
def r_watch(req):
    return WATCH.set(req.json())


def on_error(path, exc):
    log(f"{path}: {exc}", "err")


H = httpx.make_handler(router, STATIC_DIRS, on_error=on_error, sse=httpx.subscribe_from(BUS))
