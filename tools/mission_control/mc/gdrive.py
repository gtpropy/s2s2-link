"""mc.gdrive -- Google Drive backup of the operational report folders (Drive REST API v3, standard library only).

Two ways to authorise (settings -> Google Drive):
  * "oauth"           a Desktop OAuth client (client id + secret) and one consent in the browser; the refresh token is stored. Uses the signed-in person's own Drive space.
  * "service_account" a service-account key file; the target must be a Shared Drive folder (a service account has no storage space of its own in My Drive).
Day folders are mirrored as <S2S_operational_reports>/<YYYY-MM-DD, HH_MMAM>/... (the root folder is found or created by that name; a folder id in the settings overrides it); only new or changed files are uploaded. Past days are marked "uploaded" when a full sync of that day had no error;
only then does the retention job delete the local copy. Pulling a day downloads that folder tree again.
Nothing here runs until Drive is configured; the API endpoints can be redirected to a test server through settings["api"] / ["token_uri"] / ["auth_uri"].
"""
import base64
import json
import mimetypes
import os
import subprocess
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid

from mc import core

API = "https://www.googleapis.com"
AUTH_URI = "https://accounts.google.com/o/oauth2/v2/auth"
TOKEN_URI = "https://oauth2.googleapis.com/token"
SCOPE = "https://www.googleapis.com/auth/drive"
FOLDER = "application/vnd.google-apps.folder"
ROOT_NAME = "S2S_operational_reports"
_tok = {"v": None, "exp": 0.0}
_lock = threading.RLock()
_state = {"last": 0.0, "error": "", "busy": False, "stats": None, "stats_t": 0.0, "uploaded_files": 0}


def cfg():
    return core.setting("gdrive", {}) or {}


def configured():
    c = cfg()
    if not c.get("enabled"):
        return False
    if c.get("mode") == "service_account":
        return os.path.isfile(c.get("sa_file", ""))
    return bool(c.get("refresh_token") and c.get("client_id"))


def _b64(b):
    return base64.urlsafe_b64encode(b).rstrip(b"=").decode()


def _http(method, url, headers=None, body=None, timeout=60):
    req = urllib.request.Request(url, data=body, method=method, headers=headers or {})
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def _token():
    c = cfg()
    with _lock:
        if _tok["v"] and time.time() < _tok["exp"] - 60:
            return _tok["v"]
        uri = c.get("token_uri") or TOKEN_URI
        if c.get("mode") == "service_account":
            sa = json.load(open(c["sa_file"]))
            now = int(time.time())
            head = _b64(json.dumps({"alg": "RS256", "typ": "JWT"}).encode())
            claim = _b64(json.dumps({"iss": sa["client_email"], "scope": SCOPE, "aud": sa.get("token_uri", uri), "iat": now, "exp": now + 3000}).encode())
            with tempfile.NamedTemporaryFile("w", suffix=".pem", delete=False) as f:
                f.write(sa["private_key"])
                keyp = f.name
            try:
                os.chmod(keyp, 0o600)
                sig = subprocess.run(["openssl", "dgst", "-sha256", "-sign", keyp], input=(head + "." + claim).encode(), capture_output=True, timeout=20)
            finally:
                os.unlink(keyp)
            if sig.returncode != 0:
                raise RuntimeError("signing the service-account token failed (is openssl installed?)")
            data = urllib.parse.urlencode({"grant_type": "urn:ietf:params:oauth:grant-type:jwt-bearer", "assertion": head + "." + claim + "." + _b64(sig.stdout)}).encode()
            st, body = _http("POST", sa.get("token_uri", uri), {"Content-Type": "application/x-www-form-urlencoded"}, data)
        else:
            data = urllib.parse.urlencode({"grant_type": "refresh_token", "refresh_token": c["refresh_token"], "client_id": c["client_id"], "client_secret": c.get("client_secret", "")}).encode()
            st, body = _http("POST", uri, {"Content-Type": "application/x-www-form-urlencoded"}, data)
        j = json.loads(body or b"{}")
        if st != 200 or "access_token" not in j:
            raise RuntimeError("Google token request failed (%s): %s" % (st, j.get("error_description") or j.get("error") or body[:120]))
        _tok["v"], _tok["exp"] = j["access_token"], time.time() + int(j.get("expires_in", 3000))
        return _tok["v"]


def _call(method, path, params=None, body=None, headers=None, upload=False, raw=False, timeout=90):
    c = cfg()
    base = (c.get("api") or API).rstrip("/")
    url = base + ("/upload" if upload else "") + path
    if params:
        url += "?" + urllib.parse.urlencode(params)
    h = {"Authorization": "Bearer " + _token(), **(headers or {})}
    st, data = _http(method, url, h, body, timeout)
    if st == 401:                                              # token expired early: once more with a fresh one
        _tok["v"] = None
        h["Authorization"] = "Bearer " + _token()
        st, data = _http(method, url, h, body, timeout)
    if st >= 300:
        try:
            msg = json.loads(data).get("error", {}).get("message", "")
        except ValueError:
            msg = data[:120].decode("utf-8", "replace")
        raise RuntimeError("Drive %s %s: %s %s" % (method, path, st, msg))
    return data if raw else (json.loads(data) if data else {})


def _q(s):
    return s.replace("\\", "\\\\").replace("'", "\\'")


def find(name, parent, folder=None):
    q = "name = '%s' and '%s' in parents and trashed = false" % (_q(name), parent)
    if folder is not None:
        q += " and mimeType %s '%s'" % ("=" if folder else "!=", FOLDER)
    r = _call("GET", "/drive/v3/files", {"q": q, "fields": "files(id,name,size,md5Checksum,modifiedTime)", "pageSize": 10, "supportsAllDrives": "true", "includeItemsFromAllDrives": "true"})
    return (r.get("files") or [None])[0]


def ensure_folder(name, parent):
    f = find(name, parent, True)
    if f:
        return f["id"]
    r = _call("POST", "/drive/v3/files", {"supportsAllDrives": "true", "fields": "id"}, json.dumps({"name": name, "mimeType": FOLDER, "parents": [parent]}).encode(), {"Content-Type": "application/json"})
    return r["id"]


def upload_file(path, parent, existing_id=None):
    meta = {"name": os.path.basename(path)} if existing_id else {"name": os.path.basename(path), "parents": [parent]}
    mime = mimetypes.guess_type(path)[0] or "application/octet-stream"
    bd = "s2s" + uuid.uuid4().hex
    body = (("--%s\r\nContent-Type: application/json; charset=UTF-8\r\n\r\n%s\r\n--%s\r\nContent-Type: %s\r\n\r\n" % (bd, json.dumps(meta), bd, mime)).encode()
            + open(path, "rb").read() + ("\r\n--%s--" % bd).encode())
    hdr = {"Content-Type": "multipart/related; boundary=" + bd}
    p = {"uploadType": "multipart", "supportsAllDrives": "true", "fields": "id"}
    if existing_id:
        return _call("PATCH", "/drive/v3/files/" + existing_id, p, body, hdr, upload=True, timeout=300)["id"]
    return _call("POST", "/drive/v3/files", p, body, hdr, upload=True, timeout=300)["id"]


def _files_table():
    with core.DBL:
        core.DB.execute("CREATE TABLE IF NOT EXISTS drive_files (path TEXT PRIMARY KEY, id TEXT, size INTEGER, mtime REAL)")
        core.DB.commit()


def root_id():
    """The Drive folder that receives the reports: the configured id, else a folder named S2S_operational_reports (found or created in My Drive)."""
    c = cfg()
    if c.get("folder_id"):
        return c["folder_id"]
    if c.get("root_id"):
        return c["root_id"]
    rid = ensure_folder(ROOT_NAME, "root")
    c["root_id"] = rid
    core.set_setting("gdrive", c)
    return rid


def sync_day(name, local_path, finalized=False):
    """Mirror one local day folder to Drive. Returns {"uploaded": n, "skipped": n, "errors": [..]}."""
    _files_table()
    res = {"uploaded": 0, "skipped": 0, "errors": []}
    did = ensure_folder(name, root_id())
    ids = {"": did}
    for dirpath, dirs, files in os.walk(local_path):
        dirs.sort()
        rel = os.path.relpath(dirpath, local_path)
        rel = "" if rel == "." else rel
        for dname in dirs:
            r2 = (rel + "/" if rel else "") + dname
            try:
                ids[r2] = ensure_folder(dname, ids[rel])
            except RuntimeError as e:
                res["errors"].append(str(e))
        for f in sorted(files):
            if f.startswith(".") or f.endswith(".tmp"):
                continue
            p = os.path.join(dirpath, f)
            key = name + "/" + ((rel + "/") if rel else "") + f
            try:
                st = os.stat(p)
                with core.DBL:
                    row = core.DB.execute("SELECT id,size,mtime FROM drive_files WHERE path=?", (key,)).fetchone()
                if row and row[1] == st.st_size and abs(row[2] - st.st_mtime) < 1:
                    res["skipped"] += 1
                    continue
                if rel not in ids:
                    continue
                fid = upload_file(p, ids[rel], row[0] if row else None)
                with core.DBL:
                    core.DB.execute("INSERT OR REPLACE INTO drive_files VALUES (?,?,?,?)", (key, fid, st.st_size, st.st_mtime))
                    core.DB.commit()
                res["uploaded"] += 1
            except (RuntimeError, OSError) as e:
                res["errors"].append("%s: %s" % (key, e))
    if finalized and not res["errors"]:
        up = core.setting("drive_uploaded", {}) or {}
        up[name] = time.time()
        core.set_setting("drive_uploaded", up)
    return res


def sync_all(force_today=True):
    """Upload every local day folder that still has something to upload (today's folder is uploaded too, but only past days are marked finalized)."""
    from mc import store
    if not configured():
        return {"configured": False}
    with _lock:
        if _state["busy"]:
            return {"busy": True}
        _state["busy"] = True
    out = {"uploaded": 0, "errors": []}
    try:
        cur = (core.setting("store_day", {}) or {}).get("name")
        store.flush(force=True)
        for d in store.list_days():
            if d["uploaded"] and d["name"] != cur:
                continue
            r = sync_day(d["name"], os.path.join(store.REPORTS, d["name"]), finalized=d["name"] != cur)
            out["uploaded"] += r["uploaded"]
            out["errors"] += r["errors"]
        _state["error"] = "; ".join(out["errors"][:2])
        _state["last"] = time.time()
        _state["uploaded_files"] += out["uploaded"]
    except Exception as e:
        _state["error"] = str(e)
        out["errors"].append(str(e))
    finally:
        _state["busy"] = False
    return out


def _walk_size(folder_id, depth=0):
    tot = n = 0
    token = None
    while True:
        p = {"q": "'%s' in parents and trashed = false" % folder_id, "fields": "nextPageToken,files(id,mimeType,size)", "pageSize": 1000, "supportsAllDrives": "true", "includeItemsFromAllDrives": "true"}
        if token:
            p["pageToken"] = token
        r = _call("GET", "/drive/v3/files", p)
        for f in r.get("files", []):
            if f["mimeType"] == FOLDER and depth < 6:
                a, b = _walk_size(f["id"], depth + 1)
                tot += a
                n += b
            else:
                tot += int(f.get("size") or 0)
                n += 1
        token = r.get("nextPageToken")
        if not token:
            return tot, n


def stats(max_age=300):
    """Live Drive numbers: account quota (used / limit), size and file count of the report folder, number of day folders."""
    if not configured():
        return {"configured": False, "reason": "Google Drive is not configured"}
    if _state["stats"] and time.time() - _state["stats_t"] < max_age:
        return _state["stats"]
    try:
        about = _call("GET", "/drive/v3/about", {"fields": "storageQuota,user"})
        q = about.get("storageQuota", {})
        b, n = _walk_size(root_id())
        s = {"configured": True, "user": (about.get("user") or {}).get("emailAddress", ""), "used": int(q.get("usage") or 0), "limit": int(q["limit"]) if q.get("limit") else None,
             "folder_bytes": b, "folder_files": n, "last_upload": _state["last"] or None, "error": _state["error"], "busy": _state["busy"]}
        _state.update(stats=s, stats_t=time.time())
        return s
    except Exception as e:
        return {"configured": True, "error": str(e), "last_upload": _state["last"] or None}


def list_days():
    """Day folders on Drive: [{name, id}]"""
    if not configured():
        return []
    return sorted([{"name": d["name"], "id": d["id"]} for d in _list_children(root_id(), True)], key=lambda x: x["name"], reverse=True)


def _list_children(folder_id, folders_only=False):
    out, token = [], None
    while True:
        q = "'%s' in parents and trashed = false" % folder_id
        if folders_only:
            q += " and mimeType = '%s'" % FOLDER
        p = {"q": q, "fields": "nextPageToken,files(id,name,mimeType,size)", "pageSize": 1000, "supportsAllDrives": "true", "includeItemsFromAllDrives": "true"}
        if token:
            p["pageToken"] = token
        r = _call("GET", "/drive/v3/files", p)
        out += r.get("files", [])
        token = r.get("nextPageToken")
        if not token:
            return out


def download_day(name, dest_root):
    """Download <S2S_operational_reports>/<name> from Drive into dest_root/<name>."""
    d = find(name, root_id(), True)
    if not d:
        raise RuntimeError("that day is not on Google Drive")
    dest = os.path.join(dest_root, name)
    os.makedirs(dest, exist_ok=True)
    count = [0]

    def rec(fid, path):
        for f in _list_children(fid):
            p = os.path.join(path, f["name"].replace("/", "_"))
            if f["mimeType"] == FOLDER:
                os.makedirs(p, exist_ok=True)
                rec(f["id"], p)
            else:
                data = _call("GET", "/drive/v3/files/" + f["id"], {"alt": "media", "supportsAllDrives": "true"}, raw=True, timeout=300)
                open(p, "wb").write(data)
                count[0] += 1
    rec(d["id"], dest)
    return {"path": dest, "files": count[0]}


# ---- OAuth (Desktop client): consent in the browser, redirect back to Mission Control, store the refresh token
def oauth_url(redirect):
    c = cfg()
    q = urllib.parse.urlencode({"client_id": c["client_id"], "redirect_uri": redirect, "response_type": "code", "scope": SCOPE, "access_type": "offline", "prompt": "consent"})
    return (c.get("auth_uri") or AUTH_URI) + "?" + q


def oauth_exchange(code, redirect):
    c = cfg()
    data = urllib.parse.urlencode({"grant_type": "authorization_code", "code": code, "client_id": c["client_id"], "client_secret": c.get("client_secret", ""), "redirect_uri": redirect}).encode()
    st, body = _http("POST", c.get("token_uri") or TOKEN_URI, {"Content-Type": "application/x-www-form-urlencoded"}, data)
    j = json.loads(body or b"{}")
    if st != 200 or "refresh_token" not in j:
        raise RuntimeError("Google did not return a refresh token (%s): %s" % (st, j.get("error_description") or j.get("error") or ""))
    c["refresh_token"] = j["refresh_token"]
    core.set_setting("gdrive", c)
    _tok["v"] = None
    return True


def save_config(new):
    c = cfg()
    if "folder_id" in new and (new["folder_id"] or "") != c.get("folder_id", ""):
        c.pop("root_id", None)
        c["folder_id"] = (new["folder_id"] or "").strip()
    for k in ("enabled", "mode", "client_id", "client_secret", "api", "token_uri", "auth_uri"):
        if k in new and new[k] not in (None, ""):
            c[k] = new[k]
    if "enabled" in new:
        c["enabled"] = bool(new["enabled"])
    if new.get("sa_json"):
        j = json.loads(new["sa_json"]) if isinstance(new["sa_json"], str) else new["sa_json"]
        if "client_email" not in j or "private_key" not in j:
            raise ValueError("that is not a service-account key file")
        p = os.path.join(core.DATA, "gdrive_service_account.json")
        with open(p, "w") as f:
            json.dump(j, f)
        os.chmod(p, 0o600)
        c["sa_file"] = p
        c["mode"] = "service_account"
    core.set_setting("gdrive", c)
    _tok["v"] = None
    _state["stats"] = None
    return public_config()


def public_config():
    c = cfg()
    return {"enabled": bool(c.get("enabled")), "mode": c.get("mode", "oauth"), "folder_id": c.get("folder_id", ""), "root_name": ROOT_NAME, "client_id": c.get("client_id", ""), "has_secret": bool(c.get("client_secret")),
            "authorised": bool(c.get("refresh_token")) or os.path.isfile(c.get("sa_file", "")), "configured": configured(), "last_upload": _state["last"] or None, "error": _state["error"]}


def _loop():
    while True:
        time.sleep(300)
        try:
            if configured():
                sync_all()
        except Exception as e:
            _state["error"] = str(e)


def start():
    threading.Thread(target=_loop, daemon=True).start()
