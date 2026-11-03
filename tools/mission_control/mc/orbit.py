"""mc.orbit -- where is the satellite? Keeps the satellite's TLE (two-line orbit data) and the ground-station position.

The TLE comes from CelesTrak by NORAD catalogue number or COSPAR id (a public API), from any address that answers with a TLE, or is pasted by the administrator; it is cached in the settings table
so the map still works without internet. The browser computes the position from the TLE (satellite.js), this module only fetches, checks
and stores. Nothing here touches the radio.
"""
import calendar
import json
import re
import threading
import time
import urllib.error
import urllib.request

from mc import core

CELESTRAK_BY = "https://celestrak.org/NORAD/elements/gp.php?%s=%s&FORMAT=tle"       # CATNR=<NORAD> or INTDES=<COSPAR>
DEFAULT_STATION = {"name": "Kathmandu (default, change in Settings)", "lat": 27.7172, "lon": 85.3240, "alt": 1.4}   # km above sea level
REFRESH_S = 6 * 3600
_lock = threading.Lock()


def _checksum_ok(line):
    if len(line) < 69 or not line[68].isdigit():
        return False
    tot = 0
    for ch in line[:68]:
        tot += int(ch) if ch.isdigit() else (1 if ch == "-" else 0)
    return tot % 10 == int(line[68])


def check_tle(l1, l2):
    l1, l2 = l1.rstrip(), l2.rstrip()
    if not (l1.startswith("1 ") and l2.startswith("2 ")):
        raise ValueError("line 1 must start with '1 ' and line 2 with '2 '")
    if len(l1) != 69 or len(l2) != 69:
        raise ValueError("each TLE line has 69 characters (%d and %d here)" % (len(l1), len(l2)))
    if not (_checksum_ok(l1) and _checksum_ok(l2)):
        raise ValueError("TLE checksum is wrong (a character was changed while copying?)")
    if l1[2:7] != l2[2:7]:
        raise ValueError("the two lines belong to different satellites")
    return l1, l2


def get():
    return {"tle": core.setting("tle", None), "station": core.setting("station", DEFAULT_STATION), "station_is_default": core.setting("station", None) is None,
            "launch": core.setting("launch", None)}


def set_launch(text):
    """Launch date and time in UTC ('YYYY-MM-DDTHH:MM[:SS]', empty clears it). Stored as unix seconds: the mission clock counts from it."""
    text = (text or "").strip()
    if not text:
        core.set_setting("launch", None)
        return
    try:
        t = calendar.timegm(time.strptime(text[:19], "%Y-%m-%dT%H:%M:%S" if len(text) > 16 else "%Y-%m-%dT%H:%M"))
    except ValueError:
        raise ValueError("launch time must look like 2026-10-29T06:30 (UTC)")
    core.set_setting("launch", t)


def set_station(st):
    lat, lon, alt = float(st["lat"]), float(st["lon"]), float(st.get("alt", 0))
    if not (-90 <= lat <= 90 and -180 <= lon <= 180):
        raise ValueError("latitude must be -90..90 and longitude -180..180")
    core.set_setting("station", {"name": str(st.get("name", "Ground station"))[:60], "lat": lat, "lon": lon, "alt": alt})


def set_manual(name, l1, l2):
    l1, l2 = check_tle(l1, l2)
    core.set_setting("tle", {"name": (name or "S2S2")[:40], "line1": l1, "line2": l2, "norad": int(l1[2:7]), "source": "pasted by an administrator",
                             "fetched": time.time(), "error": ""})


def norm_cospar(text):
    """International designator (COSPAR id) as CelesTrak wants it: 2026-123A. Accepts 2026-123A, 2026123A, 26123A and 26-123A."""
    t = (text or "").strip().upper().replace(" ", "")
    m = re.fullmatch(r"(\d{2}|\d{4})-?(\d{3})([A-Z]{1,3})", t)
    if not m:
        raise ValueError("a COSPAR / international designator looks like 2026-123A (year, launch number, piece letter)")
    yr = m.group(1)
    if len(yr) == 2:
        yr = ("19" if int(yr) >= 57 else "20") + yr
    return "%s-%s%s" % (yr, m.group(2), m.group(3))


def _tle_from_text(txt):
    """Name (optional) and the two lines out of a text answer: a 2- or 3-line TLE, or a CelesTrak / Space-Track GP JSON list with TLE_LINE1 / TLE_LINE2."""
    st = txt.lstrip()
    if st[:1] in "[{":
        try:
            j = json.loads(st)
        except ValueError:
            j = None
        if isinstance(j, dict):
            j = [j]
        if isinstance(j, list) and j and isinstance(j[0], dict):
            r = j[0]
            l1, l2 = r.get("TLE_LINE1") or r.get("tle_line1"), r.get("TLE_LINE2") or r.get("tle_line2")
            if l1 and l2:
                return str(r.get("OBJECT_NAME") or r.get("object_name") or "").strip(), l1, l2
            raise ValueError("the JSON answer has no TLE_LINE1 / TLE_LINE2 fields (an OMM without TLE lines cannot be used)")
    lines = [ln.rstrip() for ln in txt.splitlines() if ln.strip()]
    for i, ln in enumerate(lines):
        if ln.startswith("1 ") and i + 1 < len(lines) and lines[i + 1].startswith("2 "):
            return (lines[i - 1].strip() if i > 0 else ""), ln, lines[i + 1]
    raise ValueError("the answer holds no TLE (%s)" % ((lines[0] if lines else "empty answer")[:60]))


def _get(url):
    req = urllib.request.Request(url, headers={"User-Agent": "S2S2-MissionControl"})
    with urllib.request.urlopen(req, timeout=12) as r:
        return r.read(65536).decode("utf-8", "replace")


def _query_of(norad, cospar, url, cur):
    """Which source to ask: the arguments given now, else the one stored with the last good TLE, else the stored NORAD number."""
    if url:
        if not re.match(r"https?://", url.strip(), re.I):
            raise ValueError("the TLE address must start with http:// or https://")
        return {"url": url.strip()}
    if cospar:
        return {"cospar": norm_cospar(cospar)}
    if norad:
        return {"norad": int(norad)}
    q = cur.get("query") or ({"norad": int(cur["norad"])} if cur.get("norad") else None)
    if not q:
        raise ValueError("no NORAD number, COSPAR id or TLE address set")
    return q


def refresh(norad=None, cospar=None, url=None):
    """Fetch the newest TLE: from CelesTrak by NORAD number or by COSPAR id (international designator), or from any address that answers with a
    TLE (a CelesTrak query of your own, Space-Track, a ground-station web page). Returns the stored record; on failure the old TLE stays and
    `error` explains."""
    with _lock:
        cur = core.setting("tle", None) or {}
        q = None
        try:
            q = _query_of(norad, cospar, url, cur)
            if "url" in q:
                target, label = q["url"], "custom address"
            elif "cospar" in q:
                target, label = CELESTRAK_BY % ("INTDES", q["cospar"]), "CelesTrak, COSPAR " + q["cospar"]
            else:
                target, label = CELESTRAK_BY % ("CATNR", q["norad"]), "CelesTrak"
            try:
                txt = _get(target)
            except urllib.error.HTTPError as e:
                body = ""
                try:
                    body = e.read(200).decode("ascii", "replace").strip()
                except Exception:
                    pass
                if e.code == 404 and "url" not in q:
                    what = ("COSPAR id " + q["cospar"]) if "cospar" in q else ("NORAD %d" % q["norad"])
                    raise ValueError("CelesTrak has no orbit data for %s (%s). The satellite is not in the public catalogue yet, or under another number. "
                                     "Try its COSPAR id, give an address that returns a TLE, or paste the TLE under Settings." % (what, body or "HTTP 404"))
                raise ValueError("HTTP %d from the TLE source%s" % (e.code, (": " + body[:80]) if body else ""))
            name, l1, l2 = _tle_from_text(txt)
            l1, l2 = check_tle(l1, l2)
            rec = {"name": (name or cur.get("name") or "S2S2")[:40], "line1": l1, "line2": l2, "norad": int(l1[2:7]), "source": label,
                   "query": q, "fetched": time.time(), "error": ""}
        except (urllib.error.URLError, OSError, ValueError) as e:
            rec = {**cur, "error": "could not update: %s" % e}
            if q:
                rec["query"] = cur.get("query") or q
        core.set_setting("tle", rec)
        return rec


def _loop():
    while True:
        time.sleep(300)
        try:
            t = core.setting("tle", None)
            if t and t.get("query") and time.time() - t.get("fetched", 0) > REFRESH_S and t.get("source") != "pasted by an administrator":
                refresh()
        except Exception:
            pass


threading.Thread(target=_loop, daemon=True).start()
