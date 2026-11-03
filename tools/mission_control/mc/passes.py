"""mc.passes -- pass prediction on the server (the browser's orbit code, run through node) so recording, heartbeat and automatic sampling know when the satellite is overhead
even when no browser is open. Needs `node` on the PATH; without it, or without orbit data, predict() returns an empty list and says why."""
import json
import os
import shutil
import subprocess
import threading
import time

from mc import core, orbit

HERE = os.path.dirname(os.path.abspath(__file__))
_cache = {"key": None, "t": 0.0, "v": {"passes": [], "reason": "not computed"}}
_lock = threading.Lock()


def predict(start_ts=None, hours=48, min_el=5.0, max_age=600):
    """{"passes": [{aos, los, maxEl, maxT, azIn, azOut}], "reason": str}; results are cached for `max_age` seconds."""
    start_ts = start_ts or time.time()
    tle = core.setting("tle", None)
    st = core.setting("station", orbit.DEFAULT_STATION)
    if not tle or not tle.get("line1"):
        return {"passes": [], "reason": "no orbit data (TLE) is stored"}
    node = shutil.which("node")
    if not node:
        return {"passes": [], "reason": "node is not installed (needed for pass prediction)"}
    key = (tle["line1"], tle["line2"], st.get("lat"), st.get("lon"), st.get("alt"), int(start_ts // 600), hours, min_el)
    with _lock:
        if _cache["key"] == key and time.time() - _cache["t"] < max_age:
            return _cache["v"]
        arg = {"line1": tle["line1"], "line2": tle["line2"], "lat": st["lat"], "lon": st["lon"], "alt": st.get("alt", 0), "start": start_ts, "hours": hours, "minEl": min_el}
        try:
            r = subprocess.run([node, os.path.join(HERE, "passes.js")], input=json.dumps(arg), capture_output=True, text=True, timeout=60)
            v = json.loads(r.stdout) if r.returncode == 0 and r.stdout.strip() else {"passes": [], "reason": "pass prediction failed: " + r.stderr[-200:]}
        except (OSError, ValueError, subprocess.TimeoutExpired) as e:
            v = {"passes": [], "reason": "pass prediction failed: %s" % e}
        v.setdefault("reason", "")
        _cache.update(key=key, t=time.time(), v=v)
        return v


def current(now=None):
    """The pass in progress (aos <= now <= los) or None."""
    now = now or time.time()
    for p in predict(now - 3600, 50)["passes"]:
        if p["aos"] <= now <= p["los"]:
            return p
    return None


def upcoming(now=None):
    now = now or time.time()
    for p in predict(now - 3600, 50)["passes"]:
        if p["los"] > now:
            return p
    return None
