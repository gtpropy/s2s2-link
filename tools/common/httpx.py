"""
httpx.py -- the small HTTP layer shared by Mission Control and the Testbench (standard library only).

  * Router with decorators:   @router.get("/api/x")   @router.post("/api/y", role="operator")   @router.get(r"^/api/images/(\\w+)/image$")
    A handler gets a Request (req.q query values, req.json(), req.body(), req.m regex match, req.user) and returns a dict / list (JSON),
    a Raw(...) for bytes, a (status, obj) pair, or None. ValueError / KeyError / RuntimeError become 400 {"error": ...}.
  * HTTP/1.1 keep-alive (one connection serves many requests), gzip for text >= 1 KB, ETag + 304 for static files, compact JSON.
  * Server-sent events with batching: events that arrive within 80 ms are sent as ONE message holding a JSON array (the browser parses
    and renders once instead of per event).
  * Optional authentication: authenticate(req) -> user or None; routes are public with public=True.
"""
import gzip
import json
import mimetypes
import os
import queue
import re
import time
import traceback
from http.server import BaseHTTPRequestHandler
from urllib.parse import parse_qs, urlparse

TEXT = ("text/", "application/json", "application/javascript", "image/svg+xml")
mimetypes.add_type("application/javascript", ".js")


class Raw:
    def __init__(self, body, ctype="application/octet-stream", status=200, headers=None):
        self.body, self.ctype, self.status, self.headers = body, ctype, status, headers or {}


class HTTPError(Exception):
    def __init__(self, status, msg):
        super().__init__(msg)
        self.status = status


class Request:
    def __init__(self, handler, method, url, m=None):
        self.h, self.method, self.m = handler, method, m
        u = urlparse(url)
        self.path = u.path
        self.query = u.query
        self.q = {k: v[0] for k, v in parse_qs(u.query, keep_blank_values=True).items()}
        self.headers = handler.headers
        self.user = None
        self.token = None
        self._body = None

    def body(self):
        if self._body is None:
            n = int(self.headers.get("Content-Length", 0))
            if n > 64 * 1024 * 1024:
                raise ValueError("body too large")
            self._body = self.h.rfile.read(n) if n else b""
        return self._body

    def json(self):
        b = self.body()
        return json.loads(b) if b else {}

    def cookie(self, name):
        for part in (self.headers.get("Cookie") or "").split(";"):
            k, _, v = part.strip().partition("=")
            if k == name:
                return v
        return None


class Router:
    def __init__(self):
        self.routes = {"GET": [], "POST": []}

    def _add(self, method, pat, public, role):
        def deco(fn):
            rx = re.compile(pat) if pat.startswith("^") else None
            self.routes[method].append((rx, pat, fn, public, role))
            return fn
        return deco

    def get(self, pat, public=False, role=None):
        return self._add("GET", pat, public, role)

    def post(self, pat, public=False, role=None):
        return self._add("POST", pat, public, role)

    def find(self, method, path):
        for rx, pat, fn, public, role in self.routes.get(method, ()):
            if rx is None:
                if pat == path:
                    return fn, None, public, role
            else:
                m = rx.match(path)
                if m:
                    return fn, m, public, role
        return None


def make_handler(router, static_dirs=(), authenticate=None, allows=None, on_error=None, sse=None, extra_headers=None):
    """static_dirs: [(url prefix or '', directory)] searched in order; sse: (path, subscribe, unsubscribe)."""
    cache = {}                                                                      # (path, mtime, size) -> (body, gz)

    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"
        server_version = "S2S2"

        def log_message(self, *a):
            pass

        # ---------------------------------------------------------------- output
        def _send(self, status, body, ctype="application/json", headers=None, cacheable=False, etag=None):
            if isinstance(body, str):
                body = body.encode()
            hd = {"Content-Type": ctype, "Cache-Control": "no-cache" if cacheable else "no-store", "X-Content-Type-Options": "nosniff"}
            hd.update(extra_headers or {})
            hd.update(headers or {})
            if etag:
                hd["ETag"] = etag
                if self.headers.get("If-None-Match") == etag:
                    self.send_response(304)
                    for k in ("ETag", "Cache-Control"):
                        self.send_header(k, hd[k])
                    self.end_headers()
                    return
            if len(body) >= 1024 and ctype.startswith(TEXT) and "gzip" in (self.headers.get("Accept-Encoding") or ""):
                body = gzip.compress(body, 5)
                hd["Content-Encoding"] = "gzip"
            hd["Content-Length"] = str(len(body))
            hd["Vary"] = "Accept-Encoding"
            self.send_response(status)
            for k, v in hd.items():
                self.send_header(k, v)
            self.end_headers()
            if self.command != "HEAD":
                self.wfile.write(body)

        def _json(self, status, obj):
            self._send(status, json.dumps(obj, default=str, separators=(",", ":")))

        # ---------------------------------------------------------------- static
        def _static(self, path):
            rel = "index.html" if path in ("/", "") else path.lstrip("/")
            for prefix, d in static_dirs:
                pre = prefix.strip("/")
                if pre and not rel.startswith(pre + "/"):
                    continue
                sub = rel[len(pre) + 1:] if pre else rel
                f = os.path.abspath(os.path.join(d, sub))
                if f.startswith(os.path.abspath(d) + os.sep) and os.path.isfile(f):
                    st = os.stat(f)
                    key = (f, st.st_mtime_ns, st.st_size)
                    if key not in cache:
                        cache.clear() if len(cache) > 200 else None
                        cache[key] = open(f, "rb").read()
                    ct = mimetypes.guess_type(f)[0] or "application/octet-stream"
                    self._send(200, cache[key], ct, cacheable=True, etag='"%x-%x"' % (st.st_mtime_ns, st.st_size),
                               headers={"Cache-Control": "public, max-age=86400"} if "/vendor/" in "/" + rel else None)
                    return True
            return False

        # ---------------------------------------------------------------- events
        def _stream(self, req):
            _, subscribe, unsubscribe = sse
            q = subscribe()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Connection", "close")
            self.end_headers()
            self.close_connection = True
            try:
                while True:
                    try:
                        batch = [q.get(timeout=15)]
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                        self.wfile.flush()
                        continue
                    time.sleep(0.08)                                    # let a burst arrive, then send it as one message
                    while len(batch) < 500:
                        try:
                            batch.append(q.get_nowait())
                        except queue.Empty:
                            break
                    self.wfile.write(b"data: " + json.dumps(batch, default=str, separators=(",", ":")).encode() + b"\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                unsubscribe(q)

        # ---------------------------------------------------------------- dispatch
        def _handle(self, method):
            req = Request(self, method, self.path)
            try:
                self._dispatch(req, method)
            finally:
                if method == "POST" and req._body is None:               # an early answer must still consume the body (keep-alive)
                    try:
                        req.body()
                    except Exception:
                        self.close_connection = True

        def _dispatch(self, req, method):
            try:
                if authenticate:
                    req.user, req.token = authenticate(req)
                if sse and method == "GET" and req.path == sse[0]:
                    if authenticate and not req.user:
                        return self._json(401, {"error": "login required"})
                    return self._stream(req)
                hit = router.find(method, req.path)
                if hit:
                    fn, m, public, role = hit
                    req.m = m
                    if authenticate and not public and not req.user:
                        return self._json(401, {"error": "login required"})
                    if role and allows and not allows(req.user, role):
                        return self._json(403, {"error": f"needs role {role}"})
                    res = fn(req)
                    if isinstance(res, Raw):
                        return self._send(res.status, res.body, res.ctype, res.headers)
                    if isinstance(res, tuple):
                        return self._json(res[0], res[1])
                    return self._json(200, {"ok": True} if res is None else res)
                if method == "GET" and self._static(req.path):
                    return
                return self._json(404, {"error": "not found"})
            except HTTPError as e:
                return self._json(e.status, {"error": str(e)})
            except (ValueError, KeyError, RuntimeError, TypeError, json.JSONDecodeError) as e:
                if on_error:
                    on_error(req.path, e)
                return self._json(400, {"error": str(e) if not isinstance(e, KeyError) else f"missing field {e}"})
            except Exception as e:                                       # a bug: report it, keep the connection alive
                traceback.print_exc()
                if on_error:
                    on_error(req.path, e)
                return self._json(500, {"error": f"{type(e).__name__}: {e}"})

        def do_GET(self):
            self._handle("GET")

        def do_HEAD(self):
            self._handle("GET")

        def do_POST(self):
            self._handle("POST")

    return H


def subscribe_from(bus):
    return ("/events", bus.subscribe, bus.unsubscribe)
