"""mc.web -- HTTP routes of Mission Control (tools/common/httpx.py provides the server machinery)."""
import json
import os
import time

import authz
import httpx
import imagestore
from httpx import Raw
from mc import core
from mc.core import BUS, CATALOG, CUR, DBL, DB, EVENTS, HERE, IMAGES, JOBS, ROOT, audit, csv_export, history_jobs, job_update, now, series, set_setting, snapshot, submit

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
    return EVENTS.history(req.q.get("kind"), int(req.q.get("limit", 300)))


@get("/api/export")
def r_export(req):
    k = req.q.get("kind", "hk")
    if k not in ("hk", "beacon", "ack", "nack", "cw"):
        raise ValueError("bad kind")
    return Raw(csv_export(k).encode(), "text/csv", headers={"Content-Disposition": f'attachment; filename="s2s2_{k}.csv"'})


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
    return [{**c, "allowed": core.AUTH.allows(req.user, c["role"])} for c in CATALOG]


@get("/api/commands")
def r_commands(req):
    return history_jobs(int(req.q.get("n", 100)))


@post("/api/command")
def r_command(req):
    d = body(req)
    c = next((x for x in CATALOG if x["id"] == d.get("id")), None)
    if not c:
        raise ValueError("unknown command")
    if not core.AUTH.allows(req.user, c["role"]):
        return (403, {"error": f"needs role {c['role']}"})
    if c.get("critical") and not core.AUTH.verify(req.user, str(d.get("password", ""))):
        audit(req.user, "critical command refused", c["id"])
        return (403, {"error": "password confirmation failed"})
    jid = submit(req.user, c["id"], d.get("args", {}))
    audit(req.user, "command", f"{c['id']} -> {JOBS[jid]['line']}")
    return {"job": jid}


@post("/api/command/cancel", role="operator")
def r_cancel(req):
    jid = int(body(req)["id"])
    if CUR["id"] == jid:
        CUR["abort"].set()
    elif jid in JOBS and JOBS[jid]["state"] == "queued":
        job_update(jid, state="cancelled", t_end=now())
    audit(req.user, "cancel", str(jid))


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
