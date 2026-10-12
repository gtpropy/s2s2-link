"""tb.e2e"""

import math
import os
import re
import shutil
import time
from tb.bus import BUS, EVENTS, IMAGES, log
from tb.commands import run_command
from tb.devices import SDR
from tb.digi import saf_read
from tb.imaging import IMG, kind_of
from tb.links import GS, OBC
from tb.profiling import MON, monitor_start, monitor_stop
from tb.reports import save_report
from tb.state import STATE, set_result
from tb.tests_digi import test_digi
from tb.tests_image import test_image, test_sdrimage
from tb.util import CHUNK, OBC_ERASE, OBC_SENDF, OBC_SETNAME, OBC_STORE, now, sha256

# --------------------------------------------------------------------------------------
# end to end test
# --------------------------------------------------------------------------------------
def image_tx_only(abort):
    """No receiver: write the picture into CPU1's flash, read it back, and send every chunk (CPU2 transmits it). Proves the transmit side."""
    src = IMG["bytes"]
    total = math.ceil(len(src) / CHUNK)
    pieces = [src[i * CHUNK:(i + 1) * CHUNK] for i in range(total)]
    ext, _ = kind_of(src)
    mk = OBC.mark()
    OBC.send(OBC_SETNAME, 0, 0, (re.sub(r"[^A-Za-z0-9._-]", "_", os.path.splitext(IMG["name"])[0])[:20] + "." + ext).encode())
    OBC.wait_ack(OBC_SETNAME, 0, mk, 5, abort)
    mk = OBC.mark()
    OBC.send(OBC_ERASE, 0, total)
    if OBC.wait_ack(OBC_ERASE, 0, mk, 20, abort) != 0:
        raise RuntimeError("CPU1 could not create the file")
    for i in range(total):
        for _ in range(4):
            mk = OBC.mark()
            OBC.send(OBC_STORE, i, total, pieces[i])
            if OBC.wait_ack(OBC_STORE, i, mk, 6, abort) == 0:
                break
        else:
            raise RuntimeError(f"flash write of chunk {i + 1} failed")
        BUS.publish("chunk", pkt=i + 1, total=total, got=0, src="store")
    back = bytearray()
    for i in range(total):
        st, blk, ok = OBC.read_flash(i, abort)
        if st != 0 or not ok or blk != pieces[i]:
            raise RuntimeError(f"flash readback of chunk {i + 1} differs")
        back += blk
    sent = 0
    for i in range(total):
        if abort.is_set():
            break
        mk = OBC.mark()
        OBC.send(OBC_SENDF, i, total)
        if OBC.wait_ack(OBC_SENDF, i, mk, 100, abort) == 0:
            sent += 1
            BUS.publish("chunk", pkt=i + 1, total=total, got=0, src="ack")
    return {"chunks": total, "stored": total, "flash_sha256": sha256(bytes(back[:len(src)])), "transmitted": sent, "match": bytes(back[:len(src)]) == src}


def test_e2e(cfg, abort):
    """End to end through the real hardware: equipment, satellite alive, command + ACK + housekeeping + NACK, CW beacon, digipeater
    with its store-and-forward journal, and a picture that goes PC -> CPU1 flash -> CPU2 -> RF -> receiver -> background builder ->
    identical picture. Steps that need a receiver (the ground station and / or the SDR) are SKIPPED, not faked, when none is there."""
    steps = []

    def step(name, fn, needs=None):
        rec = {"n": len(steps) + 1, "name": name, "state": "running", "detail": "", "t0": now()}
        steps.append(rec)
        BUS.publish("e2e", steps=steps)
        if needs and not needs[0]:
            rec.update(state="skip", detail=needs[1])
        else:
            try:
                rec["detail"] = fn() or ""
                rec["state"] = "pass"
            except AssertionError as e:
                rec.update(state="fail", detail=str(e))
            except Exception as e:
                rec.update(state="fail", detail=f"{type(e).__name__}: {e}")
        rec["seconds"] = round(now() - rec["t0"], 1)
        log(f"E2E {rec['n']}. {name}: {rec['state'].upper()} {rec['detail'][:160]}", {"pass": "ok", "skip": "warn"}.get(rec["state"], "err"))
        BUS.publish("e2e", steps=steps)
        return rec["state"]

    gs_ok, obc_ok = GS.connected(), OBC.connected()
    sdr_ok = bool(SDR.present()[0]) if not MON["cap"] else True
    have_rx = gs_ok or sdr_ok
    if not obc_ok:
        raise RuntimeError("connect the Nucleo's ST-Link VCP port first (the satellite side)")
    cap_started = False
    cap = MON["cap"]
    log(f"END-TO-END TEST: satellite VCP {'yes' if obc_ok else 'no'}, ground station {'yes' if gs_ok else 'no'}, SDR {'yes' if sdr_ok else 'no'}")

    def s_equipment():
        return f"satellite VCP {OBC.port}; ground station {GS.port or '-'}; SDR {'yes' if sdr_ok else '-'}; direwolf {'yes' if shutil.which('direwolf') else 'NO'}"

    def s_alive():
        assert OBC.ready(abort, timeout=int(cfg.get("alive_timeout", 150))), "CPU2 never answered on the VCP (heartbeat)"
        line, recs = saf_read(abort)
        return f"CPU2 heartbeat ok; {line or 'journal: no answer'}"

    def s_command():
        r = run_command("health", {}, abort)
        assert r["verdict"] == "completed", f"HEALTH SAT: {r['verdict']}"
        kinds = [e["kind"] + (":" + e["what"] if e["kind"] == "ack" else "") for e in r["events"]]
        assert "ack:accepted" in kinds, "no ACK accepted"
        return f"HEALTH SAT: {' -> '.join(kinds[:6])} in {r['seconds']} s ({r['attempts']} try)"

    def s_nack():
        r = run_command("bad", {}, abort)
        nk = [e for e in r["events"] if e["kind"] == "nack"]
        assert nk and nk[0]["err"] == 9, f"expected NACK invalid command, got {r['verdict']}"
        return f"invalid opcode refused: NACK 0xEE 0x{nk[0]['err']:02X} ({nk[0]['name']}), mission 0x{nk[0]['mission']:02X}"

    def s_cw():
        t0 = now()
        end = t0 + float(cfg.get("cw_timeout", 130))
        while now() < end and not abort.is_set():
            ev = [e for e in EVENTS.recent(limit=300) if e["t"] >= t0 and e["kind"] in ("beacon", "cw")]
            if any(e["kind"] == "beacon" for e in ev):
                b = next(e for e in ev if e["kind"] == "beacon")
                return f"CW beacon decoded: {b['hex']} (battery {b['b1']['bat_v']} V, {b['b1']['system']})"
            if any("S2S2" in (e.get("text") or "") for e in ev):
                return "CW text decoded: " + next(e["text"] for e in ev if "S2S2" in (e.get("text") or ""))
            time.sleep(1)
        raise AssertionError("no CW beacon decoded in time")

    def s_digi():
        c2 = dict(cfg, sdr=False)
        if cap:
            c2["_cap"] = cap
        test_digi(c2, abort)
        r = STATE["digi"]
        assert r and r["verdict"] == "PASS", "digipeater test: " + str((r or {}).get("verdict")) + " " + "; ".join(
            f"{x['n']}:{x['got']}" for x in (r or {}).get("rows", []) if not x["ok"])
        seen = [x for x in r["rows"] if x["air"]]
        over = sum(1 for x in seen if x["seen"])
        return f"8/8 decisions as expected; {r['stored']} messages / {r['stored_bytes']} B stored; on the air: {over}/{len(seen)} repeats decoded" if cap else \
            f"8/8 decisions as expected; {r['stored']} messages / {r['stored_bytes']} B stored (no SDR: repeats not checked on the air)"

    def s_saf():
        line, recs = saf_read(abort)
        assert line, "the store-and-forward journal did not answer"
        assert recs, "no stored message although the digipeater repeated frames"
        return f"{line}; {len(recs)} message(s), {sum(r['len'] for r in recs)} B, e.g. {recs[0]['text'][:70]}"

    img_res = {}

    def s_image():
        assert IMG["bytes"], "no image selected"
        if gs_ok:
            test_image(dict(cfg, mode="store", fresh=False), abort)
            r = STATE["image"]
            assert r and r["verdict"] == "PASS", f"image test via the ground station: {(r or {}).get('verdict')} ({(r or {}).get('received')}/{(r or {}).get('chunks')} chunks)"
            img_res.update(src="ground station", chunks=r["chunks"], seconds=r["elapsed_s"])
        elif sdr_ok:
            cfg2 = dict(cfg, fresh=False)
            if cap:
                monitor_stop()
                time.sleep(2)
            test_sdrimage(cfg2, abort)
            r = STATE["sdrimage"]
            assert r and r["verdict"] == "PASS", f"image via the SDR: {(r or {}).get('verdict')}"
            img_res.update(src="SDR + Direwolf", chunks=r["fragments"], seconds=r.get("elapsed_s"))
        else:
            r = image_tx_only(abort)
            assert r["match"] and r["transmitted"] == r["chunks"], "flash readback or transmit failed"
            img_res.update(src="none (transmit side only)", chunks=r["chunks"], seconds=None)
            return f"{r['chunks']} chunks written to CPU1 flash, read back identical (sha256 {r['flash_sha256'][:12]}), CPU2 transmitted {r['transmitted']}; NO receiver, so reception was not checked"
        # the background builder: it saw the same fragments while the test ran
        deadline = now() + 8
        sess = None
        while now() < deadline:
            lst = IMAGES.list()
            sess = next((x for x in lst if x["complete"] and x["total"] == img_res["chunks"]), None)
            if sess:
                break
            time.sleep(0.5)
        assert sess, "the background image builder has no complete picture"
        assert sess["matches_sent"], f"background-built picture sha256 {sess['sha256'][:12]} differs from the one sent"
        img_res["session"] = sess["id"]
        return (f"{img_res['chunks']} fragments via {img_res['src']}; background builder: session {sess['id']} complete, sha256 {sess['sha256'][:12]} "
                f"= sent, sources {sess['sources']}")

    def s_report():
        return "report saved: " + save_report("end-to-end test")

    try:
        if sdr_ok and not cap and not gs_ok:
            try:
                monitor_start(cfg.get("freq_mhz"), cfg.get("gain"))
                cap, cap_started = MON["cap"], True
                time.sleep(4)
            except Exception as e:
                log(f"radio monitor not started: {e}", "warn")
        step("equipment", s_equipment)
        step("satellite alive (CPU2 heartbeat, CPU1 journal)", s_alive)
        step("command link: HEALTH SAT -> ACK, housekeeping, ACK", s_command, (gs_ok, "ground station not connected"))
        step("refused command: invalid opcode -> NACK", s_nack, (gs_ok, "ground station not connected"))
        step("CW beacon decoded (S2S2 + housekeeping bytes)", s_cw, (gs_ok and bool(cfg.get("cw", True)), "ground station not connected (or CW step off)"))
        step("digipeater: 8 injected frames, repeats, journal", s_digi, (True, ""))
        step("store-and-forward journal holds the repeated frames", s_saf)
        step("image: PC -> CPU1 flash -> CPU2 -> RF -> receiver -> background builder", s_image, (IMG["bytes"] is not None, "no image selected"))
        step("report", s_report)
    finally:
        if cap_started:
            monitor_stop()
    states = [x["state"] for x in steps]
    verdict = "FAIL" if "fail" in states else ("PASS" if have_rx and "skip" not in states else "PARTIAL")
    set_result("e2e", {"test": "e2e", "when": now(), "verdict": verdict, "steps": steps, "receivers": {"gs": gs_ok, "sdr": sdr_ok},
                       "image": img_res, "pass": states.count("pass"), "fail": states.count("fail"), "skip": states.count("skip")})
    log(f"END-TO-END {verdict}: {states.count('pass')} passed, {states.count('fail')} failed, {states.count('skip')} skipped", "ok" if verdict == "PASS" else "warn")
