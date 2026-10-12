#!/usr/bin/env python3
"""
ui_smoke.py -- open every page of a web tool in headless Chrome (DevTools protocol) and report JavaScript errors and empty pages.
No hardware is touched: the servers are started without any serial port, SWD probe or SDR.

    python3 tests/host/ui_smoke.py http://127.0.0.1:8765 dashboard consoles profiler ...
    python3 tests/host/ui_smoke.py http://127.0.0.1:8770 overview attitude ... --login admin:PASSWORD
"""
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "testbench"))
from tb import reporting                                                  # noqa: E402
from websockets.sync.client import connect                        # noqa: E402


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    login = None
    if "--login" in sys.argv:
        login = sys.argv[sys.argv.index("--login") + 1]
        args = [a for a in args if a != login]
    base, pages = args[0], args[1:]
    shot_dir = None
    if "--shots" in sys.argv:
        shot_dir = sys.argv[sys.argv.index("--shots") + 1]
        args = [a for a in args if a != shot_dir]
        pages = [p for p in pages if p != shot_dir]
        os.makedirs(shot_dir, exist_ok=True)
    s = socket.socket(); s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]; s.close()
    prof = tempfile.mkdtemp(prefix="s2s2smoke")
    p = subprocess.Popen([reporting.CHROME, "--headless=new", "--disable-gpu", "--no-first-run", f"--remote-debugging-port={port}",
                          f"--user-data-dir={prof}", "--window-size=1500,1000", "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    bad = 0
    try:
        ws_url = None
        for _ in range(100):
            try:
                pg = [t for t in json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=1)) if t.get("type") == "page"]
                if pg:
                    ws_url = pg[0]["webSocketDebuggerUrl"]; break
            except OSError:
                time.sleep(0.2)
        with connect(ws_url, max_size=64 * 1024 * 1024) as ws:
            n = [0]; errors = []

            def call(method, **params):
                n[0] += 1
                ws.send(json.dumps({"id": n[0], "method": method, "params": params}))
                while True:
                    m = json.loads(ws.recv(timeout=30))
                    if "method" in m:
                        if m["method"] == "Runtime.exceptionThrown":
                            errors.append(m["params"]["exceptionDetails"].get("exception", {}).get("description", "exception")[:300])
                        elif m["method"] == "Runtime.consoleAPICalled" and m["params"]["type"] == "error":
                            errors.append(" ".join(str(a.get("value", a.get("description", ""))) for a in m["params"]["args"])[:300])
                        continue
                    if m.get("id") == n[0]:
                        return m.get("result", {})

            def ev(js):
                r = call("Runtime.evaluate", expression=js, awaitPromise=True, returnByValue=True)
                return r.get("result", {}).get("value")
            call("Runtime.enable"); call("Page.enable")
            call("Page.navigate", url=base + "/"); time.sleep(1.5)
            if login:
                u, pw = login.split(":", 1)
                ev("fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(%s)}).then(r=>r.status)" % json.dumps({"user": u, "password": pw}))
                call("Page.navigate", url=base + "/"); time.sleep(1.5)
            for pgname in pages:
                errors.clear()
                ev(f"location.hash='{pgname}'"); time.sleep(1.8)
                txt = ev("document.querySelector('.main') ? document.querySelector('.main').innerText.length : (document.getElementById('root').innerText.length)") or 0
                ok = txt > 40 and not errors
                bad += 0 if ok else 1
                print(("ok   " if ok else "FAIL ") + pgname.ljust(12), f"{txt} chars", errors[:2] if errors else "")
                if shot_dir:
                    import base64
                    open(os.path.join(shot_dir, pgname + ".png"), "wb").write(base64.b64decode(call("Page.captureScreenshot", format="png")["data"]))
    finally:
        try:
            os.killpg(p.pid, 9)
        except OSError:
            p.kill()
        shutil.rmtree(prof, ignore_errors=True)
    print("UI smoke:", "FAILED" if bad else "OK")
    sys.exit(1 if bad else 0)


main()
