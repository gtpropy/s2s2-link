"""mc.web -- HTTP routes of Mission Control (tools/common/httpx.py provides the server machinery)."""
import json
import os
import time

import authz
import httpx
import imagestore
from httpx import Raw
from mc import core, gdrive, link, orbit, passes, store, xlsxio, docxgen
from mc.core import BUS, CATALOG, rawlog, CUR, DBL, DB, EVENTS, HERE, IMAGES, JOBS, ROOT, audit, csv_export, history_jobs, job_update, now, series, set_setting, snapshot, submit

COOKIE = "s2s2mc"
router = httpx.Router()
get, post = router.get, router.post
STATIC_DIRS = [("", os.path.join(HERE, "static")), ("", os.path.join(ROOT, "tools", "common", "web"))]
SECURITY = {"X-Frame-Options": "DENY", "Content-Security-Policy": "default-src 'self'; img-src 'self' data: blob:; style-src 'self' 'unsafe-inline'"}


def authenticate(req):
    tok = req.cookie(COOKIE)
    return core.AUTH.user_of(tok), tok


def allows(user, role):
    return bool(user) and core.AUTH.allows(user, role)


def body(req):
    """JSON body of a state-changing request. SameSite=Strict cookies plus a JSON-only content type keep other sites from posting here."""
    if req.headers.get("Content-Length", "0") != "0" and "application/json" not in (req.headers.get("Content-Type") or ""):
        raise ValueError("JSON expected")
    return req.json()


# ---------------------------------------------------------------- session
@get("/api/me", public=True)
def r_me(req):
    return {"user": req.user, "role": core.AUTH.role_of(req.user) if req.user else None}


@post("/api/login", public=True)
def r_login(req):
    d = body(req)
    user = str(d.get("user", ""))[:40]
    tok = core.AUTH.login(user, str(d.get("password", "")))
    if not tok:
        audit(user, "login failed")
        time.sleep(0.5)
        return (401, {"error": "wrong user or password (5 failures lock the account for 5 minutes)"})
    audit(user, "login")
    return Raw(json.dumps({"user": user, "role": core.AUTH.role_of(user)}).encode(), "application/json",
               headers={"Set-Cookie": f"{COOKIE}={tok}; HttpOnly; SameSite=Strict; Path=/; Max-Age=43200"})


@post("/api/logout")
def r_logout(req):
    core.AUTH.logout(req.token)
    audit(req.user, "logout")
    return Raw(b'{"ok":true}', "application/json", headers={"Set-Cookie": f"{COOKIE}=; Max-Age=0; Path=/"})


# ---------------------------------------------------------------- monitoring
@get("/api/snapshot")
def r_snapshot(req):
    return snapshot()


@get("/api/series")
def r_series(req):
    return series(req.q.get("kind", "hk"), int(req.q.get("n", 500)))


@get("/api/events")
def r_events(req):
    if req.q.get("since"):                                   # a time range from the database (trend charts), oldest first
        kind = req.q.get("kind")
        return EVENTS.between(float(req.q["since"]), time.time() + 1, [kind] if kind else None)[-int(req.q.get("limit", 20000)):]
    return EVENTS.history(req.q.get("kind"), int(req.q.get("limit", 300)))


@get("/api/export")
def r_export(req):
    k = req.q.get("kind", "hk")
    if k not in ("hk", "beacon", "ack", "nack", "cw"):
        raise ValueError("bad kind")
    return Raw(csv_export(k).encode(), "text/csv", headers={"Content-Disposition": f'attachment; filename="s2s2_{k}.csv"'})


@get("/api/rawlog")
def r_rawlog(req):
    return rawlog(req.q.get("n", 500), req.q.get("q", "")[:80], req.q.get("kind", ""))


@get("/api/orbit")
def r_orbit(req):
    return orbit.get()


@post("/api/orbit", role="admin")
def r_orbit_set(req):
    d = body(req)
    try:
        if d.get("station"):
            orbit.set_station(d["station"])
        if "launch" in d:
            orbit.set_launch(str(d["launch"] or ""))
        if d.get("line1") and d.get("line2"):
            orbit.set_manual(str(d.get("name", "")), str(d["line1"]), str(d["line2"]))
        elif d.get("norad") or d.get("cospar") or d.get("url") or d.get("refresh"):
            rec = orbit.refresh(d.get("norad"), d.get("cospar"), d.get("url"))
            if rec.get("error"):
                audit(req.user, "orbit", rec["error"])
    except (ValueError, KeyError) as e:
        return (400, {"error": str(e)})
    audit(req.user, "orbit", json.dumps({k: v for k, v in d.items() if k in ("norad", "cospar", "url", "refresh", "station", "launch")}))
    return orbit.get()


@get("/api/images")
def r_images(req):
    return IMAGES.list()


@get(r"^/api/images/([A-Za-z0-9_]+)/(image|meta)$")
def r_image(req):
    sid, what = req.m.group(1), req.m.group(2)
    if what == "meta":
        return IMAGES.get(sid) or {}
    r = IMAGES.image(sid)
    if not r:
        return (404, {"error": "no such picture"})
    hd = {"Content-Disposition": f'attachment; filename="{sid}.{imagestore.sniff(r[0])[0]}"'} if "download" in req.q else None
    return Raw(r[0], r[1], headers=hd)


# ---------------------------------------------------------------- commands
@get("/api/catalog")
def r_catalog(req):
    return [{**c, "allowed": True} for c in CATALOG]                  # every command may be sent by every signed-in operator (no role check, no password)


@get("/api/commands")
def r_commands(req):
    return history_jobs(int(req.q.get("n", 100)))


@post("/api/command")
def r_command(req):
    d = body(req)
    c = next((x for x in CATALOG if x["id"] == d.get("id")), None)
    if not c:
        raise ValueError("unknown command")
    try:
        jid = submit(req.user, c["id"], d.get("args", {}))
    except (RuntimeError, ValueError) as e:
        return (400, {"error": str(e)})
    audit(req.user, "command", f"{c['id']} -> {JOBS[jid]['line']}")
    return {"job": jid}


@post("/api/command/cancel")
def r_cancel(req):
    jid = int(body(req)["id"])
    if CUR["id"] == jid:
        CUR["abort"].set()
    elif jid in JOBS and JOBS[jid]["state"] == "queued":
        job_update(jid, state="cancelled", t_end=now())
    audit(req.user, "cancel", str(jid))



# ---------------------------------------------------------------- link supervision (heartbeat, automatic sampling)
@get("/api/link")
def r_link(req):
    return link.state()


@post("/api/link")
def r_link_set(req):
    d = body(req)
    audit(req.user, "link settings", json.dumps({k: d[k] for k in ("enabled", "autosample") if k in d}))
    return link.set_cfg(**{k: d[k] for k in ("enabled", "autosample") if k in d})


@get("/api/passes")
def r_passes(req):
    return passes.predict(time.time() - 3600, int(req.q.get("hours", 50)))


# ---------------------------------------------------------------- data store, reports, Google Drive
@get("/api/home")
def r_home(req):
    st = store.status()
    days = store.list_days()
    return {"gs": snapshot()["gs"], "link": link.state(), "store": st, "days": days, "retention_days": st["cfg"]["retention_days"], "drive": gdrive.public_config(), "t": now()}


@get("/api/drive/stats")
def r_drive_stats(req):
    return gdrive.stats(max_age=int(req.q.get("age", 120)))


@post("/api/store/flush")
def r_flush(req):
    store.flush(force=True)
    return store.status()


@post("/api/store/upload")
def r_upload(req):
    audit(req.user, "drive upload", "manual")
    return gdrive.sync_all()


@post("/api/store/cfg", role="admin")
def r_store_cfg(req):
    d = body(req)
    c = store.cfg()
    if d.get("retention_days"):
        c["retention_days"] = max(1, min(365, int(d["retention_days"])))
    if "delete_requires_upload" in d:
        c["delete_requires_upload"] = bool(d["delete_requires_upload"])
    core.set_setting("store_cfg", c)
    audit(req.user, "store settings", json.dumps(c))
    return store.status()


@get("/api/gdrive")
def r_gdrive(req):
    return gdrive.public_config()


@post("/api/gdrive", role="admin")
def r_gdrive_set(req):
    try:
        r = gdrive.save_config(body(req))
    except (ValueError, KeyError) as e:
        return (400, {"error": str(e)})
    audit(req.user, "gdrive settings", json.dumps({k: v for k, v in r.items() if k != "error"}))
    return r


@get("/api/gdrive/oauth", role="admin")
def r_gdrive_oauth(req):
    redirect = "http://%s/api/gdrive/callback" % req.headers.get("Host", "127.0.0.1:8770")
    if not gdrive.cfg().get("client_id"):
        return (400, {"error": "enter the OAuth client id first"})
    return {"url": gdrive.oauth_url(redirect), "redirect": redirect}


@get("/api/gdrive/callback")
def r_gdrive_cb(req):
    redirect = "http://%s/api/gdrive/callback" % req.headers.get("Host", "127.0.0.1:8770")
    try:
        gdrive.oauth_exchange(req.q.get("code", ""), redirect)
        audit(req.user, "gdrive authorised", "")
        msg = "Google Drive is authorised. You can close this tab."
    except Exception as e:
        msg = "Authorisation failed: %s" % e
    return Raw(("<html><body style='font-family:sans-serif;padding:2rem'><h2>%s</h2><a href='/#home'>Back to Mission Control</a></body></html>" % msg).encode(), "text/html")


@get("/api/reports")
def r_reports(req):
    local = {d["name"]: d for d in store.list_days()}
    out = [{**d, "drive": False} for d in local.values()]
    try:
        for d in gdrive.list_days():
            if d["name"] in local:
                local[d["name"]]["drive"] = True
            else:
                out.append({"name": d["name"], "date": d["name"][:10], "local": False, "drive": True, "bytes": None, "files": None, "uploaded": True})
    except Exception as e:
        return {"days": sorted(out, key=lambda x: x["name"], reverse=True), "drive_error": str(e)}
    return {"days": sorted(out, key=lambda x: x["name"], reverse=True)}


@post("/api/reports/pull")
def r_pull(req):
    name = str(body(req)["name"])
    if store.day_path(name):
        return {"pulled": False, "where": "local"}
    try:
        r = gdrive.download_day(name, store.PULLED)
    except Exception as e:
        return (400, {"error": str(e)})
    audit(req.user, "report pulled", name)
    return {"pulled": True, "where": "pulled", **r}


def _day(req):
    name = req.q.get("name", "")
    p = store.day_path(name) or store.day_path(name, pulled=True)
    if not p:
        raise ValueError("that day is not on this computer (pull it first)")
    return p


@get("/api/report")
def r_report(req):
    p = _day(req)
    files = store.tree(p)
    summary = None
    g = os.path.join(p, "GS_report.xlsx")
    if os.path.exists(g):
        try:
            summary = {s["name"]: s["rows"] for s in xlsxio.read_xlsx(g, 300)}
        except Exception:
            summary = None
    ds = os.path.join(p, "Storage", "DAY_SUMMARY", "Day_Summary.xlsx")
    if summary is not None and os.path.exists(ds):                       # the day summary sheets appear as extra tabs of the report viewer
        try:
            for s in xlsxio.read_xlsx(ds, 300):
                if s["name"] in ("At a glance", "Anomalies", "Contacts", "Satellite health", "Link quality", "Environment"):
                    summary["Day: " + s["name"] if s["name"] == "Link quality" else s["name"]] = s["rows"]
        except Exception:
            pass
    return {"name": req.q.get("name"), "files": files, "gs_report": summary}


def _file(req):
    p = _day(req)
    rel = os.path.normpath(req.q.get("path", ""))
    full = os.path.normpath(os.path.join(p, rel))
    if rel.startswith("..") or not full.startswith(p + os.sep) or not os.path.isfile(full):
        raise ValueError("no such file")
    return full


@get("/api/report/file")
def r_report_file(req):
    full = _file(req)
    ext = os.path.splitext(full)[1].lower()
    mime = {".jpeg": "image/jpeg", ".jpg": "image/jpeg", ".png": "image/png", ".txt": "text/plain; charset=utf-8", ".log": "text/plain; charset=utf-8"}.get(ext, "application/octet-stream")
    if mime.startswith("text/") and os.path.getsize(full) > 2_000_000:
        data = open(full, "rb").read()[-2_000_000:]
    else:
        data = open(full, "rb").read()
    hd = {"Content-Disposition": 'attachment; filename="%s"' % os.path.basename(full)} if "download" in req.q else None
    return Raw(data, mime, headers=hd)


@get("/api/report/sheet")
def r_report_sheet(req):
    full = _file(req)
    return xlsxio.read_xlsx(full, int(req.q.get("rows", 300)))


@get("/api/report/docx")
def r_report_docx(req):
    return {"text": docxgen.docx_text(_file(req))}


# ---------------------------------------------------------------- administration
@get("/api/users", role="admin")
def r_users(req):
    return core.AUTH.list_users()


@post("/api/users", role="admin")
def r_user_set(req):
    d = body(req)
    role = d.get("role") if d.get("role") in authz.ROLES else "operator"
    core.AUTH.set_password(str(d["user"]), str(d["password"]), role)
    audit(req.user, "user set", f"{d['user']} {role}")
    return core.AUTH.list_users()


@post("/api/users/delete", role="admin")
def r_user_delete(req):
    d = body(req)
    core.AUTH.delete_user(str(d["user"]))
    audit(req.user, "user delete", d["user"])
    return core.AUTH.list_users()


@get("/api/audit", role="admin")
def r_audit(req):
    with DBL:
        rows = DB.execute("SELECT t,user,action,detail FROM audit ORDER BY t DESC LIMIT 200").fetchall()
    return [{"t": r[0], "user": r[1], "action": r[2], "detail": r[3]} for r in rows]


@post("/api/gs/port", role="admin")
def r_gs_port(req):
    d = body(req)
    core.GS.set_port(d.get("port") if d.get("port") != "auto" else None)
    audit(req.user, "gs port", str(d.get("port")))


@post("/api/limits", role="admin")
def r_limits(req):
    lim = {k: {"lo": v.get("lo"), "hi": v.get("hi")} for k, v in body(req).items() if isinstance(v, dict)}
    set_setting("limits", lim)
    audit(req.user, "limits", json.dumps(lim))
    return lim


@post("/api/images/delete", role="admin")
def r_image_delete(req):
    return {"ok": IMAGES.delete(str(body(req)["id"]))}


H = httpx.make_handler(router, STATIC_DIRS, authenticate=authenticate, allows=allows, sse=httpx.subscribe_from(BUS), extra_headers=SECURITY)
