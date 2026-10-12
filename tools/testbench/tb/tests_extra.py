"""
tests_extra.py -- more testbench tests. Every test talks to the real boards (UART of the ground station, the Nucleo's ST-Link VCP,
SWD through OpenOCD); nothing is simulated. The analysis helpers at the top are pure functions covered by
tests/host/test_testbench_analysis.py; the tests themselves are built by make_tests().

  Boot / firmware (SWD only)      fwcheck    flash content of the board == the build in this repository
                                  lfsproof   the exact picture file is inside CPU1's LittleFS pages (raw flash read, no firmware involved)
                                  bootcfg    CPU2 boot option bytes, C2BOOT, self-test words, CPU2 heartbeat
                                  membudget  flash / RAM / stack headroom from the ELF against limits
                                  profcap    CPU time per function of both cores for N seconds (report material)
  Nucleo only (VCP)               vcpfuzz    corrupt / truncated / oversized OBC frames: CPU1 must survive and must not act on bad frames
                                  flashio    CPU1 LittleFS file: write, read back, compare, throughput
  Link (GS board + satellite)     cmdconf    every sheet command: ACK accepted, data, ACK completed, latency
                                  nackmatrix malformed commands: which NACK / silence the satellite gives
                                  beacontime CW beacon period, callsign and decode rate over a time window
                                  winmap     when in its 90 s cycle does the satellite listen
                                  soak       PING every cycle for N minutes: success rate, latency, RSSI
"""
import os
import random
import statistics
import struct
import tempfile
import time

from tb import profiler as prof_mod
from tb.bus import BUS, EVENTS, log
from tb.commands import run_command
from tb.compat import CORE
from tb.links import GS, OBC
from tb.profiling import PROF, prof_start, prof_targets
from tb.state import set_result
from tb.util import (GS_BUILD, OBC_ERASE, OBC_SETNAME, OBC_STATUS, OBC_STORE, OBC_SYNC, ROOT, SAT_BUILD, crc16_ccitt, now, obc_frame)

NEEDS = {"lfsproof": "swd", "scenario": "none", "fwcheck": "swd", "bootcfg": "swd", "membudget": "none", "profcap": "swd", "vcpfuzz": "obc", "flashio": "obc",
         "cmdconf": "gs", "nackmatrix": "gs", "beacontime": "gs", "winmap": "gs", "soak": "gs"}


# ----------------------------------------------------------------------------------------------------------------- pure helpers
def period_stats(times):
    """Gaps between successive event times."""
    gaps = [b - a for a, b in zip(times, times[1:])]
    if not gaps:
        return {"n": len(times), "gaps": []}
    return {"n": len(times), "gaps": [round(g, 1) for g in gaps], "mean": round(statistics.mean(gaps), 2), "min": round(min(gaps), 1),
            "max": round(max(gaps), 1), "std": round(statistics.pstdev(gaps), 2)}


def check_beacon_period(times, expected, tol, window_s):
    """(ok, stats, why). Every gap must be within `tol` of `expected` (a gap of 2x means one beacon was not decoded) and the count must fit the window."""
    st = period_stats(times)
    exp_n = int(window_s // expected)
    st["expected_n"] = exp_n
    if len(times) < 2:
        return False, st, f"only {len(times)} beacon(s) decoded in {window_s:.0f} s (expected about {exp_n})"
    bad = [g for g in st["gaps"] if abs(g - expected) > tol]
    if bad:
        miss = sum(1 for g in bad if abs(g / expected - round(g / expected)) * expected <= tol and round(g / expected) >= 2)
        return False, st, f"{len(bad)} gap(s) off the {expected:.0f} s period (longest {max(bad):.0f} s, {miss} look like missed decodes)"
    return True, st, ""


def estimate_window(probes, min_len=20.0):
    """probes: [(offset_s, ok)] possibly several per offset. Offsets that answered in EVERY sweep form the listen window.
    Returns (start, end, contiguous_len) of the longest run of answering offsets, or None."""
    by = {}
    for off, ok in probes:
        by.setdefault(off, []).append(bool(ok))
    offs = sorted(by)
    good = [o for o in offs if all(by[o])]
    if not good:
        return None
    step = min([b - a for a, b in zip(offs, offs[1:])] or [1.0])
    best, run = [], []
    for o in offs:
        if o in good and (not run or abs(o - run[-1] - step) < 1e-6):
            run.append(o)
        elif o in good:
            run = [o]
        else:
            run = []
        if len(run) > len(best):
            best = list(run)
    if not best:
        return None
    return best[0], best[-1] + step, best[-1] + step - best[0]


def first_mismatch(a, b):
    """Index of the first differing byte, or None when equal (length differences count)."""
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return None if len(a) == len(b) else n


def count_diff(a, b):
    return sum(1 for x, y in zip(a, b) if x != y) + abs(len(a) - len(b))


def pattern_chunk(i, seed=0x5A17, n=128):
    """Deterministic pseudo random 128-byte chunk i (the same on write and on verify)."""
    rng = random.Random((seed << 20) ^ i)                  # ONE generator per chunk: a new one per byte would repeat a single value
    return bytes(rng.getrandbits(8) for _ in range(n))


LFS_BASE, LFS_PAGES, LFS_PAGE = 0x08024000, 28, 2048                      # CPU1 LittleFS: flash pages 72..99


def chunk_hits(flash, data, chunk=128, win=32):
    """How many chunks of `data` can be found in the raw flash image. LittleFS puts block pointers into file blocks, so a chunk that straddles a
    block boundary is not contiguous: a `win`-byte window from the middle of every chunk is searched instead. Returns (found, total, missing indexes)."""
    total, found, missing = 0, 0, []
    for i in range(0, len(data), chunk):
        piece = data[i:i + chunk]
        total += 1
        w = piece if len(piece) <= win else piece[len(piece) // 2 - win // 2:len(piece) // 2 + win // 2]
        if flash.find(w) >= 0:
            found += 1
        else:
            missing.append(i // chunk + 1)
    return found, total, missing


def lfs_superblocks(flash, block=LFS_PAGE):
    """Block numbers whose metadata starts with the LittleFS superblock magic ('littlefs' at byte 8)."""
    return [b for b in range(len(flash) // block) if flash[b * block + 8:b * block + 16] == b"littlefs"]


def fuzz_cases(frame_fn, crc_fn, sync):
    """(name, bytes, must_be_ignored, why) frames that must NOT be acted on. frame_fn(cmd, seq, total, data) builds a valid frame."""
    good = frame_fn(2, 0x1111, 0, b"")
    bad_crc = bytearray(good)
    bad_crc[-1] ^= 0xFF
    body = bytes([2]) + struct.pack("<HH", 0x2222, 0) + bytes([200])         # declares 200 data bytes, then stops
    trunc = sync + body + b"\x00" * 4
    flipped = bytearray(frame_fn(2, 0x3333, 0, b""))
    flipped[len(sync) + 1] ^= 0x01                                               # one bit in the sequence number, CRC now wrong
    rnd = random.Random(7)
    return [("bad CRC", bytes(bad_crc), True, "STATUS with a wrong checksum must not be answered"),
            ("one flipped bit", bytes(flipped), True, "a bit error in the header must be caught by the CRC"),
            ("declared length longer than the data", trunc, True, "incomplete frame: no answer, and the parser must recover afterwards"),
            ("random garbage 256 B", bytes(rnd.getrandbits(8) for _ in range(256)), True, "line noise must not be answered"),
            ("sync too short", good[1:], False, "only 7 sync bytes: tolerance is allowed, noted")]


# ----------------------------------------------------------------------------------------------------------------- SWD helper
class Swd:
    """One-shot SWD access: reuses the profiler's OpenOCD when attached, otherwise starts a temporary one."""

    def __init__(self, target):
        self.target = target
        self.oo = None
        self.own = False
        self.tmp = tempfile.mkdtemp(prefix="s2s2swd")

    def __enter__(self):
        sess = PROF.sessions.get(self.target)
        if sess:
            self.oo = sess.oo
            return self
        PROF.targets = prof_targets()
        t = PROF.targets[self.target]
        import socket
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        self.oo = prof_mod.OpenOcd(t["serial"], port)
        self.oo.start()
        self.own = True
        return self

    def __exit__(self, *a):
        if self.own and self.oo:
            self.oo.stop()

    def dump(self, addr, n):
        path = os.path.join(self.tmp, "d.bin")
        if os.path.exists(path):
            os.remove(path)
        r = self.oo.cmd("targets stm32wlx.cpu0; dump_image {%s} 0x%X %d" % (path, addr, n))
        if not os.path.exists(path):
            raise RuntimeError("SWD read failed: " + r[:100])
        return open(path, "rb").read()

    def r32(self, addr):
        return int.from_bytes(self.dump(addr, 4), "little")


def make_tests():
    T = {}

    def finish(name, verdict, rows, **kw):
        res = {"test": name, "when": now(), "verdict": verdict, "rows": rows, **kw}
        set_result(name, res)
        log(f"{name.upper()} {verdict}: " + "; ".join(f"{r['name']} {r['state']}" for r in rows[:6]), "ok" if verdict == "PASS" else "warn" if verdict == "INFO" else "err")

    def row(name, ok, detail, expected=""):
        return {"name": name, "state": "PASS" if ok is True else "FAIL" if ok is False else "INFO", "detail": detail, "expected": expected}

    def verdict_of(rows):
        st = [r["state"] for r in rows]
        return "FAIL" if "FAIL" in st else ("PASS" if "PASS" in st else "INFO")

    def target_of(cfg):
        return cfg.get("target", "sat") if cfg.get("target") in ("sat", "gs") else "sat"

    # ---- boot / firmware
    def fwcheck(cfg, abort):
        tg = target_of(cfg)
        rows = []
        img = os.path.join(SAT_BUILD if tg == "sat" else GS_BUILD, "satellite.bin" if tg == "sat" else "ground_station.bin")
        nutx = os.path.join(ROOT, "nuttx", "nuttx.bin")
        with Swd(tg) as sw:
            from tb.compat import cubus_on
            parts = [("CPU2 image", img, 0x08032000)]
            if not (tg == "sat" and cubus_on()):                    # CUBUS: CPU1 holds the OBC firmware, not this repository's NuttX bench image
                parts.append(("CPU1 NuttX image", nutx, 0x08000000))
            for label, path, base in parts:
                if not os.path.exists(path):
                    rows.append(row(label, None, f"{os.path.relpath(path, ROOT)} not built: nothing to compare"))
                    continue
                want = open(path, "rb").read()
                got = sw.dump(base, len(want))
                mm = first_mismatch(got, want)
                if mm is None:
                    rows.append(row(label, True, f"{len(want):,} B at 0x{base:08X} identical to {os.path.relpath(path, ROOT)}", "flash == build"))
                else:
                    rows.append(row(label, False, f"differs from {os.path.relpath(path, ROOT)}: first byte at 0x{base + mm:08X}, {count_diff(got, want):,} B differ "
                                    f"(the board runs a different firmware than this repository's build)", "flash == build"))
        finish("fwcheck", verdict_of(rows), rows, target=tg)

    def lfsproof(cfg, abort):
        from tb.imaging import IMG
        tg = target_of(cfg)
        if not IMG["bytes"]:
            raise RuntimeError("choose the picture on the Images page first: the proof looks for exactly that file")
        data = IMG["bytes"]
        with Swd(tg) as sw:
            flash = sw.dump(LFS_BASE, LFS_PAGES * LFS_PAGE)
        sb = lfs_superblocks(flash)
        rows = [row("LittleFS superblock", bool(sb), (f"'littlefs' magic in block(s) {sb} (pages {[72 + b for b in sb]})" if sb else "no 'littlefs' magic in pages 72..99: no file system there"), "magic at byte 8 of a metadata block")]
        found, total, missing = chunk_hits(flash, data)
        pct = 100.0 * found / total
        rows.append(row("picture bytes in flash", pct >= 95, f"{found} of {total} chunks of {IMG['name']} ({len(data):,} B, sha256 {ctx_sha(data)[:16]}…) found in 0x{LFS_BASE:08X}..0x{LFS_BASE + LFS_PAGES * LFS_PAGE - 1:08X} = {pct:.1f} %"
                        + (f"; missing {missing[:12]}" if missing else ""), ">= 95 % (chunks that straddle a block boundary are interleaved with pointers)"))
        used = sum(1 for p in range(LFS_PAGES) if any(b != 0xFF for b in flash[p * LFS_PAGE:(p + 1) * LFS_PAGE]))
        rows.append(row("flash pages in use", None, f"{used} of {LFS_PAGES} pages are not erased (0xFF)"))
        finish("lfsproof", verdict_of(rows), rows, target=tg, found=found, chunks=total)

    def ctx_sha(b):
        import hashlib
        return hashlib.sha256(b).hexdigest()

    def bootcfg(cfg, abort):
        tg = target_of(cfg)
        rows = []
        elf = PROF._elf(os.path.join(SAT_BUILD if tg == "sat" else GS_BUILD, "satellite.elf" if tg == "sat" else "ground_station.elf"))
        with Swd(tg) as sw:
            srrvr = sw.r32(0x58004084)
            sbrv = srrvr & 0xFFFF
            rows.append(row("CPU2 boot address (FLASH_SRRVR.SBRV)", sbrv == 0xC800, f"SRRVR 0x{srrvr:08X}, SBRV 0x{sbrv:04X} -> CPU2 boots at 0x{0x08000000 + sbrv * 4:08X}", "SBRV 0xC800 (0x08032000)"))
            cr4 = sw.r32(0x5800040C)
            rows.append(row("PWR_CR4.C2BOOT", bool(cr4 & 0x8000), f"PWR_CR4 0x{cr4:08X}", "bit 15 set (CPU1 released CPU2)"))
            vt = sw.dump(0x08032000, 8)
            sp, rv = struct.unpack("<II", vt)
            rows.append(row("CPU2 vector table", 0x20009000 < sp <= 0x20010000 and 0x08032000 <= rv < 0x08040000, f"initial SP 0x{sp:08X}, reset 0x{rv:08X}", "SP in CPU2 RAM, reset in CPU2 flash"))
            for sym, want, label in (("g_sealSelfTest", 0xA5A50001, "integrity-check self-test"), ("g_digiSelfTest", 0xA5A50002, "digipeater rule self-test")):
                addr = elf.syms.get(sym)
                if addr is None:
                    rows.append(row(label, None, f"{sym} is not in this image (feature off)"))
                    continue
                v = sw.r32(addr)
                rows.append(row(label, v == want, f"{sym} = 0x{v:08X}" + (" (never ran)" if v == 0 else ""), f"0x{want:08X}"))
            magic, beat1 = struct.unpack("<II", sw.dump(0x20008F00, 8))
            if magic == 0x4F424331:
                time.sleep(1.2)
                beat2 = sw.r32(0x20008F04)
                rows.append(row("CPU2 heartbeat (OBC block)", beat2 != beat1, f"cpu2_beat {beat1} -> {beat2} in 1.2 s", "advances"))
            else:
                rows.append(row("CPU2 heartbeat (OBC block)", None, f"no OBC block here (magic 0x{magic:08X}): this CPU1 image has no s2s2obc app"))
        finish("bootcfg", verdict_of(rows), rows, target=tg)

    def membudget(cfg, abort):
        tg = target_of(cfg)
        lim = {"flash": float(cfg.get("flash_max_pct", 97)), "ram": float(cfg.get("ram_max_pct", 90)), "stack": float(cfg.get("stack_min_free_pct", 25))}
        PROF.targets = prof_targets()
        lay = PROF.layout(tg)
        rows = []
        for L in lay:
            if not L["elf"]:
                rows.append(row(L["core"], None, "ELF missing: build first"))
                continue
            fl, rm = L["regions"].get("FLASH"), L["regions"].get("RAM")
            if fl:
                p = 100.0 * L["flash_used"] / fl[1]
                rows.append(row(L["core"] + " flash", p <= lim["flash"], f"{L['flash_used']:,} of {fl[1]:,} B = {p:.1f} %", f"<= {lim['flash']:.0f} %"))
            if rm:
                p = 100.0 * L["ram_used"] / rm[1]
                rows.append(row(L["core"] + " static RAM", p <= lim["ram"], f"{L['ram_used']:,} of {rm[1]:,} B = {p:.1f} %", f"<= {lim['ram']:.0f} %"))
            if L["estack"] and L["ebss"] and rm:
                hs = next((x for x in L["sections"] if x["name"] == "._user_heap_stack"), None)
                free_start = (hs["addr"] + hs["size"]) if hs else L["ebss"]
                stack = L["estack"] - free_start
                pct = 100.0 * stack / rm[1]
                rows.append(row(L["core"] + " stack room", stack > 0, f"{stack:,} B between the heap reserve and the top of RAM ({pct:.0f} % of RAM)", "> 0 (live depth: Profiler, arm watermark)"))
        finish("membudget", verdict_of(rows), rows, target=tg, limits=lim)

    def profcap(cfg, abort):
        tg = target_of(cfg)
        secs = max(5, min(600, int(cfg.get("seconds", 30))))
        started = tg not in PROF.sessions
        if started:
            prof_start(tg)
        try:
            end = now() + secs
            while now() < end and not abort.is_set():
                time.sleep(0.5)
            st = PROF.sessions[tg].state()
        finally:
            if started:
                PROF.stop(tg)
        rows = []
        for c in st["cores"]:
            rows.append(row(c["core"], None, f"busy {c['busy_pct']} % over {c['samples']} samples; " + ", ".join(f"{x['fn']} {x['pct']} %" for x in c["lifetime"][:6])))
        rows.append(row("sampling", st["rate"] > 50, f"{st['rate']} PC samples/s, SRAM dump {st['mem_ms']} ms", "> 50 samples/s"))
        finish("profcap", verdict_of(rows), rows, target=tg, seconds=secs, cores=st["cores"])

    # ---- Nucleo only
    def alive(OBC, abort, timeout=8):
        end = now() + timeout
        n = 0x7000
        while now() < end and not abort.is_set():
            n += 1
            mk = OBC.mark()
            OBC.send(OBC_STATUS, n & 0xFFFF)
            st = OBC.wait_ack(OBC_STATUS, n & 0xFFFF, mk, 1.5, abort)
            if st is not None:
                return st
        return None

    def vcpfuzz(cfg, abort):
        if not OBC.connected():
            raise RuntimeError("connect the Nucleo's VCP first")
        rows = []
        st0 = alive(OBC, abort, 60)
        rows.append(row("baseline STATUS", st0 == 0, f"status {st0}", "0 = CPU1 up, CPU2 heartbeat advancing"))
        for name, frame, must_ignore, why in fuzz_cases(obc_frame, crc16_ccitt, OBC_SYNC):
            seq = struct.unpack("<H", frame[10:12])[0] if len(frame) >= 12 and frame[:9] == OBC_SYNC else None
            mk = OBC.mark()
            OBC.ser.write(frame)
            time.sleep(1.0)
            with OBC.lock:
                answered = [a for a in OBC.acks[mk:] if a[1] == OBC_STATUS and (seq is None or a[2] == seq)] if seq is not None else []
            if name == "random garbage 256 B":
                with OBC.lock:
                    answered = list(OBC.acks[mk:])
            st = alive(OBC, abort, 12)
            recovered = st == 0
            if must_ignore:
                rows.append(row(name, (not answered) and recovered, f"{'ANSWERED the bad frame' if answered else 'ignored'}; CPU1 {'alive' if recovered else 'NOT answering afterwards'}", why))
            else:
                rows.append(row(name, None, f"{'answered' if answered else 'ignored'}; CPU1 {'alive' if recovered else 'NOT answering afterwards'}", why))
        # burst of valid frames: how many are answered
        mk = OBC.mark()
        for i in range(50):
            OBC.ser.write(obc_frame(OBC_STATUS, 0x4000 + i))
            time.sleep(0.02)
        time.sleep(4)
        with OBC.lock:
            got = len({a[2] for a in OBC.acks[mk:] if a[1] == OBC_STATUS and 0x4000 <= a[2] < 0x4000 + 50})
        rows.append(row("50 STATUS frames back to back", None, f"{got} of 50 answered (frames sent every 20 ms; CPU1 may drop the ones that arrive while it waits for CPU2)"))
        fin = alive(OBC, abort, 15)
        rows.append(row("alive after the test", fin == 0, f"status {fin}", "0"))
        finish("vcpfuzz", verdict_of(rows), rows)

    def flashio(cfg, abort):
        if not OBC.connected():
            raise RuntimeError("connect the Nucleo's VCP first")
        if not cfg.get("confirm"):
            raise RuntimeError("this test replaces the picture stored in CPU1's flash: tick the confirmation box")
        n = max(4, min(int(cfg.get("chunks", 40)), 340))
        rounds = max(1, min(int(cfg.get("rounds", 2)), 10))
        rows = []
        wr, rd = [], []
        for rnd in range(rounds):
            if abort.is_set():
                break
            mk = OBC.mark()
            OBC.send(OBC_SETNAME, 0, 0, b"fio_test.bin")
            OBC.wait_ack(OBC_SETNAME, 0, mk, 5, abort)
            mk = OBC.mark()
            OBC.send(OBC_ERASE, 0, n)
            if OBC.wait_ack(OBC_ERASE, 0, mk, 30, abort) != 0:
                rows.append(row(f"round {rnd + 1}: create file", False, "CPU1 could not create the file"))
                break
            t0 = now()
            fail = None
            for i in range(n):
                for _ in range(4):
                    mk = OBC.mark()
                    OBC.send(OBC_STORE, i, n, pattern_chunk(i + rnd * 1000))
                    if OBC.wait_ack(OBC_STORE, i, mk, 6, abort) == 0:
                        break
                else:
                    fail = i
                    break
            tw = now() - t0
            if fail is not None:
                rows.append(row(f"round {rnd + 1}: write", False, f"chunk {fail + 1} was never acknowledged"))
                break
            t0 = now()
            bad = []
            for i in range(n):
                s, blk, ok = OBC.read_flash(i, abort)
                if s != 0 or not ok or blk != pattern_chunk(i + rnd * 1000):
                    bad.append(i + 1)
            tr = now() - t0
            wr.append(n / tw)
            rd.append(n / tr)
            rows.append(row(f"round {rnd + 1}: write + read back", not bad, f"{n} chunks written in {tw:.1f} s ({n / tw:.1f}/s), read in {tr:.1f} s ({n / tr:.1f}/s)" + (f"; MISMATCH in chunks {bad[:10]}" if bad else "; every chunk identical"), "identical"))
        if wr:
            rows.append(row("throughput", None, f"write {statistics.mean(wr) * 128 / 1024:.2f} KiB/s, read {statistics.mean(rd) * 128 / 1024:.2f} KiB/s over the VCP incl. flash programming"))
        finish("flashio", verdict_of(rows), rows, chunks=n, rounds=rounds)

    # ---- link (GS board + satellite)
    def gs_events_since(t0, kinds=("ack", "nack", "hk", "beacon", "cw", "rssi")):
        return [e for e in EVENTS.recent(limit=800) if e["t"] >= t0 and e["kind"] in kinds]

    def cmdconf(cfg, abort):
        spec = [("health", "HEALTH SAT (HK1)", True), ("hk2", "Housekeeping 2 (HK2)", True), ("ping", "PING", False), ("hkdl", "Stored housekeeping download", False), ("adcs", "ADCS telecommand", False), ("epdm", "EPDM telecommand", False), ("bad", "Invalid opcode", None)]
        rows = []
        for cid, label, need_hk in spec:
            if abort.is_set():
                break
            t0 = now()
            r = run_command(cid, {"count": 1}, abort, attempts=8, every=14.0)
            ev = r.get("events", [])
            acc = next((e for e in ev if e["kind"] == "ack" and e["what"] == "accepted"), None)
            com = next((e for e in ev if e["kind"] == "ack" and e["what"] == "completed"), None)
            hk = any(e["kind"] == "hk" for e in ev)
            nack = next((e for e in ev if e["kind"] == "nack"), None)
            lat = lambda e: f"{e['t'] - t0:.1f} s" if e else "–"
            if cid == "bad":
                ok = bool(nack) and nack.get("err") == 9
                rows.append(row(label, ok, f"NACK {('0x%02X %s' % (nack['err'], nack['name'])) if nack else 'none'} after {lat(nack)}", "NACK 0x09 invalid command"))
            else:
                ok = r["verdict"] == "completed" and (hk or not need_hk)
                rows.append(row(label, ok, f"{r['verdict']}; ACK accepted {lat(acc)}, completed {lat(com)}{'; housekeeping packet' if hk else ''}; {r.get('attempts', '?')} send(s)", "ACK accepted -> data -> ACK completed" if need_hk else "ACK accepted -> ACK completed"))
            time.sleep(3)
        finish("cmdconf", verdict_of(rows), rows)

    def nackmatrix(cfg, abort):
        cases = [("invalid opcode", "53 01 FF FF FF", True, "NACK 0x09"), ("unknown destination MCU", "53 7F 1D D1 F2", False, "observed"),
                 ("valid opcode + garbage tail", "53 01 1D D1 F2 DE AD BE EF 11 22 33 44", False, "observed"), ("all zero body", "53 00 00 00 00", False, "observed"),
                 ("wrong header byte", "54 01 1D D1 F2", False, "observed (probably silence)")]
        rows = []
        for name, hexs, expect_nack, exp in cases:
            if abort.is_set():
                break
            body = (hexs + " 00" * 13).split()[:13]
            t0 = now()
            seen = []
            for attempt in range(1, 7):
                GS.send("HEX " + " ".join(body))
                end = now() + 14
                while now() < end and not abort.is_set():
                    seen = gs_events_since(t0, ("ack", "nack"))
                    if seen:
                        break
                    time.sleep(0.3)
                if seen:
                    break
            nack = next((e for e in seen if e["kind"] == "nack"), None)
            acks = [e for e in seen if e["kind"] == "ack"]
            what = (f"NACK 0x{nack['err']:02X} {nack['name']}" if nack else (f"ACK {acks[0]['what']}" if acks else f"silence after {attempt} send(s)"))
            if expect_nack:
                rows.append(row(name, bool(nack) and nack["err"] == 9, what, exp))
            else:
                rows.append(row(name, None, what, exp))
            time.sleep(4)
        finish("nackmatrix", verdict_of(rows), rows)

    def beacontime(cfg, abort):
        secs = max(120, min(7200, int(cfg.get("seconds", 600))))
        c = CORE.load_config()
        period = float(cfg.get("period_s") or c.get("beacon_period_s", 90))
        tol = float(cfg.get("tol_s", 4))
        t0 = now()
        end = t0 + secs
        while now() < end and not abort.is_set():
            BUS.publish("progress", test="beacontime", done=round(now() - t0), total=secs)
            time.sleep(2)
        ev = gs_events_since(t0, ("beacon", "cw", "rssi"))
        bt = [e["t"] for e in ev if e["kind"] == "beacon"]
        ok, st, why = check_beacon_period(bt, period, tol, secs)
        want = f"{c.get('project', 'S2S2')} {c.get('sat_call', '9N2SJ')}"
        texts = [e.get("text", "") for e in ev if e["kind"] == "cw"]
        call_ok = bool(texts) and all(want in t for t in texts)
        rs = [e["rssi"] for e in ev if e["kind"] == "rssi"]
        rows = [row("beacon period", ok, f"{st['n']} beacons decoded (expected about {st['expected_n']}); gaps {st.get('gaps', [])}" + (f"; mean {st['mean']} s, std {st['std']} s" if "mean" in st else "") + (f". {why}" if why else ""), f"{period:.0f} s +-{tol:.0f} s"),
                row("callsign text", call_ok, f"{len(texts)} CW text(s): {sorted(set(texts))[:3]}", f"contains '{want}'"),
                row("signal", None, f"{len(rs)} received packets" + (f", RSSI mean {statistics.mean(rs):.0f} dBm, min {min(rs)}, max {max(rs)}" if rs else ""))]
        finish("beacontime", verdict_of(rows), rows, period=period, seconds=secs, times=bt)

    def winmap(cfg, abort):
        c = CORE.load_config()
        P = float(cfg.get("period_s") or c.get("beacon_period_s", 90))
        step = max(8.0, float(cfg.get("step_s", 10)))
        sweeps = max(1, min(int(cfg.get("sweeps", 2)), 6))
        log(f"window map: waiting for a CW beacon to anchor the {P:.0f} s cycle ...")
        anchor = None
        end = now() + 4 * P
        t_start = now()
        while now() < end and not abort.is_set():
            b = [e["t"] for e in gs_events_since(t_start, ("beacon",))]
            if b:
                anchor = b[-1]
                break
            time.sleep(1)
        if anchor is None:
            raise RuntimeError("no CW beacon decoded: cannot anchor the cycle")
        probes = []
        offsets = [i * step for i in range(int(P // step))]
        for sw_i in range(sweeps):
            for off in offsets:
                if abort.is_set():
                    break
                b = [e["t"] for e in gs_events_since(t_start, ("beacon",))]
                if b:
                    anchor = b[-1]
                phase = (now() - anchor) % P
                wait = (off - phase) % P
                end_w = now() + wait
                while now() < end_w and not abort.is_set():
                    time.sleep(0.2)
                t0 = now()
                GS.send("PING")
                ok, lat = False, None
                while now() < t0 + 7 and not abort.is_set():
                    a = [e for e in gs_events_since(t0, ("ack", "nack")) if e["kind"] == "ack"]
                    if a:
                        ok, lat = True, a[0]["t"] - t0
                        break
                    time.sleep(0.2)
                probes.append({"offset": off, "ok": ok, "lat": lat, "sweep": sw_i + 1})
                BUS.publish("progress", test="winmap", done=len(probes), total=len(offsets) * sweeps)
                time.sleep(6)                            # let the answer frames finish before the next probe
        w = estimate_window([(p["offset"], p["ok"]) for p in probes])
        rows = [row(f"offset {p['offset']:.0f} s after the beacon (sweep {p['sweep']})", p["ok"], f"{'ACK after %.1f s' % p['lat'] if p['ok'] else 'no answer'}") for p in probes]
        for r_ in rows:
            r_["state"] = "PASS" if r_["state"] == "PASS" else "INFO"
        summary = row("listen window", bool(w) and w[2] >= 20, (f"answers from {w[0]:.0f} s to {w[1]:.0f} s after the beacon ({w[2]:.0f} s long, of a {P:.0f} s cycle)" if w else "no offset was answered in every sweep"), ">= 20 s (firmware: about 50 s after the CW)")
        finish("winmap", "PASS" if summary["state"] == "PASS" else "FAIL", [summary] + rows, probes=probes, period=P, step=step)

    def soak(cfg, abort):
        minutes = max(2, min(int(cfg.get("minutes", 30)), 720))
        t_end = now() + minutes * 60
        series = []
        n = 0
        while now() < t_end and not abort.is_set():
            n += 1
            t0 = now()
            r = run_command("ping", {}, abort, attempts=7, every=14.0)
            rs = [e["rssi"] for e in gs_events_since(t0, ("rssi",))]
            series.append({"n": n, "t": t0, "ok": r["verdict"] == "completed", "verdict": r["verdict"], "seconds": r.get("seconds"), "attempts": r.get("attempts"), "rssi": (sum(rs) / len(rs)) if rs else None})
            BUS.publish("progress", test="soak", done=round(now() - (t_end - minutes * 60)), total=minutes * 60)
            wait_until = t0 + float(cfg.get("interval_s", 90))
            while now() < wait_until and now() < t_end and not abort.is_set():
                time.sleep(0.5)
        okn = sum(1 for s in series if s["ok"])
        lat = [s["seconds"] for s in series if s["ok"] and s["seconds"] is not None]
        rs = [s["rssi"] for s in series if s["rssi"] is not None]
        rate = 100.0 * okn / len(series) if series else 0
        rows = [row("PING success", rate >= 95, f"{okn} of {len(series)} completed ({rate:.0f} %)", ">= 95 %"),
                row("latency to completion", None, (f"mean {statistics.mean(lat):.1f} s, max {max(lat):.1f} s, 95th percentile {sorted(lat)[int(0.95 * (len(lat) - 1))]:.1f} s" if lat else "no completed PING")),
                row("sends needed per PING", None, f"mean {statistics.mean([s['attempts'] or 0 for s in series]):.1f}" if series else ""),
                row("RSSI", None, (f"mean {statistics.mean(rs):.0f} dBm, min {min(rs):.0f}, max {max(rs):.0f}" if rs else "no RSSI lines"))]
        finish("soak", verdict_of(rows), rows, series=series, minutes=minutes)

    T.update({"lfsproof": lfsproof, "fwcheck": fwcheck, "bootcfg": bootcfg, "membudget": membudget, "profcap": profcap, "vcpfuzz": vcpfuzz, "flashio": flashio,
              "cmdconf": cmdconf, "nackmatrix": nackmatrix, "beacontime": beacontime, "winmap": winmap, "soak": soak})
    return T
