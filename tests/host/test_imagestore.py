#!/usr/bin/env python3
"""imagestore.py: fragments in any order with duplicates and repeats rebuild the picture byte for byte, in the background, on disk."""
import hashlib, os, random, shutil, sys, tempfile, time
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "common"))
import imagestore as I

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "tools", "testbench", "samples")
events = []
def wait(pred, t=5):
    end = time.time() + t
    while time.time() < end:
        if pred(): return True
        time.sleep(0.02)
    return False

d = tempfile.mkdtemp()
try:
    for name in ("gs_downloaded_photo.jpg", "antarikshya_logo.png"):
        src = open(os.path.join(ROOT, name), "rb").read()
        total = (len(src) + 127) // 128
        frags = [src[i * 128:(i + 1) * 128].ljust(128, b"\0") for i in range(total)]
        st = I.ImageStore(os.path.join(d, name), lambda k, **kw: events.append((k, kw)))
        st.expect(hashlib.sha256(src).hexdigest(), len(src))
        order = list(range(total)); random.Random(3).shuffle(order)
        half = order[: total // 2]
        for i in half: st.ingest("sdr", i, total, frags[i])
        assert wait(lambda: (st.list() or [{"got": 0}])[0]["got"] == len(half)), "partial"
        sid = st.live_id()
        part, mime = st.image(sid)
        assert len(part) == total * 128 and not st.get(sid)["complete"]            # partial picture is available while receiving
        for i in half[:20]: st.ingest("sdr", i, total, frags[i])                    # duplicates
        for i in order[total // 2:]: st.ingest("gs", i, total, frags[i])
        assert wait(lambda: st.get(sid)["complete"]), "complete"
        s = st.get(sid)
        data, mime = st.image(sid)
        assert data == src, "rebuilt picture differs"
        assert s["sha256"] == hashlib.sha256(src).hexdigest() and s["matches_sent"] is True, s
        assert s["sources"].get("sdr") == len(half) and s["sources"].get("gs") == total - len(half), s["sources"]
        assert s["dups"] >= 20
        n_before = len(st.list())
        for i in range(total): st.ingest("gs", i, total, frags[i])                   # the satellite repeats the picture: no new session
        time.sleep(0.5)
        assert len(st.list()) == n_before, "repeat of a finished picture made a new session"
        other = [bytes([i % 251]) * 128 for i in range(total)]                      # a different picture of the same size: new session
        for i in range(total): st.ingest("gs", i, total, other[i])
        assert wait(lambda: len(st.list()) == n_before + 1 and st.list()[0]["complete"])
        st.stop_ev.set()
        st2 = I.ImageStore(os.path.join(d, name), lambda k, **kw: None)             # restart: the gallery is still there
        assert len(st2.list()) == n_before + 1 and st2.image(sid)[0] == src
        st2.stop_ev.set()
        print("imagestore OK:", name, "%d B, %d fragments, %s" % (len(src), total, mime))
finally:
    shutil.rmtree(d, ignore_errors=True)
