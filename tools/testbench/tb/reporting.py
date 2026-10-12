"""
reporting.py -- screenshots, ZIP bundles and PDF of the testbench pages / reports, using the Google Chrome already on the Mac
(headless, its own throw-away profile, so the user's browser is not touched).
"""
import os
import shutil
import subprocess
import tempfile
import time
import zipfile

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"


def available():
    return os.path.exists(CHROME)


def _run(extra, out, timeout=45):
    """Start headless Chrome, wait until `out` exists and stops growing, then kill it (it does not exit by itself)."""
    if not available():
        raise RuntimeError("Google Chrome not found at " + CHROME)
    prof = tempfile.mkdtemp(prefix="s2s2chrome")
    if os.path.exists(out):
        os.remove(out)
    p = subprocess.Popen([CHROME, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check", "--hide-scrollbars",
                          "--user-data-dir=" + prof] + extra, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    t0, last = time.time(), -1
    try:
        while time.time() - t0 < timeout:
            if os.path.exists(out):
                sz = os.path.getsize(out)
                if sz > 0 and sz == last:
                    return out
                last = sz
            time.sleep(0.4)
        raise RuntimeError("Chrome produced no output in %d s" % timeout)
    finally:
        try:
            os.killpg(p.pid, 9)
        except OSError:
            p.kill()
        shutil.rmtree(prof, ignore_errors=True)


def screenshot(url, out_png, width=1500, height=1100, wait_s=6.0):
    """Render a live page in headless Chrome and save it as PNG. Uses the DevTools protocol (a page with a long-lived EventSource never
    'finishes loading', so the plain --screenshot switch cannot be used): load, wait `wait_s` for the data to arrive, capture."""
    import base64
    import json
    import socket
    import urllib.request
    from websockets.sync.client import connect
    if not available():
        raise RuntimeError("Google Chrome not found at " + CHROME)
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    prof = tempfile.mkdtemp(prefix="s2s2chrome")
    p = subprocess.Popen([CHROME, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check", "--hide-scrollbars",
                          f"--remote-debugging-port={port}", "--user-data-dir=" + prof, f"--window-size={width},{height}", "about:blank"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
    try:
        ws_url = None
        for _ in range(100):
            try:
                tabs = json.load(urllib.request.urlopen(f"http://127.0.0.1:{port}/json/list", timeout=1))
                pages = [t for t in tabs if t.get("type") == "page"]
                if pages:
                    ws_url = pages[0]["webSocketDebuggerUrl"]
                    break
            except OSError:
                pass
            time.sleep(0.2)
        if not ws_url:
            raise RuntimeError("Chrome did not start")
        with connect(ws_url, max_size=64 * 1024 * 1024) as ws:
            n = [0]

            def call(method, **params):
                n[0] += 1
                ws.send(json.dumps({"id": n[0], "method": method, "params": params}))
                while True:
                    m = json.loads(ws.recv(timeout=30))
                    if m.get("id") == n[0]:
                        if "error" in m:
                            raise RuntimeError(method + ": " + m["error"].get("message", "?"))
                        return m.get("result", {})
            call("Emulation.setDeviceMetricsOverride", width=width, height=height, deviceScaleFactor=1, mobile=False)
            call("Page.navigate", url=url)
            time.sleep(wait_s)
            data = call("Page.captureScreenshot", format="png")["data"]
        with open(out_png, "wb") as f:
            f.write(base64.b64decode(data))
        return out_png
    finally:
        try:
            os.killpg(p.pid, 9)
        except OSError:
            p.kill()
        shutil.rmtree(prof, ignore_errors=True)


def pdf(src_html, out_pdf):
    return _run(["--no-pdf-header-footer", f"--print-to-pdf={out_pdf}", "file://" + os.path.abspath(src_html)], out_pdf)


def zip_dir(d, out_zip):
    with zipfile.ZipFile(out_zip, "w", zipfile.ZIP_DEFLATED) as z:
        for root, _, files in os.walk(d):
            for f in files:
                p = os.path.join(root, f)
                if os.path.abspath(p) != os.path.abspath(out_zip):
                    z.write(p, os.path.join(os.path.basename(d), os.path.relpath(p, d)))
    return out_zip
