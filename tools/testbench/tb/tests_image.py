"""tb.tests_image"""

import math
import os
import re
import socket
import subprocess
import threading
import time
from tb.bus import BUS, IMAGES, log
from tb.devices import LedTrace, SDR, _ax25_addr, _crc16_x25, _stop_pipeline
from tb.imaging import IMG, kind_of, set_image
from tb.links import GS, OBC
from tb.profiling import PROF
from tb.state import PROG, set_result
from tb.util import CHUNK, FLASH_BYTES, FLASH_CHUNKS, OBC_CHUNK, OBC_ERASE, OBC_SENDF, OBC_SETNAME, OBC_STORE, ROOT, now, sha256

def gs_chunk_lines(lines):
    """Yield (pkt0, total, offset, data) for every camera chunk the GS printed as JSON."""
    for t_, txt in lines:
        m = re.search(r'\{"type":"CAMERA","pkt":(\d+),"total":(\d+),"offset":(\d+),"len":(\d+),"hex":"([0-9A-Fa-f]*)"', txt)
        if m:
            yield int(m.group(1)) - 1, int(m.group(2)), int(m.group(3)), bytes.fromhex(m.group(5))


def test_image(cfg, abort):
    """Image test. store mode: PC -> CPU1 LittleFS (blue) -> read back and compare (green) -> CPU1 reads each
    chunk into the ring, CPU2 transmits (yellow) -> GS prints it; every chunk is cross-checked against the GS
    log and re-sent if missing. (A "direct" mode existed for the old CPU2-side link; CPU1 has no CHUNK command.)"""
    if IMG["bytes"] is None:
        raise RuntimeError("no image selected")
    if not OBC.connected():
        raise RuntimeError("connect the Nucleo's ST-Link VCP port first")
    src = IMG["bytes"]
    total = math.ceil(len(src) / CHUNK)
    mode = cfg.get("mode", "store")            # store = PC -> OBC flash -> read back -> RF 
    if mode != "store":
        # The CPU1 firmware (apps/examples/s2s2obc) has no CHUNK command any more: the image always goes
        # PC -> CPU1 LittleFS -> ring -> CPU2 -> RF. The old "direct" mode sent CHUNK frames, got no answer
        # and failed after 2 s with 0 chunks received (that was the "bench stopped working" report).
        raise RuntimeError(f"mode '{mode}' is not supported by the CPU1 firmware; use 'store'")
    ext, _ = kind_of(src)
    if mode == "store" and total > FLASH_CHUNKS:
        # The LittleFS store in CPU1 flash holds FLASH_BYTES. Instead of failing, shrink the picture to fit (JPEG);
        # a file that is not an image cannot be shrunk, so that case still stops with a clear message.
        log(f"{len(src)} B = {total} chunks does not fit the CPU1 flash store ({FLASH_CHUNKS} chunks): compressing", "warn")
        try:
            set_image(IMG["orig"] or src, IMG["name"], "flash")
        except Exception as e:
            raise RuntimeError(f"{IMG['name']} is {len(src)} B but CPU1's flash store holds {FLASH_BYTES} B and it could not "
                               f"be compressed: {e}") from e
        src = IMG["bytes"]
        total = math.ceil(len(src) / CHUNK)
    IMG["rx"] = None
    pieces = [src[i * CHUNK:(i + 1) * CHUNK] for i in range(total)]      # exact bytes, last one short
    log(f"IMAGE TEST ({'via OBC flash' if mode == 'store' else 'direct'}): {IMG['name']} -- {len(src)} B, "
        f"{total} chunks, sha256 {sha256(src)}")

    if not OBC.ready(abort):
        raise RuntimeError("the satellite never answered on the VCP (is the CPU2 firmware with the OBC link flashed?)")

    tracer = None
    if cfg.get("ledtrace") and cfg.get("sat_probe"):
        try:
            PROF.stop("sat")
            tracer = LedTrace(cfg["sat_probe"])
            tracer.start()
        except Exception as e:
            log(f"LED trace not available: {e}", "warn")
            tracer = None
    PROG["led"] = None

    stored, read_ok, acked = set(), set(), set()
    got = {}                          # chunk index -> (offset, data) as printed by the GS
    PROG.update(total=total, acked=acked, got=set(), stored=stored, read=read_ok, phase="", mode=mode)
    ack_ms = []
    retx = 0
    flash_sha = None
    store_info = None
    t_test = now()
    phase_t = {}

    def phase(name, led):
        PROG["phase"] = name
        phase_t[name] = now()
        BUS.publish("phase", name=name, led=led)
        log(f"-- phase: {name}  (Nucleo LED {led})")

    # ---------- 1. write the image into the OBC flash (blue) ----------
    if mode == "store":
        phase("write the file to CPU1 flash (LittleFS)", "BLUE")
        fname = re.sub(r"[^A-Za-z0-9._-]", "_", os.path.splitext(IMG["name"])[0])[:20] + "." + ext
        mk = OBC.mark()
        OBC.send(OBC_SETNAME, 0, 0, fname.encode())
        if OBC.wait_ack(OBC_SETNAME, 0, mk, 5, abort) != 0:
            raise RuntimeError("CPU1 rejected the file name")
        for attempt in range(3):
            mk = OBC.mark()
            OBC.send(OBC_ERASE, 0, total)
            st = OBC.wait_ack(OBC_ERASE, 0, mk, 20, abort)
            if st == 0:
                break
            log(f"  new-file answered {st}", "warn")
        else:
            raise RuntimeError("CPU1 could not create the file on its LittleFS (is the CPU1 s2s2obc app flashed?)")
        for idx in range(total):
            if abort.is_set():
                break
            for attempt in range(1, 5):
                mk = OBC.mark()
                OBC.send(OBC_STORE, idx, total, pieces[idx])
                st = OBC.wait_ack(OBC_STORE, idx, mk, 6, abort)
                if st == 0:
                    stored.add(idx)
                    BUS.publish("chunk", pkt=idx + 1, total=total, got=len(got), src="store")
                    break
                retx += 1
                log(f"  store chunk {idx + 1}: status {st}, try {attempt}", "warn")
            else:
                raise RuntimeError(f"flash write of chunk {idx + 1} failed")
        log(f"  {len(stored)}/{total} chunks written to the LittleFS file and verified")
        store_info = OBC.info(abort)
        log(f"  CPU1 says: {store_info}", "ok")

        # ---------- 2. read everything back out of the flash (green) ----------
        phase("read back from CPU1 flash", "GREEN")
        flash = bytearray()
        bad = []
        for idx in range(total):
            if abort.is_set():
                break
            st, blk, ok = OBC.read_flash(idx, abort)
            if st == 0 and blk is not None and ok and blk == pieces[idx]:
                read_ok.add(idx)
                BUS.publish("chunk", pkt=idx + 1, total=total, got=len(got), src="read")
            else:
                bad.append(idx + 1)
            flash += (blk or b"")
        flash_sha = sha256(bytes(flash[:len(src)]))
        log(f"  flash readback sha256 {flash_sha}  {'== source' if flash_sha == sha256(src) else 'MISMATCH'}",
            "ok" if flash_sha == sha256(src) else "err")
        if bad:
            raise RuntimeError(f"flash readback differs for chunks {bad[:10]}")

    # ---------- 3. transmit (read from flash -> M0+ -> RF) (yellow) ----------
    phase("CPU1 reads flash -> ring buffer -> CPU2 -> RF" if mode == "store" else "transmit over RF", "GREEN then YELLOW")
    gs_seen = len(GS.lines)
    consecutive_fail = 0

    def drain_gs():
        nonlocal gs_seen
        with GS.lock:
            new = GS.lines[gs_seen:]
            gs_seen = len(GS.lines)
        for idx, tot, off, data in gs_chunk_lines(new):
            if idx not in got:
                got[idx] = (off, data)
                PROG["got"].add(idx)
                BUS.publish("chunk", pkt=idx + 1, total=total, got=len(got), src="gs")

    for idx in range(total):
        if abort.is_set():
            break
        for attempt in range(1, 6):
            mk = OBC.mark()
            t0 = now()
            if mode == "store":
                OBC.send(OBC_SENDF, idx, total)
            else:
                OBC.send(OBC_CHUNK, idx, total, src[idx * CHUNK:(idx + 1) * CHUNK])
            st = OBC.wait_ack(OBC_SENDF if mode == "store" else OBC_CHUNK, idx, mk, 100 if mode == "store" else 15, abort)
            if st == 0:
                ack_ms.append(int(1000 * (now() - t0)))
                acked.add(idx)
                BUS.publish("chunk", pkt=idx + 1, total=total, got=len(got), src="ack")
                end = now() + 2.5
                while idx not in got and now() < end and not abort.is_set():
                    drain_gs()
                    time.sleep(0.03)
                drain_gs()
                if idx in got:
                    consecutive_fail = 0
                    break
                log(f"  chunk {idx + 1}/{total}: transmitted by the Nucleo but the GS did not print it -- sending again", "warn")
            else:
                log(f"  chunk {idx + 1}/{total}: no/bad answer from the Nucleo (status {st}) -- try {attempt}", "warn")
            retx += 1
            if attempt >= 5:
                consecutive_fail += 1
        if consecutive_fail >= 3:
            log("3 chunks in a row failed -- stopping", "err")
            break
        if (idx + 1) % 25 == 0:
            log(f"  {idx + 1}/{total} chunks transmitted, GS has {len(got)}")
    time.sleep(1.0)
    drain_gs()
    PROG["phase"] = "done"
    time.sleep(0.5)
    led_summary = None
    if tracer:
        tracer.stop()
        names = list(phase_t)
        wins = {n: (phase_t[n], (phase_t[names[i + 1]] if i + 1 < len(names) else now())) for i, n in enumerate(names)}
        led_summary = tracer.summary(wins)
        PROG["led"] = [(round(t - t_test, 2), c) for t, c in tracer.trace]
        log("LED pins seen over SWD: " + ", ".join(f"{k} {v}s" for k, v in led_summary["seconds_in_colour"].items()), "ok")
        for n, v in led_summary["phase_check"].items():
            log(f"  {n}: expected {'/'.join(v['expected'])} -> seen {100 * (v['seen_fraction'] or 0):.0f} % of the phase")

    buf = bytearray(total * CHUNK)
    for idx, (off, data) in got.items():
        if off + len(data) <= len(buf):
            buf[off:off + len(data)] = data
    rx = bytes(buf[:len(src)])
    missing = [i + 1 for i in range(total) if i not in got]
    match = (rx == src) and not missing
    IMG["rx"] = rx
    elapsed = now() - t_test
    with GS.lock:
        rssi = [int(x) for x in re.findall(r"\[GS RX\] Packet #\d+ \| RSSI=(-?\d+) dBm",
                                          "\n".join(tx for tt, tx in GS.lines if tt >= t_test))]
    res = {"test": "image", "mode": mode, "when": now(), "name": IMG["name"], "type": ext, "bytes": len(src),
           "chunks": total, "received": len(got), "acked": len(acked), "missing": missing,
           "stored_in_flash": len(stored), "flash_readback_ok": len(read_ok),
           "store_info": store_info, "flash_sha256": flash_sha, "flash_match": (flash_sha == sha256(src)) if flash_sha else None,
           "retransmits": retx, "src_sha256": sha256(src), "rx_sha256": sha256(rx), "match": match,
           "elapsed_s": round(elapsed, 1), "payload_bps": int(len(got) * CHUNK * 8 / max(elapsed, 1)),
           "ack_ms_avg": int(sum(ack_ms) / len(ack_ms)) if ack_ms else None,
           "rssi_avg": round(sum(rssi) / len(rssi), 1) if rssi else None,
           "led_check": led_summary,
           "verdict": "PASS" if match and (mode != "store" or flash_sha == sha256(src)) else "FAIL"}
    set_result("image", res)
    log(f"IMAGE TEST {res['verdict']}: GS received {len(got)}/{total} chunks ({retx} re-send(s)), "
        f"{'flash ' + ('MATCH, ' if res['flash_match'] else 'MISMATCH, ') if mode == 'store' else ''}"
        f"GS sha256 {'MATCH' if match else 'MISMATCH'}, {elapsed:.0f} s", "ok" if res["verdict"] == "PASS" else "err")


def test_sdrimage(cfg, abort):
    """Proof without the ground station: the Nucleo sends the selected image over the air, an RTL-SDR + Direwolf (a
    stock AX.25 / G3RUH decoder, nothing of ours) receives it, and the bench checks every frame it delivers:
    AX.25 addresses, control / PID, the camera fragment layout of the Packet Format sheet (type 0x08, CRC-16/X.25) and
    that the picture rebuilt from those fragments is byte-identical to the image that was sent."""
    if IMG["bytes"] is None:
        raise RuntimeError("no image selected")
    if not OBC.connected():
        raise RuntimeError("connect the Nucleo's ST-Link VCP port first")
    ok_sdr, why = SDR.check_tuner()
    if not ok_sdr:
        raise RuntimeError("SDR not usable: " + " | ".join(why[:3]) + " (unplug and replug the dongle)")
    freq = float(cfg.get("freq_mhz", 434.9936))
    gain = float(cfg.get("gain", 40))
    src = IMG["bytes"]
    if math.ceil(len(src) / CHUNK) > FLASH_CHUNKS:
        set_image(IMG["orig"] or src, IMG["name"], "flash")
        src = IMG["bytes"]
    total = math.ceil(len(src) / CHUNK)
    log(f"IMAGE VIA SDR + DIREWOLF: {IMG['name']} {len(src)} B = {total} fragments; SDR tuned to {freq} MHz (the dongle's own clock error moves it away from 435.000)")

    conf = os.path.join(ROOT, "tools", "testbench", "direwolf_s2s2_4800.conf")
    pipe = (f"rtl_fm -f {freq:.6f}M -s 48000 -g {gain} 2>/dev/null | "
            f"direwolf -c '{conf}' -r 48000 -n 1 -B 4800 -g -t 0 -")
    dw = subprocess.Popen(pipe, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
    frames = []
    stop = threading.Event()

    def kiss_reader():
        s = None
        for _ in range(60):
            try:
                s = socket.create_connection(("127.0.0.1", 8001), timeout=3)
                break
            except OSError:
                time.sleep(0.5)
        if s is None:
            log("Direwolf's KISS port 8001 never opened", "err")
            return
        s.settimeout(1.0)
        buf = b""
        while not stop.is_set():
            try:
                d = s.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            if not d:
                break
            buf += d
            while True:
                a = buf.find(b"\xc0")
                b = buf.find(b"\xc0", a + 1) if a >= 0 else -1
                if a < 0 or b < 0:
                    break
                fr, buf = buf[a + 1:b], buf[b:]
                if len(fr) > 1:
                    fr = fr[1:].replace(b"\xdb\xdc", b"\xc0").replace(b"\xdb\xdd", b"\xdb")
                    frames.append((now(), fr))
                    if len(fr) >= 154 and fr[16] == 0x08 and (fr[16 + 136] | (fr[16 + 137] << 8)) == _crc16_x25(fr[16:16 + 136]):
                        IMAGES.ingest("sdr", fr[18] | (fr[19] << 8), fr[20] | (fr[21] << 8), fr[24:152])
                    if len(fr) >= 17:                    # one line per frame in the page's "Direwolf" log tab
                        inf = fr[16:]
                        extra = ""
                        if len(inf) == 138 and inf[0] == 0x08:
                            crc_ok = (inf[136] | (inf[137] << 8)) == _crc16_x25(inf[:136])
                            extra = f" | camera fragment {(inf[2] | (inf[3] << 8)) + 1}/{inf[4] | (inf[5] << 8)}, CRC-16/X.25 {'ok' if crc_ok else 'BAD'}"
                        BUS.publish("sdr", msg=f"frame {len(frames)}: {_ax25_addr(fr[7:14])} > {_ax25_addr(fr[0:7])}  ctl 0x{fr[14]:02X} pid 0x{fr[15]:02X}  info {len(inf)} B{extra}")
        s.close()

    threading.Thread(target=kiss_reader, daemon=True).start()
    time.sleep(3)
    good = {}                      # fragment index -> 128 data bytes (CRC checked)
    other = 0
    heads = set()
    infos = set()
    last_pidctl = (None, None)

    def digest():
        nonlocal other, last_pidctl
        good.clear()
        other = 0
        for _, fr in list(frames):
            if len(fr) < 17:
                other += 1
                continue
            heads.add((_ax25_addr(fr[7:14]), _ax25_addr(fr[0:7])))
            last_pidctl = (fr[14], fr[15])
            info = fr[16:]
            infos.add(len(info))
            if len(info) == 138 and info[0] == 0x08 and (info[136] | (info[137] << 8)) == _crc16_x25(info[:136]):
                idx, tot = info[2] | (info[3] << 8), info[4] | (info[5] << 8)
                if tot == total and idx < total:
                    good.setdefault(idx, info[8:136])
                    continue
            other += 1

    PROG.update(total=total, acked=set(), got=set(), stored=set(), read=set(), phase="", mode="sdr", led=None)

    def phase(name, led):
        PROG["phase"] = name
        BUS.publish("phase", name=name, led=led)
        log(f"-- phase: {name}  (Nucleo LED {led})")

    def chunk_ev(i, src):
        BUS.publish("chunk", pkt=i + 1, total=total, got=len(PROG["got"]), src=src)

    def new_good():
        """digest() the frames Direwolf delivered so far and tell the page about fragments that just arrived."""
        before = set(good)
        digest()
        for i in sorted(set(good) - before):
            PROG["got"].add(i)
            chunk_ev(i, "gs")

    try:
        if not OBC.ready(abort, timeout=180):
            raise RuntimeError("the satellite did not answer on the VCP")

        def cmd(c, seq=0, tot=0, data=b"", to=10):
            mk = OBC.mark()
            OBC.send(c, seq, tot, data)
            return OBC.wait_ack(c, seq, mk, to, abort)

        phase("write the file to CPU1 flash (LittleFS)", "BLUE")
        cmd(OBC_ERASE, 1, 0, b"", 15)
        for i in range(total):
            for _ in range(4):
                if cmd(OBC_STORE, i, total, src[i * CHUNK:(i + 1) * CHUNK]) == 0:
                    PROG["stored"].add(i)
                    chunk_ev(i, "store")
                    break
            else:
                raise RuntimeError(f"flash write of fragment {i} failed")
        log(f"  {total}/{total} fragments stored in CPU1's LittleFS file", "ok")
        phase("CPU1 reads flash -> ring buffer -> CPU2 -> RF -> SDR + Direwolf", "GREEN then YELLOW")
        t0 = now()
        for pass_no in range(1, 4):
            todo = [i for i in range(total) if i not in good]
            if not todo or abort.is_set():
                break
            if pass_no > 1:
                log(f"  pass {pass_no}: re-sending {len(todo)} fragment(s) Direwolf did not deliver")
            for n, i in enumerate(todo):
                if abort.is_set():
                    break
                if cmd(OBC_SENDF, i, total, b"", 110) == 0:
                    PROG["acked"].add(i)
                    chunk_ev(i, "ack")
                time.sleep(0.25)                     # let Direwolf deliver this fragment
                new_good()
                if n % 10 == 9:
                    log(f"  {len(good)}/{total} fragments decoded by Direwolf so far")
            time.sleep(3)
            new_good()
        elapsed = now() - t0
        PROG["phase"] = "done"
    finally:
        stop.set()
        _stop_pipeline(dw)
    digest()
    rebuilt = b"".join(good.get(i, b"\x00" * CHUNK) for i in range(total))[:len(src)]
    IMG["rx"] = rebuilt
    match = rebuilt == src and len(good) == total
    res = {"test": "sdrimage", "when": now(), "name": IMG["name"], "bytes": len(src), "fragments": total,
           "frames": len(frames), "valid": len(good), "other": other, "missing": [i + 1 for i in range(total) if i not in good][:40],
           "addresses": sorted(f"{a} -> {b}" for a, b in heads), "control": f"0x{last_pidctl[0]:02X}" if last_pidctl[0] is not None else None,
           "pid": f"0x{last_pidctl[1]:02X}" if last_pidctl[1] is not None else None, "info_lengths": sorted(infos),
           "src_sha256": sha256(src), "rx_sha256": sha256(rebuilt), "match": match, "elapsed_s": round(elapsed, 1),
           "freq_mhz": freq, "verdict": "PASS" if match else "FAIL",
           "raw_frames": [fr.hex() for _, fr in frames]}
    set_result("sdrimage", res)
    log(f"IMAGE VIA SDR + DIREWOLF {res['verdict']}: Direwolf delivered {len(frames)} frames, {len(good)}/{total} valid camera fragments, "
        f"rebuilt picture {'IDENTICAL to' if match else 'DIFFERENT from'} the image sent ({res['rx_sha256'][:16]}... vs {res['src_sha256'][:16]}...)",
        "ok" if match else "err")


def test_imageloop(cfg, abort):
    """Send the selected image's camera fragments over the air again and again until Stop (or `seconds`), for screenshots of a
    terminal Direwolf (rtl_fm | direwolf -d p). The image is stored in CPU1's flash first if the file there is not the
    selected one. `fragments` = how many fragments per round, counting from the first (0 = the whole image): the first
    fragment carries the JPEG header (FF D8 FF E0 ... "JFIF"), so a short round shows it every few seconds."""
    if IMG["bytes"] is None:
        raise RuntimeError("no image selected")
    if not OBC.connected():
        raise RuntimeError("connect the Nucleo's ST-Link VCP port first")
    src = IMG["bytes"]
    if math.ceil(len(src) / CHUNK) > FLASH_CHUNKS:
        set_image(IMG["orig"] or src, IMG["name"], "flash")
        src = IMG["bytes"]
    total = math.ceil(len(src) / CHUNK)
    per_round = total if int(cfg.get("fragments", 10)) <= 0 else min(total, int(cfg.get("fragments", 10)))
    secs = max(10, min(43200, int(cfg.get("seconds", 3600))))
    log(f"CAMERA FRAGMENT LOOP: {IMG['name']} {len(src)} B = {total} fragments; sending the first {per_round} over and over for up to {secs} s")
    if not OBC.ready(abort, timeout=180):
        raise RuntimeError("the satellite did not answer on the VCP")

    def cmd(c, seq=0, tot=0, data=b"", to=10):
        mk = OBC.mark()
        OBC.send(c, seq, tot, data)
        return OBC.wait_ack(c, seq, mk, to, abort)

    info = OBC.info(abort)
    have = (info or "") if isinstance(info, str) else str(info or "")
    if f"{len(src)} B" in have:               # same size as the selected image: it is the file already in CPU1's flash
        log(f"  the image is already in CPU1's flash ({have.strip()[:90]})")
    else:
        log("-- phase: store the image in CPU1 flash")
        cmd(OBC_ERASE, 1, 0, b"", 15)
        for i in range(total):
            for _ in range(4):
                if cmd(OBC_STORE, i, total, src[i * CHUNK:(i + 1) * CHUNK]) == 0:
                    break
            else:
                raise RuntimeError(f"flash write of fragment {i} failed")
        log(f"  {total}/{total} fragments stored", "ok")
    log("CAMERA FRAGMENTS ON AIR: run  rtl_fm -f <your SDR frequency> -s 48000 -g 30 | direwolf -c tools/testbench/direwolf_s2s2_4800.conf -r 48000 -n 1 -B 4800 -g -t 0 -d p -  "
        "and screenshot the terminal. Press Stop to end.", "ok")
    t_end = now() + secs
    sent = rounds = 0
    try:
        while now() < t_end and not abort.is_set():
            for i in range(per_round):
                if abort.is_set() or now() >= t_end:
                    break
                if cmd(OBC_SENDF, i, total, b"", 110) == 0:
                    sent += 1
            rounds += 1
            if rounds % 5 == 0:
                log(f"  {sent} fragments sent so far ({rounds} rounds)")
    finally:
        log(f"CAMERA FRAGMENT LOOP OFF: {sent} fragments sent in {rounds} round(s)", "ok")
    set_result("imageloop", {"test": "imageloop", "when": now(), "verdict": "PASS", "sent": sent, "rounds": rounds, "fragments": per_round})
