#!/usr/bin/env python3
"""S2S2 test bench -- a local web GUI for proving the satellite <-> ground-station link.

  tools/.venv/bin/python tools/testbench/server.py        # then open http://127.0.0.1:8765

Two serial ports, no SWD and no probe serial numbers needed to run tests:
  GS   /dev/cu.usbserial-*   the ground station's console (FTDI)
  SAT  /dev/cu.usbmodem*     the Nucleo's ST-Link virtual COM port (LPUART1, CPU2 firmware)

Tests (one at a time):
  link   GS -> satellite PING x N: ACK / reply / ACK-completed come back, RSSI, latency.
  image  The PC hands the Nucleo the file chunk by chunk over the VCP (OBC link frames, see
         cpu2/satellite/ipc/obc_shared.h). For each chunk the Nucleo transmits it over RF and
         then answers  A5 5A 01 <seq> <status> 0D 0A.  The GS prints every chunk it receives as JSON;
         the bench checks that the GS got that chunk (re-sending it if not), rebuilds the file from
         the GS output and compares SHA-256 with the source.
  sdr    RTL-SDR -> rtl_fm -> Direwolf (G3RUH 4800) while the GS pings the satellite.
Every run can be saved as a proof report (JSON + HTML + both files + the raw GS and satellite logs).
"""
import argparse
import base64
import collections
import datetime
import glob
import hashlib
import html
import io
import json
import math
import os
import queue
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

import saf_tool                                   # tnc2_to_body (same directory)

try:
    import serial
except ImportError:                     # pragma: no cover
    serial = None
try:
    from PIL import Image, ImageFile, ImageOps
    ImageFile.LOAD_TRUNCATED_IMAGES = True      # a cut-off JPEG/PNG still gives a picture instead of an error
    Image.MAX_IMAGE_PIXELS = 400_000_000        # big panoramas / scans are fine (default limit is 89 MP)
except ImportError:                     # pragma: no cover
    Image = None
try:                                    # iPhone / Mac photos are HEIC; optional extra (pip install pillow-heif)
    import pillow_heif
    pillow_heif.register_heif_opener()
except ImportError:                     # pragma: no cover
    pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SAT_DIR = os.path.join(ROOT, "cpu2", "satellite")
GS_DIR = os.path.join(ROOT, "cpu2", "ground_station")
REPORT_DIR = os.path.join(ROOT, "docs", "reports")
TOOLCHAIN = "/Applications/ArmGNUToolchain/15.3.rel1/arm-none-eabi/bin"

CHUNK = 128
GS_BUF_MAX = 10240                     # GS reassembly buffer (gs_cam_buf) -- only matters for "fit to GS"
OBC_SYNC = b"\xC5" * 8 + b"\xA7"
OBC_CHUNK, OBC_STATUS, OBC_RESET, OBC_ERASE, OBC_STORE, OBC_SENDF, OBC_READF, OBC_SETNAME, OBC_INFO = 1, 2, 3, 4, 5, 6, 7, 8, 9
OBC_RATE = 10        # raw air-rate test: seq = gap ms between packets, total = packet count
OBC_CWTEST = 11      # continuous CW "HELLO" for a spectrum analyzer: seq = seconds (0 = stop)
OBC_SAF = 12         # store-and-forward journal: seq 0 = status line, n = record n, 0xFFFF = empty the journal
OBC_INJECT = 13      # hand the satellite's digipeater a frame body as if it had been received over the air
FLASH_BYTES = 44 * 1024                # LittleFS on CPU1 flash pages 72..99 = 56 KB; leave room for copy-on-write
FLASH_CHUNKS = (FLASH_BYTES + 127) // 128

DEFAULTS = {
    "gs_port": "",                     # empty = first /dev/cu.usbserial-*
    "gs_baud": 115200,
    "sat_probe": "002E00354741500120383733",   # only used for build/flash
    "gs_probe": "303030303030303030303031",
    "freq_mhz": 435.000,
    "sdr_gain": 40,
}


def now():
    return time.time()


def sha256(b):
    return hashlib.sha256(b).hexdigest()


# --------------------------------------------------------------------------------------
# event bus (SSE) + shared state
# --------------------------------------------------------------------------------------
class Bus:
    def __init__(self):
        self.lock = threading.Lock()
        self.subs = []
        self.hist = collections.deque(maxlen=3000)

    def publish(self, kind, **kw):
        ev = {"t": round(now(), 3), "kind": kind}
        ev.update(kw)
        with self.lock:
            if kind != "chunk":
                self.hist.append(ev)
            subs = list(self.subs)
        for q in subs:
            try:
                q.put_nowait(ev)
            except queue.Full:
                pass

    def subscribe(self):
        q = queue.Queue(maxsize=5000)
        with self.lock:
            for ev in list(self.hist)[-300:]:
                q.put_nowait(ev)
            self.subs.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.subs:
                self.subs.remove(q)


BUS = Bus()


def log(msg, level="info"):
    BUS.publish("log", level=level, msg=msg)


# --------------------------------------------------------------------------------------
# ground-station serial link
# --------------------------------------------------------------------------------------
class GSLink:
    def __init__(self):
        self.ser = None
        self.port = None
        self.lines = []                # (t, text)
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.thread = None

    def connected(self):
        return self.ser is not None and self.ser.is_open

    def connect(self, port="", baud=115200):
        if serial is None:
            raise RuntimeError("pyserial missing")
        self.disconnect()
        if not port:
            cands = sorted(glob.glob("/dev/cu.usbserial-*"))
            if not cands:
                raise RuntimeError("no /dev/cu.usbserial-* port found (is the FTDI plugged in?)")
            port = cands[0]
        self.ser = serial.Serial(port, baud, timeout=0.05)
        self.port = port
        self.stop.clear()
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()
        log(f"GS serial open: {port} @ {baud}")

    def disconnect(self):
        self.stop.set()
        if self.thread:
            self.thread.join(1.0)
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

    def _reader(self):
        buf = b""
        while not self.stop.is_set():
            try:
                d = self.ser.read(4096)
            except Exception as e:     # port vanished
                log(f"GS serial error: {e}", "err")
                break
            if not d:
                continue
            buf += d
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                txt = raw.decode(errors="replace").replace("\r", "")
                if not txt.strip():
                    continue
                with self.lock:
                    self.lines.append((now(), txt))
                BUS.publish("gs", msg=txt[:300])

    def mark(self):
        with self.lock:
            return len(self.lines)

    def since(self, mark):
        with self.lock:
            return list(self.lines[mark:])

    def send(self, text):
        if not self.connected():
            raise RuntimeError("GS serial not connected")
        self.ser.write((text + "\r\n").encode())
        log(f"GS <- {text}")

    def wait(self, pattern, since, timeout, abort=None):
        """Wait for a regex in lines after `since`. Returns (line, t) or (None, None)."""
        rx = re.compile(pattern)
        end = now() + timeout
        seen = since
        while now() < end:
            if abort and abort.is_set():
                return None, None
            with self.lock:
                new = self.lines[seen:]
                seen = len(self.lines)
            for t, txt in new:
                if rx.search(txt):
                    return txt, t
            time.sleep(0.03)
        return None, None


GS = GSLink()


# --------------------------------------------------------------------------------------
# Nucleo ST-Link VCP: OBC link frames + the satellite's own log
# --------------------------------------------------------------------------------------
def crc16_ccitt(d, c=0xFFFF):
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def obc_frame(cmd, seq=0, total=0, data=b""):
    """PC -> CPU1 frame: C5 x8, A7, cmd, seq(2 LE), total(2 LE), len, data, crc16(2 LE) over cmd..data."""
    body = bytes([cmd]) + struct.pack("<HH", seq, total) + bytes([len(data)]) + data
    return OBC_SYNC + body + struct.pack("<H", crc16_ccitt(body))


class ObcLink:
    """The Nucleo's ST-Link virtual COM port. Carries the satellite's text log (sat -> PC) and the
    binary OBC frames (PC -> sat) with their  A5 5A cmd seq16 status 0D 0A  answers."""

    def __init__(self):
        self.ser = None
        self.port = None
        self.lock = threading.Lock()
        self.acks = []                 # (t, cmd, seq, status)
        self.flashdata = {}            # seq -> (t, bytes, crc_ok)  (READF answers)
        self.lines = []                # (t, text)
        self.stop = threading.Event()
        self.thread = None

    def connected(self):
        return self.ser is not None and self.ser.is_open and getattr(self, "ok", True)

    @staticmethod
    def ports():
        return sorted(glob.glob("/dev/cu.usbmodem*"))

    @staticmethod
    def _open_quiet(port, baud):
        """Open the ST-Link virtual COM port WITHOUT asserting DTR / RTS. A normal open pulses those lines, and on this
        board that resets the whole STM32 (both cores). Opened by the bench's auto-reconnect it formed a loop: reset ->
        the port glitches -> reconnect -> reset ..., one reset every ~4.5 s (found with SWD: the CPU1 and CPU2 clocks
        restarted together; stopping the bench stopped the resets)."""
        s = serial.Serial()
        s.port = port
        s.baudrate = baud
        s.timeout = 0.02
        s.dtr = False
        s.rts = False
        s.open()
        return s

    def connect(self, port="", baud=115200):
        self.disconnect()
        if not port:
            c = self.ports()
            if not c:
                raise RuntimeError("no /dev/cu.usbmodem* port: plug the Nucleo's ST-Link USB cable in")
            port = c[0]
        self.ser = self._open_quiet(port, baud)
        self.port = port
        self.baud = baud
        self.ok = True
        self.stop.clear()
        self.thread = threading.Thread(target=self._reader, daemon=True)
        self.thread.start()
        log(f"Nucleo VCP open: {port} @ {baud}")

    def disconnect(self):
        self.stop.set()
        if self.thread:
            self.thread.join(1.0)
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None

    def _reader(self):
        buf = b""
        pat = re.compile(rb"\xA5[\x5A\x5C]")
        while not self.stop.is_set():
            try:
                d = self.ser.read(4096)
            except Exception as e:
                log(f"Nucleo VCP error: {e}", "err")
                if not self._reconnect():
                    break
                buf = b""
                continue
            if not d:
                continue
            buf += d
            while True:
                m = pat.search(buf)
                if m:
                    i = m.start()
                    if buf[i + 1] == 0x5A:                                   # A5 5A cmd seq16 status 0D 0A
                        if len(buf) < i + 8:
                            self._text(buf[:i]); buf = buf[i:]; break
                        if buf[i + 6:i + 8] == b"\r\n":
                            ack = buf[i:i + 8]
                            self._text(buf[:i]); buf = buf[i + 8:]
                            with self.lock:
                                self.acks.append((now(), ack[2], ack[3] | (ack[4] << 8), ack[5]))
                            continue
                    else:                                                      # A5 5C seq16 len data crc16 0D 0A
                        if len(buf) < i + 5:
                            self._text(buf[:i]); buf = buf[i:]; break
                        ln = buf[i + 4]
                        end = i + 5 + ln + 4
                        if len(buf) < end:
                            self._text(buf[:i]); buf = buf[i:]; break
                        if buf[end - 2:end] == b"\r\n":
                            seq = buf[i + 2] | (buf[i + 3] << 8)
                            body = buf[i + 2:i + 5 + ln]
                            ok = crc16_ccitt(body) == (buf[end - 4] | (buf[end - 3] << 8))
                            self._text(buf[:i]); data = buf[i + 5:i + 5 + ln]; buf = buf[end:]
                            with self.lock:
                                self.flashdata[seq] = (now(), data, ok)
                            continue
                    self._text(buf[:i + 1]); buf = buf[i + 1:]               # not a frame: plain bytes
                    continue
                j = buf.rfind(b"\n")
                if j >= 0:
                    self._text(buf[:j + 1]); buf = buf[j + 1:]
                break
            if len(buf) > 8192:
                buf = buf[-64:]

    def _reconnect(self, timeout=120.0):
        """The ST-Link's USB serial port can drop out for a few seconds mid-transfer (the Nucleo itself keeps running:
        CPU1/CPU2 are not affected). Keep trying to re-open it -- same name, or any /dev/cu.usbmodem* -- so the test
        carries on instead of crashing. Returns False if the port never comes back or disconnect() was called."""
        self.ok = False
        try:
            self.ser.close()
        except Exception:
            pass
        end = now() + timeout
        while not self.stop.is_set() and now() < end:
            for cand in dict.fromkeys([self.port] + self.ports()):
                try:
                    self.ser = self._open_quiet(cand, self.baud)
                    self.port = cand
                    self.ok = True
                    log(f"Nucleo VCP back: {cand} (it had dropped out for a moment; the board kept running)", "warn")
                    return True
                except Exception:
                    pass
            time.sleep(3.0)                 # never re-open faster than this
        log("Nucleo VCP did not come back", "err")
        return False

    def _text(self, b):
        for ln in b.decode(errors="replace").replace("\r", "").split("\n"):
            if ln.strip():
                with self.lock:
                    self.lines.append((now(), ln))
                BUS.publish("sat", msg=ln[:300])

    def mark(self):
        with self.lock:
            return len(self.acks)

    def send(self, cmd, seq=0, total=0, data=b""):
        if not self.connected() and not getattr(self, "ok", True):
            self._wait_back()                       # port dropped out a moment ago: the reader is re-opening it
        if not self.connected():
            raise RuntimeError("Nucleo VCP not connected")
        frame = obc_frame(cmd, seq, total, data)
        try:
            self.ser.write(frame)
        except Exception as e:                      # e.g. [Errno 6] Device not configured: USB serial dropped out
            log(f"Nucleo VCP write failed ({e}); waiting for the port to come back", "warn")
            self.ok = False
            self._wait_back()
            self.ser.write(frame)                   # every OBC command is safe to repeat (STORE/SENDF are by sequence number)

    def _wait_back(self, timeout=120.0):
        end = now() + timeout
        while not getattr(self, "ok", True) and now() < end and not self.stop.is_set():
            time.sleep(0.2)
        if not getattr(self, "ok", True):
            raise RuntimeError("Nucleo VCP did not come back")

    def wait_ack(self, cmd, seq, since, timeout, abort=None):
        end = now() + timeout
        seen = since
        while now() < end:
            if abort and abort.is_set():
                return None
            with self.lock:
                new = self.acks[seen:]
                seen = len(self.acks)
            for t, c, s, st in new:
                if c == cmd and s == seq:
                    return st
            time.sleep(0.01)
        return None

    def read_flash(self, seq, abort=None, timeout=6):
        """READF: the Nucleo reads chunk `seq` out of its internal flash and sends the bytes back."""
        with self.lock:
            self.flashdata.pop(seq, None)
        mk = self.mark()
        self.send(OBC_READF, seq)
        st = self.wait_ack(OBC_READF, seq, mk, timeout, abort)
        end = now() + 1.0
        while now() < end:
            with self.lock:
                v = self.flashdata.get(seq)
            if v:
                return st, v[1], v[2]
            time.sleep(0.01)
        return st, None, False

    def info(self, abort=None, timeout=5):
        """INFO: CPU1 reports the file it holds on its LittleFS (name, size, flash pages)."""
        with self.lock:
            self.flashdata.pop(0, None)
        mk = self.mark()
        self.send(OBC_INFO, 0)
        st = self.wait_ack(OBC_INFO, 0, mk, timeout, abort)
        with self.lock:
            v = self.flashdata.get(0)
        return v[1].decode(errors="replace") if v else None

    def ready(self, abort, timeout=180):
        """Ping with STATUS until the satellite answers (it is deaf while keying its CW beacon)."""
        end = now() + timeout
        n = 0
        while now() < end and not (abort and abort.is_set()):
            mk = self.mark()
            n += 1
            self.send(OBC_STATUS, n & 0xFFFF)
            if self.wait_ack(OBC_STATUS, n & 0xFFFF, mk, 1.5, abort) == 0:      # 0 = CPU1 up and CPU2's heartbeat advancing
                return True
        return False


OBC = ObcLink()


def sat_fresh_start(abort):
    """Make the satellite forget the replay floor (the GS counter restarts when the GS reboots)."""
    if not OBC.connected():
        log("fresh start skipped: Nucleo VCP not connected", "warn")
        return False
    log("waiting for CPU1 to answer on the VCP and for CPU2's heartbeat ...")
    if not OBC.ready(abort):
        log("satellite did not answer on the VCP", "err")
        return False
    mk = OBC.mark()
    OBC.send(OBC_RESET, 1)
    # CPU1 itself waits up to 3 s for CPU2 to acknowledge; the bench must wait longer than that, or it gives up
    # at the same instant and logs a false "RESET not acknowledged"
    ok = OBC.wait_ack(OBC_RESET, 1, mk, 6, abort) == 0
    log("satellite reset (replay floor cleared)" if ok else "RESET not acknowledged", "ok" if ok else "warn")
    return ok


# --------------------------------------------------------------------------------------
# image preparation
# --------------------------------------------------------------------------------------
IMG = {"name": None, "bytes": None, "orig": None, "info": "", "fit": "none"}


def kind_of(b):
    """(extension, mime type) from the magic bytes of an image file."""
    if b[:3] == b"\xff\xd8\xff":
        return "jpg", "image/jpeg"
    if b[:8] == b"\x89PNG\r\n\x1a\n":
        return "png", "image/png"
    if b[:4] == b"GIF8":
        return "gif", "image/gif"
    return "bin", "application/octet-stream"


def fit_jpeg(raw, name, max_bytes, max_w=320):
    """Re-encode as the best-looking JPEG that is <= max_bytes (largest size/quality that fits)."""
    if raw[:2] == b"\xff\xd8" and len(raw) <= max_bytes and raw.rstrip(b"\0")[-2:] == b"\xff\xd9":
        return raw, f"{name}: JPEG already fits ({len(raw)} B)"
    # (a JPEG without its end marker is a cut-off download: re-encode it below instead of sending a broken picture)
    if Image is None:
        raise RuntimeError("Pillow missing: pip install pillow")
    try:
        im = Image.open(io.BytesIO(raw))
        im.load()
    except Exception as e:
        raise RuntimeError(f"{name}: cannot read this as an image ({e}). Use JPEG, PNG, GIF, BMP, WebP"
                           f"{', HEIC' if 'pillow_heif' in globals() else ' (pip install pillow-heif for HEIC)'}") from e
    im = ImageOps.exif_transpose(im)             # phone photos: honour the EXIF rotation instead of sending them sideways
    if im.mode in ("RGBA", "LA", "P"):
        bg = Image.new("RGB", im.size, (255, 255, 255))
        im = im.convert("RGBA")
        bg.paste(im, mask=im.split()[-1])
        im = bg
    else:
        im = im.convert("RGB")
    w, h = im.size
    for scale in (1.0, 0.85, 0.7, 0.55, 0.4, 0.3, 0.2):
        tw = max(16, int(min(max_w, w) * scale))
        th = max(16, int(tw * h / w))
        r = im.resize((tw, th), Image.LANCZOS)
        for q in (90, 85, 75, 65, 55, 45, 35, 25):
            b = io.BytesIO()
            r.save(b, "JPEG", quality=q, optimize=True)
            if b.tell() <= max_bytes:
                return b.getvalue(), f"{name}: compressed to JPEG {tw}x{th} q{q} ({b.tell()} B, was {len(raw)} B)"
    raise RuntimeError("could not shrink the image under %d B" % max_bytes)


def set_image(raw, name, mode="flash"):
    """mode: none = send the file byte for byte; flash = compress to fit the 64 KB OBC flash;
    quick = small <=10 KB JPEG for a fast demo."""
    mode = {True: "quick", False: "none"}.get(mode, mode) or "flash"
    if mode == "flash":
        data, info = fit_jpeg(raw, name, FLASH_BYTES - 1024, 480)
    elif mode == "quick":
        data, info = fit_jpeg(raw, name, GS_BUF_MAX - 64, 320)
    else:
        data, info = raw, f"{name}: sent as-is, {len(raw)} B (no conversion)"
    IMG.update(name=name, bytes=data, orig=raw, info=info, fit=mode, rx=None)
    log(info + f", {math.ceil(len(data) / CHUNK)} chunks, sha256 {sha256(data)[:16]}…")
    return info


# --------------------------------------------------------------------------------------
# tests
# --------------------------------------------------------------------------------------
RUN = {"lock": threading.Lock(), "thread": None, "abort": threading.Event(), "name": None}
STATE = {"link": None, "image": None, "sdr": None, "rate": None, "cwtest": None, "sdrimage": None, "imageloop": None, "digi": None, "image_rx": None, "gs_log": []}
PROG = {"total": 0, "acked": set(), "got": set(), "stored": set(), "read": set(), "phase": "", "mode": "", "led": None}


def set_result(name, res):
    """Remember the last result of a test for the page and for the report."""
    STATE[name] = res
    BUS.publish("result", name=name, result={k: v for k, v in res.items() if k not in ("rx_bytes", "raw_frames")})


ACK_RX = r"ACK ACCEPTED|ACK Accepted"
CAM_START_RX = r'Starting camera download|"type":"CAMERA"'


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


class LedTrace:
    """Samples the Nucleo's real LED GPIO outputs (GPIOB ODR: PB15 blue, PB9 green, PB11 red) over SWD,
    independently of the firmware's own messages. Needs the ST-Link serial of the satellite Nucleo."""

    NAMES = {"B--": "BLUE", "-G-": "GREEN", "--R": "RED", "-GR": "YELLOW", "BGR": "WHITE", "BG-": "CYAN",
             "B-R": "MAGENTA", "---": "off"}

    def __init__(self, probe):
        self.probe = probe
        self.proc = None
        self.sk = None
        self.trace = []                # (abs time, "BGR"-style string)
        self.run = False
        self.thread = None

    def start(self):
        s_ = socket.socket()
        s_.bind(("127.0.0.1", 0))
        port = s_.getsockname()[1]
        s_.close()
        self.proc = subprocess.Popen(
            ["openocd", "-f", "interface/stlink.cfg", "-c", "transport select hla_swd",
             "-c", f"hla_serial {self.probe}", "-f", "target/stm32wlx.cfg", "-c", "reset_config none",
             "-c", f"tcl_port {port}", "-c", "gdb_port disabled", "-c", "telnet_port disabled", "-c", "init"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        for _ in range(60):
            if self.proc.poll() is not None:
                raise RuntimeError("openocd exited (is another tool holding the ST-Link?)")
            try:
                self.sk = socket.create_connection(("127.0.0.1", port), timeout=2)
                break
            except OSError:
                time.sleep(0.2)
        else:
            raise RuntimeError("openocd tcl port did not come up")
        self.sk.settimeout(3)
        self.run = True
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()
        log(f"LED trace: sampling GPIOB over SWD (probe {self.probe})")

    def _cmd(self, c):
        self.sk.sendall(c.encode() + b"\x1a")
        d = b""
        while not d.endswith(b"\x1a"):
            d += self.sk.recv(4096)
        return d[:-1].decode().strip()

    def _loop(self):
        last = None
        while self.run:
            try:
                v = int(self._cmd("read_memory 0x48000414 32 1"), 16)
            except Exception:
                time.sleep(0.2)
                continue
            col = ("B" if v & (1 << 15) else "-") + ("G" if v & (1 << 9) else "-") + ("R" if v & (1 << 11) else "-")
            if col != last:
                self.trace.append((now(), col))
                last = col

    def stop(self):
        self.run = False
        if self.thread:
            self.thread.join(2)
        for x in (self.sk,):
            try:
                x.close()
            except Exception:
                pass
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(3)
            except Exception:
                self.proc.kill()

    def colour_fraction(self, t0, t1, want):
        """Fraction of the window [t0,t1] during which the LED pattern was in `want` (a set of patterns)."""
        if t1 <= t0 or not self.trace:
            return None
        tot = 0.0
        tr = self.trace + [(t1, "")]
        for (ta, c), (tb, _) in zip(tr, tr[1:]):
            a, b = max(ta, t0), min(tb, t1)
            if b > a and c in want:
                tot += b - a
        return round(tot / (t1 - t0), 3)

    def summary(self, phases):
        dur = collections.Counter()
        tr = self.trace
        for (ta, c), (tb, _) in zip(tr, tr[1:] + [(tr[-1][0], "")] if tr else []):
            dur[self.NAMES.get(c, c)] += tb - ta
        want = {"write": {"B--", "BG-", "B-R", "BGR"}, "read back": {"-G-", "BG-", "BGR"},
                "CPU1 reads": {"-G-", "-GR", "BGR"}, "transmit": {"-GR", "BGR", "--R"}}
        check = {}
        for name, (t0, t1) in phases.items():
            key = next((k for k in want if name.startswith(k)), None)
            if key:
                check[name] = {"expected": sorted(self.NAMES.get(w, w) for w in want[key]),
                               "seen_fraction": self.colour_fraction(t0, t1, want[key])}
        return {"samples_transitions": len(tr), "seconds_in_colour": {k: round(v, 1) for k, v in dur.items()},
                "phase_check": check}


class Sdr:
    def __init__(self):
        self.proc = None
        self.frames = []
        self.levels = []
        self.lines = []

    def present(self):
        """Is an RTL-SDR on USB? Read from the USB registry; the dongle is not opened (opening it from a status refresh
        can disturb a running capture)."""
        names = [n for n, _ in _usb_devices()]
        hit = [n for n in names if "RTL2832" in n or "RTL2838" in n or "RTL-SDR" in n.upper()]
        if hit:
            return True, [f"{hit[0]} is on USB (not opened; the tuner is checked when a test starts)"]
        return False, ["no RTL-SDR on USB: plug the dongle in (directly in the Mac or a powered hub)"]

    def check_tuner(self):
        """Open the dongle once (rtl_test): used only right before a test starts."""
        try:
            r = subprocess.run(["rtl_test", "-t"], capture_output=True, text=True, timeout=12)
            out = (r.stdout + r.stderr)
            return ("Found" in out and "No supported devices" not in out), out.strip().splitlines()[:4]
        except FileNotFoundError:
            return False, ["rtl_test not installed (brew install librtlsdr)"]
        except subprocess.TimeoutExpired:
            return False, ["rtl_test timed out (SDR stalled: unplug and replug it)"]

    def start(self, freq_mhz, gain):
        conf = os.path.join(ROOT, "tools", "testbench", "direwolf_s2s2_4800.conf")
        cmd = (f"rtl_fm -f {freq_mhz:.6f}M -s 48000 -g {gain} 2>/dev/null | "
               f"direwolf -c '{conf}' -r 48000 -n 1 -B 4800 -g -t 0 -")
        self.frames, self.levels, self.lines = [], [], []
        self.proc = subprocess.Popen(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     text=True, bufsize=1, preexec_fn=os.setsid)
        threading.Thread(target=self._read, daemon=True).start()
        log(f"SDR: rtl_fm {freq_mhz} MHz gain {gain} -> Direwolf (G3RUH 4800)")

    def _read(self):
        for line in self.proc.stdout:
            line = line.rstrip()
            self.lines.append(line)
            BUS.publish("sdr", msg=line[:300])
            m = re.search(r"\]\s*([A-Z0-9-]+)>([A-Z0-9-]+)", line)
            if m and "9N2SJ" in line + m.group(1):
                self.frames.append({"t": now(), "line": line})
                BUS.publish("log", level="ok", msg=f"Direwolf decoded: {line[:120]}")
            a = re.search(r"Audio level = (\d+)", line)
            if a:
                self.levels.append(int(a.group(1)))

    def stop(self):
        if self.proc and self.proc.poll() is None:
            _stop_pipeline(self.proc)
        self.proc = None


SDR = Sdr()


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


def _crc16_x25(d):
    c = 0xFFFF
    for x in d:
        c ^= x
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c ^ 0xFFFF


def _ax25_addr(b):
    return "".join(chr(c >> 1) for c in b[:6]).strip() + "-" + str((b[6] >> 1) & 15)


def _stop_pipeline(proc):
    """Shut `rtl_fm | direwolf` down politely: SIGINT lets rtl_fm release the dongle cleanly, SIGTERM after 6 s, and SIGKILL only
    as a last resort after 15 s. Killing rtl_fm abruptly leaves the RTL-SDR stalled (it enumerates but delivers no samples
    until it is unplugged and replugged)."""
    try:
        pg = os.getpgid(proc.pid)
    except Exception:
        return
    for sig, wait in ((signal.SIGINT, 6.0), (signal.SIGTERM, 9.0), (signal.SIGKILL, 1.0)):
        try:
            os.killpg(pg, sig)
        except Exception:
            return
        end = now() + wait
        while now() < end:
            if proc.poll() is not None:
                return
            time.sleep(0.2)


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


# --------------------------------------------------------------------------------------
# digipeater / store and forward (CPU2 digipeater, CPU1 flash journal): injected frames, store listing, optional SDR check
# --------------------------------------------------------------------------------------
DIGI_REASONS = ["repeated", "disabled", "malformed", "not a UI frame", "no unused path entry", "next hop is not us / alias / WIDE2-N",
                "our own source", "loop (our call already used)", "denied station", "duplicate", "hop limit", "too long for the radio packet",
                "global rate limit", "same station too soon"]


def digi_identity():
    """Satellite call sign / SSID / alias as configured in the TUI (.s2s2.config), defaults if the file is missing."""
    c = {"sat_call": "9N2SJ", "sat_ssid": 1, "digi_alias": "ARISS"}
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


class DwCapture:
    """rtl_fm | direwolf (stock decoder) with the KISS port read into a list of (time, frame bytes)."""

    def __init__(self, freq, gain):
        conf = os.path.join(ROOT, "tools", "testbench", "direwolf_s2s2_4800.conf")
        pipe = (f"rtl_fm -f {freq:.6f}M -s 48000 -g {gain} 2>/dev/null | "
                f"direwolf -c '{conf}' -r 48000 -n 1 -B 4800 -g -t 0 -")
        self.proc = subprocess.Popen(pipe, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, preexec_fn=os.setsid)
        self.frames = []
        self.stop_ev = threading.Event()
        threading.Thread(target=self._reader, daemon=True).start()

    def _reader(self):
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
        while not self.stop_ev.is_set():
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
                    self.frames.append((now(), fr))
                    t = ax25_text(fr)
                    if t:
                        BUS.publish("sdr", msg="Direwolf: " + t)
        s.close()

    def texts(self):
        return [t for t in (ax25_text(f) for _, f in list(self.frames)) if t]

    def close(self):
        self.stop_ev.set()
        _stop_pipeline(self.proc)


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
    dw = None
    if cfg.get("sdr"):
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
        if dw is not None:
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


TESTS = {"link": test_link, "image": test_image, "sdr": test_sdr, "rate": test_rate, "cwtest": test_cwtest, "sdrimage": test_sdrimage, "imageloop": test_imageloop, "digi": test_digi}


def start_test(name, cfg):
    """Run one test in a thread (only one at a time). Clears the satellite replay floor first (RESET frame)."""
    if not RUN["lock"].acquire(blocking=False):
        raise RuntimeError(f"test '{RUN['name']}' is still running")
    if name not in ("cwtest", "sdrimage", "imageloop", "digi") and not GS.connected():     # these tests only need the Nucleo (and the SDR); the GS is just a cross-check
        RUN["lock"].release()
        raise RuntimeError("connect the GS serial port first")
    RUN["abort"].clear()
    RUN["name"] = name

    def work():
        BUS.publish("test", name=name, state="running")
        try:
            if cfg.get("fresh", True) and OBC.connected():
                sat_fresh_start(RUN["abort"])
            TESTS[name](cfg, RUN["abort"])
        except Exception as e:
            log(f"{name} test crashed: {e}", "err")
            set_result(name, {"test": name, "when": now(), "verdict": "FAIL", "why": str(e)})
        finally:
            SDR.stop()
            BUS.publish("test", name=name, state="done")
            RUN["name"] = None
            RUN["lock"].release()

    RUN["thread"] = threading.Thread(target=work, daemon=True)
    RUN["thread"].start()


# --------------------------------------------------------------------------------------
# build / flash / status / report
# --------------------------------------------------------------------------------------
def run_cmd(args, cwd, label):
    env = dict(os.environ, PATH=TOOLCHAIN + ":/opt/homebrew/bin:" + os.environ.get("PATH", ""))
    for v in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH"):
        env.pop(v, None)
    log(f"$ {' '.join(args)}   ({label})")
    p = subprocess.Popen(args, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    tail = []
    for l in p.stdout:
        l = l.rstrip()
        if re.search(r"error|Error|BIN :|verified|Programming|undefined", l):
            log("  " + l[:200], "warn" if "rror" in l else "info")
        tail.append(l)
    p.wait()
    log(f"{label}: exit {p.returncode}", "ok" if p.returncode == 0 else "err")
    return p.returncode


def do_build(target):
    d = SAT_DIR if target == "sat" else GS_DIR
    run_cmd(["make", "-j4"], d, f"build {target}")
    status_build(target)


def do_flash(target, cfg):
    d = SAT_DIR if target == "sat" else GS_DIR
    probe = cfg.get("sat_probe" if target == "sat" else "gs_probe") or DEFAULTS["sat_probe" if target == "sat" else "gs_probe"]
    run_cmd(["make", "flash", f"SERIAL={probe}"], d, f"flash {target}")


def status_build(target):
    d, name = (SAT_DIR, "satellite") if target == "sat" else (GS_DIR, "ground_station")
    b = os.path.join(d, "build", name + ".bin")
    if os.path.exists(b):
        return {"bin": os.path.getsize(b), "mtime": os.path.getmtime(b), "limit": 57344}
    return None


def git_info():
    try:
        h = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
        return h
    except Exception:
        return "?"


def _usb_devices():
    """(product name, serial number) of every USB device, read from macOS's USB registry. This does NOT talk to the
    devices. (It used to run `st-info --probe` and `rtl_test -t`: both open the device, and probing an ST-Link resets the
    board it is attached to -- a status refresh was resetting the Nucleo in the middle of tests.)"""
    out = []
    try:
        txt = subprocess.run(["ioreg", "-p", "IOUSB", "-l", "-w0"], capture_output=True, text=True, timeout=15).stdout
        name = None
        for ln in txt.splitlines():
            m = re.search(r'"USB Product Name" = "([^"]*)"', ln)
            if m:
                name = m.group(1)
                out.append([name, ""])
                continue
            m = re.search(r'"USB Serial Number" = "([^"]*)"', ln)
            if m and out:
                out[-1][1] = m.group(1)
    except Exception:
        pass
    return [tuple(x) for x in out]


def probes():
    return [sn for name, sn in _usb_devices() if "stlink" in name.lower().replace("-", "").replace("_", "").replace(" ", "") and sn]


_sdr_cache = {"t": 0, "v": None}


def get_status(fresh=False):
    ports = sorted(glob.glob("/dev/cu.usbserial-*"))
    if fresh and not RUN["name"]:      # rtl_test takes seconds: only on an explicit "Detect"
        _sdr_cache["v"] = SDR.present()
        _sdr_cache["t"] = now()
    sdr = _sdr_cache["v"] or (False, [])
    return {"gs": {"connected": GS.connected(), "port": GS.port, "ports": ports},
            "probes": probes() if fresh else [],
            "sdr": {"present": sdr[0], "info": sdr[1]},
            "sat_build": status_build("sat"), "gs_build": status_build("gs"),
            "git": git_info(), "running": RUN["name"],
            "obc": {"connected": OBC.connected(), "port": OBC.port, "ports": OBC.ports()},
            "progress": {"total": PROG["total"], "acked": sorted(PROG["acked"]), "got": sorted(PROG["got"]),
                         "stored": sorted(PROG["stored"]), "read": sorted(PROG["read"]), "phase": PROG["phase"], "mode": PROG["mode"]},
            "image": {"name": IMG["name"], "bytes": len(IMG["bytes"]) if IMG["bytes"] else 0,
                      "chunks": math.ceil(len(IMG["bytes"]) / CHUNK) if IMG["bytes"] else 0,
                      "info": IMG["info"], "sha256": sha256(IMG["bytes"]) if IMG["bytes"] else None,
                      "fit": IMG["fit"], "type": kind_of(IMG["bytes"])[0] if IMG["bytes"] else None,
                      "fits_flash": (math.ceil(len(IMG["bytes"]) / CHUNK) <= FLASH_CHUNKS) if IMG["bytes"] else None},
            "results": {k: ({kk: vv for kk, vv in v.items() if kk != "raw_frames"} if v else None) for k, v in STATE.items()
                        if k in ("link", "image", "sdr", "rate", "cwtest", "sdrimage", "imageloop", "digi")}}


def save_report(note=""):
    """Write docs/reports/<time>/: report.json/html, source + received file, raw GS and Nucleo logs, LED trace."""
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    d = os.path.join(REPORT_DIR, ts)
    os.makedirs(d, exist_ok=True)
    st = get_status()
    rep = {"generated": datetime.datetime.now().isoformat(timespec="seconds"), "git": st["git"],
           "note": note, "sat_bin": st["sat_build"], "gs_bin": st["gs_build"],
           "results": st["results"]}
    with open(os.path.join(d, "report.json"), "w") as f:
        json.dump(rep, f, indent=2, default=str)
    with open(os.path.join(d, "gs_uart.log"), "w") as f:
        with GS.lock:
            for t, txt in GS.lines:
                f.write(f"{t:.3f} {txt}\n")
    if PROG.get("led"):
        json.dump(PROG["led"], open(os.path.join(d, "led_trace.json"), "w"))
    with open(os.path.join(d, "sat_vcp.log"), "w") as f:
        with OBC.lock:
            for t, txt in OBC.lines:
                f.write(f"{t:.3f} {txt}\n")
            for t, cmd, seq, status in OBC.acks:
                f.write(f"{t:.3f} ACK-FRAME cmd={cmd} seq={seq} status={status}\n")
    imgs = ""
    if IMG["bytes"]:
        ext, mime = kind_of(IMG["bytes"])
        open(os.path.join(d, f"source.{ext}"), "wb").write(IMG["bytes"])
        imgs += f'<figure><img src="data:{mime};base64,{base64.b64encode(IMG["bytes"]).decode()}"><figcaption>sent to the Nucleo — {IMG["name"]}, {len(IMG["bytes"])} B</figcaption></figure>'
        if IMG.get("rx"):
            open(os.path.join(d, f"received.{ext}"), "wb").write(IMG["rx"])
            imgs += f'<figure><img src="data:{mime};base64,{base64.b64encode(IMG["rx"]).decode()}"><figcaption>received by the ground station over the air</figcaption></figure>'
    sdrimg = STATE.get("sdrimage") or {}
    if sdrimg.get("raw_frames"):                  # the frames Direwolf delivered, as hex: AX.25 header + information field, no flags / FCS
        open(os.path.join(d, "direwolf_frames.txt"), "w").write(
            "# one frame per line, hex: 7 B destination, 7 B source, control, PID, information field (what Direwolf delivered over KISS)\n"
            + "\n".join(sdrimg["raw_frames"]) + "\n")
    rows = ""
    for k in ("link", "image", "sdr", "rate", "sdrimage", "digi"):
        r = st["results"].get(k)
        if not r:
            rows += f"<tr><td>{k}</td><td>not run</td><td></td></tr>"
            continue
        det = "<br>".join(f"{html.escape(str(a))}: {html.escape(str(b))}" for a, b in r.items()
                          if a not in ("rows", "test", "when", "verdict", "missing", "detail", "raw_frames")
                          and not (a == "frames" and isinstance(b, list)))
        if r.get("rows"):
            det += "<br>" + "<br>".join(html.escape(json.dumps(x)) for x in r["rows"])
        if isinstance(r.get("frames"), list):
            det += "<br>" + "<br>".join(html.escape(x) for x in r["frames"][:10])
        if r.get("missing"):
            det += "<br>missing chunks: " + html.escape(str(r["missing"]))
        rows += f"<tr><td>{k}</td><td class='{r.get('verdict','')}'>{r.get('verdict')}</td><td>{det}</td></tr>"
    page = f"""<!doctype html><meta charset=utf-8><title>S2S2 test report {ts}</title>
<style>body{{font:14px system-ui;margin:2rem;max-width:960px}}td,th{{border:1px solid #ccc;padding:6px;vertical-align:top}}
table{{border-collapse:collapse;width:100%}}.PASS{{color:#0a7a2f;font-weight:700}}.FAIL{{color:#b00020;font-weight:700}}
.PARTIAL{{color:#b36b00;font-weight:700}}figure{{display:inline-block;margin:0 1rem 1rem 0}}img{{max-width:320px;image-rendering:auto}}</style>
<h1>S2S2 link test report</h1><p>{rep['generated']} · git {st['git']} · satellite.bin {st['sat_build'] and st['sat_build']['bin']} B ·
ground_station.bin {st['gs_build'] and st['gs_build']['bin']} B</p><p>{html.escape(note)}</p>
<table><tr><th>test</th><th>verdict</th><th>details</th></tr>{rows}</table><h2>Images</h2>{imgs or '<p>none</p>'}
<p>Raw logs: gs_uart.log (ground station), sat_vcp.log (Nucleo ST-Link VCP) · machine-readable: report.json</p>"""
    open(os.path.join(d, "report.html"), "w").write(page)
    log(f"report saved: {d}", "ok")
    return d


# --------------------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------------------
class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, (dict, list)):
            body = json.dumps(body, default=str).encode()
        elif isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _body(self):
        n = int(self.headers.get("Content-Length", 0))
        return self.rfile.read(n) if n else b""

    def _json(self):
        b = self._body()
        return json.loads(b) if b else {}

    def do_GET(self):
        u = urlparse(self.path)
        p = u.path
        if p in ("/", "/index.html"):
            return self._send(200, open(os.path.join(HERE, "index.html"), "rb").read(), "text/html")
        if p == "/events":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            q = BUS.subscribe()
            try:
                while True:
                    try:
                        ev = q.get(timeout=15)
                        self.wfile.write(b"data: " + json.dumps(ev, default=str).encode() + b"\n\n")
                    except queue.Empty:
                        self.wfile.write(b": keepalive\n\n")
                    self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                BUS.unsubscribe(q)
            return
        if p == "/api/status":
            return self._send(200, get_status(fresh="fresh" in u.query))
        if p == "/api/image/source":
            return self._send(200, IMG["bytes"], kind_of(IMG["bytes"])[1]) if IMG["bytes"] else self._send(404, b"")
        if p == "/api/image/received":
            return self._send(200, IMG["rx"], kind_of(IMG["bytes"])[1]) if IMG.get("rx") else self._send(404, b"")
        if p == "/api/samples":
            out = []
            for pat in (os.path.join(GS_DIR, "*.png"), os.path.join(GS_DIR, "*.jpg"), os.path.join(ROOT, "*.png")):
                out += [os.path.relpath(f, ROOT) for f in sorted(glob.glob(pat))]
            out.sort(key=lambda p: (0 if "antarikshya" in p else 1, p))
            return self._send(200, out)
        return self._send(404, {"error": "not found"})

    def do_POST(self):
        u = urlparse(self.path)
        p = u.path
        try:
            if p == "/api/gs/connect":
                c = self._json()
                GS.connect(c.get("port", ""), int(c.get("baud", 115200)))
                return self._send(200, {"ok": True, "port": GS.port})
            if p == "/api/gs/disconnect":
                GS.disconnect()
                return self._send(200, {"ok": True})
            if p == "/api/gs/send":
                GS.send(self._json()["text"])
                return self._send(200, {"ok": True})
            if p == "/api/image/select":
                c = self._json()
                f = os.path.join(ROOT, c["path"])
                if not os.path.abspath(f).startswith(ROOT):
                    raise RuntimeError("path outside the repository")
                return self._send(200, {"info": set_image(open(f, "rb").read(), os.path.basename(f), c.get("fit", "flash"))})
            if p == "/api/image/refit":
                # re-process the file that is already loaded (upload OR sample) with another fit mode
                if IMG.get("orig") is None:
                    raise RuntimeError("no image loaded yet")
                return self._send(200, {"info": set_image(IMG["orig"], IMG["name"], self._json().get("fit", "flash"))})
            if p == "/api/image/upload":
                q = parse_qs(u.query)
                return self._send(200, {"info": set_image(self._body(), q.get("name", ["upload"])[0], q.get("fit", ["flash"])[0])})
            if p == "/api/obc/connect":
                c = self._json()
                OBC.connect(c.get("port", ""), int(c.get("baud", 115200)))
                return self._send(200, {"ok": True, "port": OBC.port})
            if p == "/api/obc/disconnect":
                OBC.disconnect()
                return self._send(200, {"ok": True})
            if p == "/api/digi/inject":
                if RUN["name"]:
                    raise RuntimeError(f"test '{RUN['name']}' is running")
                st, why = digi_inject(self._json()["text"])
                return self._send(200, {"status": st, "result": why})
            if p == "/api/digi/store":
                if RUN["name"]:
                    raise RuntimeError(f"test '{RUN['name']}' is running")
                line, recs = saf_read()
                return self._send(200, {"journal": line, "objects": recs, "stored": len(recs), "bytes": sum(x["len"] for x in recs)})
            if p == "/api/digi/clear":
                mk = OBC.mark()
                OBC.send(OBC_SAF, 0xFFFF)
                return self._send(200, {"status": OBC.wait_ack(OBC_SAF, 0xFFFF, mk, 8), "note": "journal emptied; reset CPU2 so it forgets its RAM copy"})
            if p == "/api/test/start":
                c = self._json()
                start_test(c["name"], c)
                return self._send(200, {"ok": True})
            if p == "/api/test/stop":
                RUN["abort"].set()
                SDR.stop()
                return self._send(200, {"ok": True})
            if p == "/api/build":
                c = self._json()
                threading.Thread(target=do_build, args=(c["target"],), daemon=True).start()
                return self._send(200, {"ok": True})
            if p == "/api/flash":
                c = self._json()
                threading.Thread(target=do_flash, args=(c["target"], c), daemon=True).start()
                return self._send(200, {"ok": True})
            if p == "/api/sat/reset":
                threading.Thread(target=lambda: sat_fresh_start(RUN["abort"]), daemon=True).start()
                return self._send(200, {"ok": True})
            if p == "/api/report":
                c = self._json()
                return self._send(200, {"dir": save_report(c.get("note", ""))})
        except Exception as e:
            import traceback
            traceback.print_exc()
            log(f"{p}: {e}", "err")
            return self._send(400, {"error": str(e)})
        return self._send(404, {"error": "not found"})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--no-browser", action="store_true")
    a = ap.parse_args()
    srv = ThreadingHTTPServer((a.host, a.port), H)
    url = f"http://{a.host}:{a.port}/"
    print("S2S2 test bench:", url)
    if not a.no_browser:
        subprocess.Popen(["open", url])
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        SDR.stop()
        GS.disconnect()
        OBC.disconnect()


if __name__ == "__main__":
    main()
