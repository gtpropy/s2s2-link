"""tb.tests_link"""

import json
import re
import struct
import time
from tb.bus import BUS, log
from tb.devices import SDR
from tb.links import GS, OBC
from tb.state import ACK_RX, set_result
from tb.util import DEFAULTS, OBC_CWTEST, OBC_RATE, OBC_RESET, now

def ack_wait_loop(cmd, cfg, abort, every=14.0, attempts=10, pattern=ACK_RX):
    """Send `cmd` until the satellite answers (`pattern` on the GS UART). The satellite only listens
    ~50 s of every ~95 s, so a command can fall into its CW phase and get lost; retry then.
    Never re-sends after an answer. Returns (mark_at_send, t_uplink_done, t_answer, tries)."""
    for i in range(1, attempts + 1):
        if abort.is_set():
            break
        mk = GS.mark()
        GS.send(cmd)
        _, t_ul = GS.wait(r"UPLINK COMPLETE", mk, 8, abort)
        line, t_ans = GS.wait(pattern, mk, every, abort)
        if line:
            return mk, t_ul, t_ans, i
        log(f"  no answer to {cmd} (try {i}/{attempts}) -- satellite is probably in its CW phase", "warn")
        if i == 2 and OBC.connected():
            # most likely the GS counter restarted (GS rebooted): clear the satellite's replay floor
            mk2 = OBC.mark()
            OBC.send(OBC_RESET, 2)
            if OBC.wait_ack(OBC_RESET, 2, mk2, 90, abort) == 0:
                log("  sent RESET over the VCP (replay floor cleared)", "warn")
    return None, None, None, attempts


def collect_rx_stats(lines):
    """Parse the GS UART lines of one command: decoded frames, CRC failures, RSSI, wrapped blocks, ACK kinds."""
    txt = "\n".join(t for _, t in lines)
    return {
        "decode_ok": txt.count("AX.25 DECODE OK"),
        "decode_failed": txt.count("DECODE FAILED"),
        "checked_blocks": txt.count("integrity check OK"),
        "rssi": [int(x) for x in re.findall(r"\[GS RX\] Packet #\d+ \| RSSI=(-?\d+) dBm", txt)],
        "ack_accepted": len(re.findall(r"ACK ACCEPTED|ACK Accepted", txt)) // 2 or txt.count("ACK ACCEPTED"),
        "ping_reply": bool(re.search(r"ACK: PING OK|HK TELEMETRY PACKET DETECTED", txt)),   # CHANGED: PING is now HEALTH SAT (ACK, HK packet, ACK)
        "ack_completed": bool(re.search(r"ACK COMPLETED|ACK Completed", txt)),
    }


def test_link(cfg, abort):
    n = int(cfg.get("count", 5))
    rows = []
    log(f"LINK TEST: {n} x PING")
    for k in range(1, n + 1):
        if abort.is_set():
            break
        mk, t_ul, t_ack, tries = ack_wait_loop("PING", cfg, abort)
        if mk is None:
            rows.append({"n": k, "ok": False, "why": "no ACK"})
            continue
        GS.wait(r"ACK COMPLETED|ACK Completed", mk, 12, abort)
        time.sleep(0.5)
        st = collect_rx_stats(GS.since(mk))
        ok = st["decode_ok"] >= 3 and st["ping_reply"] and st["ack_completed"]
        row = {"n": k, "ok": ok, "tries": tries,
               "latency_ms": int(1000 * (t_ack - t_ul)) if t_ul and t_ack else None,
               "rssi_avg": round(sum(st["rssi"]) / len(st["rssi"]), 1) if st["rssi"] else None,
               "frames": st["decode_ok"], "bad_frames": st["decode_failed"],
               "wrapped_blocks": st["checked_blocks"]}
        rows.append(row)
        log(f"  PING {k}/{n}: {'PASS' if ok else 'FAIL'} frames={st['decode_ok']} rssi={row['rssi_avg']} "
            f"latency={row['latency_ms']} ms")
        BUS.publish("progress", test="link", done=k, total=n)
        time.sleep(3)
    passed = sum(1 for r in rows if r["ok"])
    res = {"test": "link", "when": now(), "rows": rows, "passed": passed, "total": n,
           "verdict": "PASS" if rows and passed == n else ("PARTIAL" if passed else "FAIL")}
    set_result("link", res)
    log(f"LINK TEST {res['verdict']}: {passed}/{n}", "ok" if passed == n else "warn")


def test_sdr(cfg, abort):
    """RTL-SDR + Direwolf test: capture the satellite downlink and let Direwolf decode it (never passed yet:
    the SDR was not detected in the sessions so far)."""
    freq = float(cfg.get("freq_mhz", DEFAULTS["freq_mhz"]))
    gain = cfg.get("gain", DEFAULTS["sdr_gain"])
    present, info = SDR.present()
    if not present:
        set_result("sdr", {"test": "sdr", "when": now(), "verdict": "FAIL", "why": "SDR not found",
                           "detail": info})
        log("SDR: " + " | ".join(info), "err")
        return
    SDR.start(freq, gain)
    time.sleep(3)
    if SDR.proc.poll() is not None:
        set_result("sdr", {"test": "sdr", "when": now(), "verdict": "FAIL", "why": "pipeline exited",
                           "detail": SDR.lines[-5:]})
        return
    n = int(cfg.get("count", 3))
    gs_ok = 0
    for k in range(1, n + 1):
        if abort.is_set():
            break
        before = len(SDR.frames)
        mk, t_ul, t_ack, tries = ack_wait_loop("PING", cfg, abort)
        if mk is None:
            continue
        GS.wait(r"ACK COMPLETED|ACK Completed", mk, 12, abort)
        time.sleep(2)
        st = collect_rx_stats(GS.since(mk))
        gs_ok += st["decode_ok"]
        log(f"  PING {k}/{n}: GS decoded {st['decode_ok']} frames, Direwolf decoded "
            f"{len(SDR.frames) - before}")
        time.sleep(3)
    SDR.stop()
    res = {"test": "sdr", "when": now(), "freq_mhz": freq, "pings": n, "gs_frames": gs_ok,
           "direwolf_frames": len(SDR.frames), "frames": [f["line"][:160] for f in SDR.frames[:30]],
           "audio_level_avg": round(sum(SDR.levels) / len(SDR.levels), 1) if SDR.levels else None,
           "verdict": "PASS" if SDR.frames else "FAIL"}
    set_result("sdr", res)
    log(f"SDR TEST {res['verdict']}: Direwolf decoded {len(SDR.frames)} frame(s), GS decoded {gs_ok}",
        "ok" if SDR.frames else "err")


def test_rate(cfg, abort):
    """Raw air-rate test. For each gap the satellite (CPU2) sends N camera-format packets with that pause between them
    (0 ms = back to back), with no flash, no ring buffer, no CW beacon and no per-packet PA sequencing in between; the
    GS runs silent (RATE ON) and only counts. Reported per gap:
      air rate     = N * 200 B * 8 / (sat's own first-TX-start -> last-TX-done)   (what the modem really sustains)
      payload rate = the same with the 128 file bytes of each packet
      received / lost as counted by the GS, and the GS-side span first -> last packet."""
    if not OBC.connected():
        raise RuntimeError("connect the Nucleo's ST-Link VCP port first")
    n = int(cfg.get("count", 60))
    gaps = [int(x) for x in str(cfg.get("gaps", "300,100,50,20,0")).replace(" ", "").split(",") if x != ""]
    PKT_BITS = 200 * 8
    rows = []
    log(f"RAW RATE TEST: {n} packets per run, gaps {gaps} ms")
    if not OBC.ready(abort):
        raise RuntimeError("the satellite never answered on the VCP")
    for gap in gaps:
        if abort.is_set():
            break
        GS.send("RATE ON")
        time.sleep(0.6)
        mk = GS.mark()
        GS.send("RATE?")
        GS.wait(r'"type":"RATE"', mk, 3, abort)
        with OBC.lock:
            OBC.flashdata.pop(n, None)
        mk = OBC.mark()
        t0 = now()
        OBC.send(OBC_RATE, gap, n)
        limit = 130 + n * (0.45 + gap / 1000.0)
        st = OBC.wait_ack(OBC_RATE, gap, mk, limit, abort)
        wall = now() - t0
        time.sleep(1.5)                                    # let the GS drain its receive slots
        with OBC.lock:
            fd = OBC.flashdata.get(n)
        sent, elapsed = (struct.unpack("<II", fd[1][:8]) if fd and len(fd[1]) >= 8 else (0, 0))
        mk = GS.mark()
        GS.send("RATE?")
        GS.wait(r'"type":"RATE"', mk, 4, abort)
        g = {}
        for item in GS.since(mk):
            line = item[1] if isinstance(item, tuple) else item
            m = re.search(r'\{"type":"RATE".*\}', line)
            if m:
                try:
                    g = json.loads(m.group(0))
                except ValueError:
                    pass
        GS.send("RATE OFF")
        time.sleep(0.5)
        got = g.get("uniq", 0)
        span = (g.get("last", 0) - g.get("first", 0)) if g.get("first") else 0
        row = {"gap_ms": gap, "n": n, "status": st, "sat_sent": sent, "sat_ms": elapsed,
               "air_bps": int(sent * PKT_BITS * 1000 / elapsed) if elapsed else None,
               "payload_bps": int(sent * 128 * 8 * 1000 / elapsed) if elapsed else None,
               "ms_per_packet": round(elapsed / sent, 1) if sent else None,
               "gs_got": got, "gs_bad": g.get("bad", 0), "gs_rx": g.get("rx", 0), "gs_span_ms": span,
               "loss_pct": round(100.0 * (n - got) / n, 1) if n else None, "rssi": g.get("rssi"), "wall_s": round(wall, 1)}
        rows.append(row)
        log(f"  gap {gap:4d} ms: sat sent {sent}/{n} in {elapsed} ms = {row['ms_per_packet']} ms/packet -> air {row['air_bps']} bit/s, "
            f"payload {row['payload_bps']} bit/s | GS received {got}/{n} ({row['loss_pct']} % lost, {g.get('bad', 0)} bad)",
            "ok" if got == n and sent == n else "warn")
        BUS.publish("progress", test="rate", done=len(rows), total=len(gaps))
    ok = bool(rows) and all(r["sat_sent"] == n for r in rows)
    set_result("rate", {"test": "rate", "when": now(), "rows": rows, "count": n, "verdict": "PASS" if ok else "FAIL"})
    best = max((r for r in rows if r["gs_got"] == n and r["payload_bps"]), key=lambda r: r["payload_bps"], default=None)
    if best:
        log(f"RAW RATE: best loss-free setting = gap {best['gap_ms']} ms: air {best['air_bps']} bit/s, payload {best['payload_bps']} bit/s", "ok")


def test_cwtest(cfg, abort):
    """Continuous CW "HELLO" on the satellite's downlink frequency (435.000 MHz) for a spectrum analyzer such as a TinySA.
    Each round = 1 s steady carrier, then HELLO in Morse (80 ms unit, 15 WPM), then 1.5 s quiet (~9 s per round). Runs for
    `seconds` or until Stop is pressed; the satellite also stops by itself after that time. The GS decodes the Morse as a
    cross-check ("[CW DECODED] HELLO")."""
    if not OBC.connected():
        raise RuntimeError("connect the Nucleo's ST-Link VCP port first")
    secs = max(5, min(43200, int(cfg.get("seconds", 120))))
    mode = {"gmsk": 1, "carrier": 2}.get(str(cfg.get("mode", "cw")).lower(), 0)
    heard = 0
    log("CW HELLO TEST: waiting until the satellite answers (it is deaf while a CW beacon is being sent) ...")
    # Wait for CPU2's heartbeat first: a request that reaches CPU1 before CPU2 has booted is treated by CPU2 as a leftover
    # from before a reboot and ignored (that is how a freshly rebooted Nucleo ended up never sending anything).
    if not OBC.ready(abort, timeout=180):
        raise RuntimeError("the satellite did not answer on the VCP")
    st = None
    for attempt in range(1, 6):
        log(f"CW HELLO TEST: asking the satellite to start (try {attempt}) ...")
        mk = OBC.mark()
        OBC.send(OBC_CWTEST, secs, mode)
        # CPU1 answers OK only once CPU2 is really transmitting; status 8 = CPU2 did not pick the request up within 60 s
        st = OBC.wait_ack(OBC_CWTEST, secs, mk, 75, abort)
        if st == 0 or abort.is_set():
            break
    if st != 0:
        raise RuntimeError(f"the satellite did not start the CW test (status {st})")
    if mode:
        log(f"GMSK HELLO TEST ON for {secs} s: standard 4800 bit/s GMSK AX.25 packets carrying HELLO, back to back, centred on "
            "435.000 MHz (occupied width roughly 10 kHz). Press Stop to end it earlier.", "ok")
    else:
        log(f"CW HELLO TEST ON for {secs} s: carrier at 435.000 MHz keyed as HELLO (1 s carrier, HELLO, 1.5 s quiet, repeat). "
            "Press Stop to end it earlier.", "ok")
    end = now() + secs + 40            # +40 s: a beacon that was already running finishes before the test starts
    gmark = GS.mark() if GS.connected() else 0
    try:
        while now() < end and not abort.is_set():
            time.sleep(1.0)
            if GS.connected():
                n = sum(1 for _, ln in GS.since(gmark) if "CW DECODED" in ln and "HELLO" in ln)
                if n != heard:
                    heard = n
                    log(f"  ground station has decoded HELLO {heard} time(s)", "ok")
    finally:
        mk = OBC.mark()
        OBC.send(OBC_CWTEST, 0, mode)   # stop (idempotent)
        OBC.wait_ack(OBC_CWTEST, 0, mk, 5, None)
        log("CW HELLO TEST OFF (the satellite finishes the HELLO it is sending and returns to the normal 90 s beacon)", "ok")
    set_result("cwtest", {"test": "cwtest", "when": now(), "verdict": "PASS", "seconds": secs, "gs_decoded_hello": heard})
