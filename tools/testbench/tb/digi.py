"""tb.digi"""

import json
import os
from tb.compat import saf_tool
from tb.links import OBC
from tb.util import OBC_INJECT, OBC_SAF, ROOT

# --------------------------------------------------------------------------------------
# digipeater / store and forward (CPU2 digipeater, CPU1 flash journal): injected frames, store listing, optional SDR check
# --------------------------------------------------------------------------------------
DIGI_REASONS = ["repeated", "disabled", "malformed", "not a UI frame", "no unused path entry", "next hop is not us / alias / WIDE2-N",
                "our own source", "loop (our call already used)", "denied station", "duplicate", "hop limit", "too long for the radio packet",
                "global rate limit", "same station too soon"]


def digi_identity():
    """Satellite call sign / SSID / alias as configured in the TUI (.s2s2.config), defaults if the file is missing."""
    c = {"sat_call": "9N2SJ", "sat_ssid": 1, "digi_alias": "APNDIG"}
    try:
        c.update({k: v for k, v in json.load(open(os.path.join(ROOT, ".s2s2.config"))).items() if k in c})
    except (OSError, ValueError):
        pass
    me = c["sat_call"] + ("-%d" % c["sat_ssid"] if c["sat_ssid"] else "")
    return me, c["digi_alias"] or c["sat_call"]


def ax25_text(fr):
    """KISS frame (destination, source, via..., control, PID, info; no flags / FCS) -> TNC2 text 'SRC>DEST,VIA*:info'."""
    def addr(b):
        call = "".join(chr(x >> 1) for x in b[:6]).strip()
        ss = (b[6] >> 1) & 15
        return call + ("-%d" % ss if ss else ""), bool(b[6] & 0x80)
    if len(fr) < 16:
        return None
    dest, _ = addr(fr[0:7])
    src, _ = addr(fr[7:14])
    vias, pos, last = [], 14, bool(fr[13] & 1)
    while not last and pos + 9 <= len(fr):
        v, h = addr(fr[pos:pos + 7])
        vias.append((v, h))
        last = bool(fr[pos + 6] & 1)
        pos += 7
    star = max([i for i, (_, h) in enumerate(vias) if h], default=-1)
    path = "".join("," + v + ("*" if i == star else "") for i, (v, _) in enumerate(vias))
    return f"{src}>{dest}{path}:" + bytes(fr[pos + 2:]).decode("latin-1")


def saf_read(abort=None):
    """Journal on CPU1: (status line, [records]); a record = dict(n, id, len, hops_left, fwd_count, ttl_s, text)."""
    with OBC.lock:
        OBC.flashdata.pop(0, None)
    mk = OBC.mark()
    OBC.send(OBC_SAF, 0)
    st = OBC.wait_ack(OBC_SAF, 0, mk, 5, abort)
    with OBC.lock:
        fd = OBC.flashdata.get(0)
    line = fd[1].decode(errors="replace") if (fd and st == 0) else None
    recs = []
    n = 1
    while line and n <= 16:
        with OBC.lock:
            OBC.flashdata.pop(n, None)
        mk = OBC.mark()
        OBC.send(OBC_SAF, n)
        st = OBC.wait_ack(OBC_SAF, n, mk, 5, abort)
        with OBC.lock:
            fd = OBC.flashdata.get(n)
        if st != 0 or not fd:
            break
        d = fd[1]
        recs.append({"n": n, "id": d[0] | d[1] << 8, "len": d[2], "hops_left": d[3], "fwd_count": d[4], "ttl_s": d[5] | d[6] << 8,
                     "text": ax25_text(d[7:]) if len(d) > 7 else ""})
        n += 1
    return line, recs


def digi_inject(text, abort=None, timeout=75):
    """Hand one frame (TNC2 text) to the satellite digipeater through CPU1. Returns (status, meaning); status 0 = repeated."""
    body = saf_tool.tnc2_to_body(text)
    mk = OBC.mark()
    OBC.send(OBC_INJECT, 0, 0, body)
    st = OBC.wait_ack(OBC_INJECT, 0, mk, timeout, abort)
    if st is None:
        return None, "no answer (the satellite answers only between beacons; the digipeater may be off)"
    if st == 0:
        return 0, "repeated"
    if 0x20 < st < 0x20 + len(DIGI_REASONS):
        return st, "not repeated: " + DIGI_REASONS[st - 0x20]
    return st, {0x07: "CPU2 did not answer within 50 s", 0x02: "bad frame"}.get(st, f"status 0x{st:02X}")
