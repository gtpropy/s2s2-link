"""
imagestore.py -- stores camera fragments the moment they arrive and rebuilds the picture in the background.

Every receiver feeds this store, whether or not a test is running: the ground station's UART lines (JSON "CAMERA" records) and the
SDR + Direwolf monitor (Packet Format sheet fragments, type 0x08). Each fragment is written to disk at once; a worker thread keeps
the rebuilt picture up to date (missing fragments are zero filled so a partly received JPEG already shows), and when the last
fragment is in it finishes the picture: trims it, hashes it, compares the hash with what the sender announced, and keeps it in a
gallery that survives a restart.

Layout (data/testbench/images/<session>/):
    meta.json        total, fragment count, sources, times, sha256, kind, complete, matches_sent
    frag_00001.bin   128 bytes of fragment 1 (as received, the last one zero padded by the satellite)
    image.<ext>      the rebuilt picture (rewritten while fragments arrive, final when complete)
"""
import hashlib
import json
import os
import queue
import threading
import time

FRAG_BYTES = 128


def sniff(data):
    """(extension, mime) from the first bytes."""
    if data[:3] == b"\xff\xd8\xff":
        return "jpg", "image/jpeg"
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png", "image/png"
    if data[:4] == b"GIF8":
        return "gif", "image/gif"
    if data[:2] == b"BM":
        return "bmp", "image/bmp"
    return "bin", "application/octet-stream"


def trim(data):
    """Cut the zero padding of the last fragment: after the JPEG end marker / PNG IEND, else trailing zeros."""
    if data[:3] == b"\xff\xd8\xff":
        i = data.rfind(b"\xff\xd9")
        if i >= 0:
            return data[:i + 2]
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        i = data.rfind(b"IEND")
        if i >= 0:
            return data[:i + 8]
    return data.rstrip(b"\x00")


class Session:
    def __init__(self, sid, total, directory):
        self.id = sid
        self.total = total
        self.dir = directory
        self.frags = {}                    # idx (0 based) -> bytes
        self.sources = {}                  # source name -> number of fragments it delivered first
        self.dups = 0
        self.t_first = self.t_last = time.time()
        self.complete = False
        self.sha256 = None
        self.size = 0
        self.kind = "bin"
        self.matches_sent = None
        self.dirty = True
        self.t_done = None

    def summary(self):
        return {"id": self.id, "total": self.total, "got": len(self.frags), "complete": self.complete, "sha256": self.sha256,
                "size": self.size, "kind": self.kind, "sources": self.sources, "dups": self.dups,
                "t_first": round(self.t_first, 2), "t_last": round(self.t_last, 2),
                "seconds": round((self.t_done or self.t_last) - self.t_first, 1),
                "matches_sent": self.matches_sent,
                "missing": [i + 1 for i in range(self.total) if i not in self.frags][:2000]}


class ImageStore:
    def __init__(self, root, publish):
        self.root = root
        self.publish = publish
        self.lock = threading.Lock()
        self.sessions = {}
        self.order = []                    # session ids, oldest first
        self.expected = None               # (sha256, size) the sender announced
        self.q = queue.Queue()
        self.stop_ev = threading.Event()
        os.makedirs(root, exist_ok=True)
        self._load()
        self.worker = threading.Thread(target=self._run, daemon=True)
        self.worker.start()

    # ------------------------------------------------------------------ input
    def expect(self, sha256, size):
        """The picture that is about to be sent: the next completed picture is compared with this hash."""
        self.expected = (sha256, size)

    def ingest(self, src, idx, total, data):
        """idx is 0 based. Cheap and thread safe: the work happens in the background thread."""
        if total <= 0 or total > 4096 or idx < 0 or idx >= total or not data:
            return
        self.q.put((time.time(), src, idx, total, bytes(data[:FRAG_BYTES])))

    # ------------------------------------------------------------------ queries
    def list(self):
        with self.lock:
            return [self.sessions[i].summary() for i in reversed(self.order)]

    def live_id(self):
        with self.lock:
            return self.order[-1] if self.order else None

    def get(self, sid):
        with self.lock:
            s = self.sessions.get(sid)
            return s.summary() if s else None

    def image(self, sid):
        """(bytes, mime) of the rebuilt picture; a partial one while fragments are still coming."""
        with self.lock:
            s = self.sessions.get(sid)
            if not s:
                return None
            data = self._assemble(s, final=s.complete)
        return data, sniff(data)[1]

    def fragment(self, sid, idx):
        with self.lock:
            s = self.sessions.get(sid)
            return s.frags.get(idx) if s else None

    def delete(self, sid):
        with self.lock:
            s = self.sessions.pop(sid, None)
            if sid in self.order:
                self.order.remove(sid)
        if s:
            for f in os.listdir(s.dir):
                os.remove(os.path.join(s.dir, f))
            os.rmdir(s.dir)
        return bool(s)

    # ------------------------------------------------------------------ worker
    def _assemble(self, s, final):
        buf = bytearray(s.total * FRAG_BYTES)
        for i, d in s.frags.items():
            buf[i * FRAG_BYTES:i * FRAG_BYTES + len(d)] = d
        data = bytes(buf)
        return trim(data) if final else data

    def _new_session(self, total):
        sid = time.strftime("%Y%m%d_%H%M%S")
        n = 0
        while sid in self.sessions:
            n += 1
            sid = time.strftime("%Y%m%d_%H%M%S") + "_%d" % n
        d = os.path.join(self.root, sid)
        os.makedirs(d, exist_ok=True)
        s = Session(sid, total, d)
        self.sessions[sid] = s
        self.order.append(sid)
        return s

    def _pick(self, idx, total, data):
        """The session this fragment belongs to (None = a duplicate of a finished picture)."""
        cur = self.sessions[self.order[-1]] if self.order else None
        if cur and cur.total == total:
            if not cur.complete:
                return cur
            if cur.frags.get(idx) == data:
                return None                          # the satellite is repeating a picture we already have
        return self._new_session(total)

    def _run(self):
        last_pub = 0.0
        while not self.stop_ev.is_set():
            try:
                t, src, idx, total, data = self.q.get(timeout=0.5)
            except queue.Empty:
                self._flush_dirty()
                continue
            with self.lock:
                s = self._pick(idx, total, data)
                if s is None:
                    continue
                if idx in s.frags:
                    s.dups += 1
                else:
                    s.frags[idx] = data
                    s.sources[src] = s.sources.get(src, 0) + 1
                    s.dirty = True
                    with open(os.path.join(s.dir, "frag_%05d.bin" % (idx + 1)), "wb") as f:
                        f.write(data)
                s.t_last = t
                just_done = False
                if not s.complete and len(s.frags) == s.total:
                    s.complete = True
                    s.t_done = t
                    just_done = True
                summ = s.summary()
            if just_done:
                self._finish(s)
                summ = self.get(s.id)
                self.publish("image", done=True, **summ)
            elif time.time() - last_pub > 0.2 or len(summ) == 0:
                last_pub = time.time()
                self.publish("image", done=False, **summ)
            if self.q.empty():
                self._flush_dirty()

    def _flush_dirty(self):
        with self.lock:
            todo = [s for s in self.sessions.values() if s.dirty and not s.complete]
        for s in todo:
            self._write_image(s, final=False)

    def _write_image(self, s, final):
        with self.lock:
            data = self._assemble(s, final)
            s.dirty = False
            ext = sniff(data)[0]
            s.kind = ext
            s.size = len(data)
            meta = s.summary()
        for f in os.listdir(s.dir):
            if f.startswith("image."):
                os.remove(os.path.join(s.dir, f))
        with open(os.path.join(s.dir, "image." + ext), "wb") as f:
            f.write(data)
        with open(os.path.join(s.dir, "meta.json"), "w") as f:
            json.dump(meta, f)

    def _finish(self, s):
        with self.lock:
            data = trim(self._assemble(s, False))
            s.sha256 = hashlib.sha256(data).hexdigest()
            if self.expected:
                s.matches_sent = (s.sha256 == self.expected[0])
        self._write_image(s, final=True)

    def _load(self):
        for sid in sorted(os.listdir(self.root)):
            d = os.path.join(self.root, sid)
            try:
                meta = json.load(open(os.path.join(d, "meta.json")))
            except (OSError, ValueError):
                continue
            s = Session(sid, meta["total"], d)
            for f in os.listdir(d):
                if f.startswith("frag_") and f.endswith(".bin"):
                    s.frags[int(f[5:10]) - 1] = open(os.path.join(d, f), "rb").read()
            s.complete = bool(meta.get("complete")) and len(s.frags) == s.total
            s.sha256, s.size, s.kind = meta.get("sha256"), meta.get("size", 0), meta.get("kind", "bin")
            s.sources, s.dups = meta.get("sources", {}), meta.get("dups", 0)
            s.t_first, s.t_last = meta.get("t_first", time.time()), meta.get("t_last", time.time())
            s.matches_sent = meta.get("matches_sent")
            s.t_done = s.t_last if s.complete else None
            s.dirty = False
            self.sessions[sid] = s
            self.order.append(sid)
