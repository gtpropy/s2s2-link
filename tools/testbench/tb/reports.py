"""tb.reports"""

import base64
import datetime
import html
import json
import os
import re
import shutil
import time
from tb.bus import DATA, EVENTS, IMAGES, log
from tb import reporting
from tb.registry import TESTS
from tb.compat import imagestore
from tb.imaging import IMG, kind_of
from tb.links import GS, OBC
from tb.profiling import PROF
from tb.state import PROG, STATE
from tb.status import get_status
from tb.util import REPORT_DIR, now

SHOT_DIR = os.path.join(DATA, "shots")
START_T = now()
PORT = [8765]


def take_shot(route, note=""):
    """Screenshot of one testbench page (headless Chrome renders the live page with its current data)."""
    os.makedirs(SHOT_DIR, exist_ok=True)
    slug = re.sub(r"[^A-Za-z0-9_-]", "", route) or "page"
    fn = time.strftime("%Y%m%d_%H%M%S") + "_" + slug + ".png"
    out = os.path.join(SHOT_DIR, fn)
    reporting.screenshot(f"http://127.0.0.1:{PORT[0]}/#{route}", out)
    json.dump({"route": route, "note": note, "t": now()}, open(out + ".json", "w"))
    log(f"screenshot saved: {fn}", "ok")
    return fn


def list_shots():
    out = []
    if os.path.isdir(SHOT_DIR):
        for f in sorted(os.listdir(SHOT_DIR), reverse=True):
            if f.endswith(".png"):
                try:
                    meta = json.load(open(os.path.join(SHOT_DIR, f + ".json")))
                except (OSError, ValueError):
                    meta = {}
                out.append({"file": f, "route": meta.get("route", ""), "note": meta.get("note", ""), "t": meta.get("t") or os.path.getmtime(os.path.join(SHOT_DIR, f))})
    return out


def list_reports():
    out = []
    if os.path.isdir(REPORT_DIR):
        for dn in sorted(os.listdir(REPORT_DIR), reverse=True):
            d = os.path.join(REPORT_DIR, dn)
            if os.path.isfile(os.path.join(d, "report.html")):
                try:
                    rep = json.load(open(os.path.join(d, "report.json")))
                except (OSError, ValueError):
                    rep = {}
                res = rep.get("results") or {}
                out.append({"dir": dn, "note": rep.get("note", ""), "generated": rep.get("generated"),
                            "verdicts": {k: v.get("verdict") for k, v in res.items() if v}, "zip": os.path.exists(os.path.join(d, "report.zip")),
                            "pdf": os.path.exists(os.path.join(d, "report.pdf")), "shots": len(os.listdir(os.path.join(d, "screenshots"))) if os.path.isdir(os.path.join(d, "screenshots")) else 0})
    return out


def save_report(note="", make_pdf=True):
    """Write docs/reports/<time>/: report.html/json/pdf/zip, screenshots, profiler snapshot, source + received file, raw GS and Nucleo logs, LED trace."""
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    d = os.path.join(REPORT_DIR, ts)
    os.makedirs(d, exist_ok=True)
    st = get_status()
    rep = {"generated": datetime.datetime.now().isoformat(timespec="seconds"), "git": st["git"],
           "note": note, "sat_bin": st["sat_build"], "gs_bin": st["gs_build"],
           "results": st["results"]}
    with open(os.path.join(d, "report.json"), "w") as f:
        json.dump(rep, f, indent=2, default=str)
    with open(os.path.join(d, "gs_uart.log"), "w") as f:
        with GS.lock:
            for t, txt in GS.lines:
                f.write(f"{t:.3f} {txt}\n")
    if PROG.get("led"):
        json.dump(PROG["led"], open(os.path.join(d, "led_trace.json"), "w"))
    with open(os.path.join(d, "sat_vcp.log"), "w") as f:
        with OBC.lock:
            for t, txt in OBC.lines:
                f.write(f"{t:.3f} {txt}\n")
            for t, cmd, seq, status in OBC.acks:
                f.write(f"{t:.3f} ACK-FRAME cmd={cmd} seq={seq} status={status}\n")
    with open(os.path.join(d, "events.json"), "w") as f:
        json.dump(EVENTS.recent(limit=2000), f, default=str)
    imgs = ""
    if IMG["bytes"]:
        ext, mime = kind_of(IMG["bytes"])
        open(os.path.join(d, f"source.{ext}"), "wb").write(IMG["bytes"])
        imgs += f'<figure><img src="data:{mime};base64,{base64.b64encode(IMG["bytes"]).decode()}"><figcaption>sent to the Nucleo — {IMG["name"]}, {len(IMG["bytes"])} B</figcaption></figure>'
        if IMG.get("rx"):
            open(os.path.join(d, f"received.{ext}"), "wb").write(IMG["rx"])
            imgs += f'<figure><img src="data:{mime};base64,{base64.b64encode(IMG["rx"]).decode()}"><figcaption>received by the ground station over the air</figcaption></figure>'
    for sess in IMAGES.list()[:4]:                                # pictures rebuilt in the background from the receivers
        r = IMAGES.image(sess["id"])
        if r:
            open(os.path.join(d, f"background_{sess['id']}.{imagestore.sniff(r[0])[0]}"), "wb").write(r[0])
            imgs += (f'<figure><img src="data:{r[1]};base64,{base64.b64encode(r[0]).decode()}"><figcaption>background builder {sess["id"]}: '
                     f'{sess["got"]}/{sess["total"]} fragments, sources {sess["sources"]}{", = file sent" if sess["matches_sent"] else ""}</figcaption></figure>')
    sdrimg = STATE.get("sdrimage") or {}
    if sdrimg.get("raw_frames"):                  # the frames Direwolf delivered, as hex: AX.25 header + information field, no flags / FCS
        open(os.path.join(d, "direwolf_frames.txt"), "w").write(
            "# one frame per line, hex: 7 B destination, 7 B source, control, PID, information field (what Direwolf delivered over KISS)\n"
            + "\n".join(sdrimg["raw_frames"]) + "\n")
    rows = ""
    for k in TESTS:
        r = st["results"].get(k)
        if not r:
            rows += f"<tr><td>{k}</td><td>not run</td><td></td></tr>"
            continue
        det = "<br>".join(f"{html.escape(str(a))}: {html.escape(str(b))}" for a, b in r.items()
                          if a not in ("rows", "test", "when", "verdict", "missing", "detail", "raw_frames", "steps")
                          and not (a == "frames" and isinstance(b, list)))
        if r.get("steps"):
            det += "<br>" + "<br>".join(f"<b>{x['n']}. {html.escape(x['name'])}</b>: {x['state'].upper()} — {html.escape(x['detail'])}" for x in r["steps"])
        if r.get("rows"):
            det += "<br>" + "<br>".join(html.escape(json.dumps(x)) for x in r["rows"])
        if isinstance(r.get("frames"), list):
            det += "<br>" + "<br>".join(html.escape(x) for x in r["frames"][:10])
        if r.get("missing"):
            det += "<br>missing chunks: " + html.escape(str(r["missing"]))
        rows += f"<tr><td>{k}</td><td class='{r.get('verdict','')}'>{r.get('verdict')}</td><td>{det}</td></tr>"
    # profiler snapshot
    profhtml = ""
    snaps = {}
    for tname, sess in PROF.sessions.items():
        stt = sess.state()
        snaps[tname] = {"rate": stt["rate"], "cores": stt["cores"], "watch": stt["watch"], "uptime": stt["uptime"]}
        for c in stt["cores"]:
            profhtml += (f"<h3>{tname}: {html.escape(c['core'])} — busy {c['busy_pct']} %, {c['samples']} PC samples</h3><table><tr><th>function</th><th>share of samples</th></tr>"
                         + "".join(f"<tr><td>{html.escape(x['fn'])}</td><td>{x['pct']} %</td></tr>" for x in c["lifetime"][:12]) + "</table>")
            if c.get("stack") and c["stack"].get("used") is not None:
                m = c["stack"]
                profhtml += f"<p>stack high-water mark: {m['used']} B used, {m['free']} B free (top 0x{m['top']:X})</p>"
    if snaps:
        json.dump(snaps, open(os.path.join(d, "profiler.json"), "w"), default=str)
    for t_name in PROF.targets:
        try:
            json.dump(PROF.layout(t_name), open(os.path.join(d, f"memory_layout_{t_name}.json"), "w"), default=str)
        except Exception:
            pass
    # screenshots taken since the server started
    shots_html = ""
    sd = os.path.join(d, "screenshots")
    for sh in reversed(list_shots()):
        if sh["t"] >= START_T - 5:
            os.makedirs(sd, exist_ok=True)
            shutil.copy(os.path.join(SHOT_DIR, sh["file"]), os.path.join(sd, sh["file"]))
            b64 = base64.b64encode(open(os.path.join(SHOT_DIR, sh["file"]), "rb").read()).decode()
            shots_html += f'<figure style="display:block;max-width:100%"><img style="max-width:100%" src="data:image/png;base64,{b64}"><figcaption>{html.escape(sh["route"])} {html.escape(sh["note"])}</figcaption></figure>'
    hk = EVENTS.latest.get("hk")
    hkhtml = ""
    if hk:
        hkhtml = "<h2>Last housekeeping packet</h2><pre>" + html.escape(json.dumps({k: v for k, v in hk.items() if k != "hex"}, indent=1)) + "</pre>"
    page = f"""<!doctype html><meta charset=utf-8><title>S2S2 testbench report {ts}</title>
<style>body{{font:14px system-ui;margin:2rem;max-width:1000px}}td,th{{border:1px solid #ccc;padding:6px;vertical-align:top}}
table{{border-collapse:collapse;width:100%}}.PASS{{color:#0a7a2f;font-weight:700}}.FAIL{{color:#b00020;font-weight:700}}
.PARTIAL{{color:#b36b00;font-weight:700}}figure{{display:inline-block;margin:0 1rem 1rem 0}}img{{max-width:320px;image-rendering:auto}}pre{{background:#f4f4f4;padding:8px;overflow:auto}}</style>
<h1>S2S2 testbench report</h1><p>{rep['generated']} · git {st['git']} · satellite.bin {st['sat_build'] and st['sat_build']['bin']} B ·
ground_station.bin {st['gs_build'] and st['gs_build']['bin']} B</p><p>{html.escape(note)}</p>
<table><tr><th>test</th><th>verdict</th><th>details</th></tr>{rows}</table>{hkhtml}<h2>Profiler</h2>{profhtml or '<p>profiler was not running</p>'}
<h2>Images</h2>{imgs or '<p>none</p>'}<h2>Screenshots</h2>{shots_html or '<p>none taken</p>'}
<p>Raw logs: gs_uart.log (ground station), sat_vcp.log (Nucleo ST-Link VCP), events.json · machine-readable: report.json, profiler.json</p>"""
    open(os.path.join(d, "report.html"), "w").write(page)
    pdf_note = ""
    if make_pdf:
        try:
            reporting.pdf(os.path.join(d, "report.html"), os.path.join(d, "report.pdf"))
        except Exception as e:
            pdf_note = f" (PDF failed: {e})"
    try:
        reporting.zip_dir(d, os.path.join(d, "report.zip"))
    except Exception as e:
        pdf_note += f" (ZIP failed: {e})"
    log(f"report saved: {d}{pdf_note}", "ok" if not pdf_note else "warn")
    return d
