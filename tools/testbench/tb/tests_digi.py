"""tb.tests_digi"""

import time
from tb.bus import BUS, log
from tb.devices import DwCapture, SDR
from tb.digi import DIGI_REASONS, digi_identity, digi_inject, saf_read
from tb.links import OBC
from tb.state import set_result
from tb.util import now

def test_digi(cfg, abort):
    """Digipeater + store-and-forward test through the VCP, no ground station needed. Injects frames into the satellite's
    digipeater (as if they had been heard on 437.375 MHz) and checks, for each, that the decision is the expected one: repeat
    (alias, WIDE2-N with trace), duplicate, fill-in tier, no path, own source, loop, hop limit. Then reads the journal that
    CPU1 keeps in flash (hops left, sends so far, time left, bytes). With `sdr` set an RTL-SDR + Direwolf also checks that the
    repeats really appear on 435.000 MHz with the rewritten path."""
    if not OBC.connected():
        raise RuntimeError("connect the Nucleo's ST-Link VCP port first")
    me, alias = digi_identity()
    tag = "%04d" % (int(now()) % 10000)
    wide = [
        ("alias is repeated",              f"T1TEST>APZS2S,{alias}:>digi test alias {tag}",        0,  f"T1TEST>APZS2S,{me}*:>digi test alias {tag}"),
        ("WIDE2-2: trace + one hop used",  f"T2TEST>APZS2S,WIDE2-2:>digi test wide2-2 {tag}",     0,  f"T2TEST>APZS2S,{me}*,WIDE2-1:>digi test wide2-2 {tag}"),
        ("same frame again = duplicate",   f"T1TEST>APZS2S,{alias}:>digi test alias {tag}",        9,  None),
        ("WIDE1-1 is the fill-in tier",    f"T3TEST>APZS2S,WIDE1-1:>digi test fill-in {tag}",      5,  None),
        ("no path = heard direct",         f"T4TEST>APZS2S:>digi test direct {tag}",               4,  None),
        ("our own source is never repeated", f"{me}>APZS2S,WIDE2-2:>digi test own source {tag}",   6,  None),
        ("loop: our call already used",    f"T5TEST>APZS2S,{me}*,WIDE2-1:>digi test loop {tag}",   7,  None),
        ("WIDE2-3 is above the hop limit", f"T6TEST>APZS2S,WIDE2-3:>digi test hops {tag}",         10, None),
    ]
    log(f"DIGIPEATER TEST: satellite {me}, alias {alias}, {len(wide)} frames injected through CPU1 (tag {tag})")
    if not OBC.ready(abort):
        raise RuntimeError("the satellite never answered on the VCP")
    dw = cfg.get("_cap")                         # a capture that is already running (radio monitor, end-to-end test)
    own_cap = False
    if dw is None and cfg.get("sdr"):
        own_cap = True
        ok_sdr, why = SDR.check_tuner()
        if not ok_sdr:
            raise RuntimeError("SDR not usable: " + " | ".join(why[:3]) + " (unplug and replug the dongle)")
        freq, gain = float(cfg.get("freq_mhz", 434.9936)), float(cfg.get("gain", 40))
        log(f"  SDR + Direwolf listening at {freq} MHz")
        dw = DwCapture(freq, gain)
        time.sleep(4)
    line0, before = saf_read(abort)
    log(f"  journal before: {line0 or 'no answer from the journal (store and forward off?)'}")
    rows = []
    try:
        for i, (name, text, want, air) in enumerate(wide, 1):
            if abort.is_set():
                break
            t0 = now()
            st, why = digi_inject(text, abort)
            ok = (st == (0 if want == 0 else 0x20 + want))
            rows.append({"n": i, "name": name, "frame": text, "expected": "repeated" if want == 0 else DIGI_REASONS[want],
                         "got": why, "ok": ok, "air": air, "seen": None, "wait_s": round(now() - t0, 1)})
            log(f"  {i}. {name}: {why} {'(as expected)' if ok else '(EXPECTED ' + rows[-1]['expected'] + ')'}", "ok" if ok else "err")
            BUS.publish("progress", test="digi", done=i, total=len(wide))
        if dw is not None:
            time.sleep(6)
            heard = dw.texts()
            for r in rows:
                if r["air"]:
                    r["seen"] = any(h.startswith(r["air"]) for h in heard)
                    log(f"  over the air: {r['air']} -> {'DECODED by Direwolf' if r['seen'] else 'not heard'}", "ok" if r["seen"] else "err")
            appended = [h for h in heard if "MEMORY OBJECTS" in h]
            if appended:
                log(f"  store statistics appended by the satellite: {appended[0][-150:]}", "ok")
    finally:
        if dw is not None and own_cap:
            dw.close()
    time.sleep(3)                                          # CPU2 reports a new record to the journal within a second or two
    line1, after = saf_read(abort)
    repeats = sum(1 for r in rows if r["expected"] == "repeated" and r["ok"])
    total_b = sum(x["len"] for x in after)
    log(f"  journal after: {line1 or 'no answer'}")
    for x in after:
        log(f"  MEMORY OBJECTS: #{x['n']} id={x['id']} len={x['len']} hops_left={x['hops_left']} forwarded={x['fwd_count']} ttl_left={x['ttl_s']}s | {x['text'][:90]}")
    log(f"  STORED {len(after)} msg, {total_b} B", "ok")
    store_ok = None if line1 is None else (len(after) >= min(repeats, len(before) + repeats))
    ok = bool(rows) and all(r["ok"] for r in rows) and (store_ok in (True, None)) and all(r["seen"] in (True, None) for r in rows)
    set_result("digi", {"test": "digi", "when": now(), "verdict": "PASS" if ok else "FAIL", "satellite": me, "alias": alias, "rows": rows,
                        "journal": line1, "stored": len(after), "stored_bytes": total_b, "objects": after, "repeats": repeats,
                        "sdr": bool(cfg.get("sdr"))})
