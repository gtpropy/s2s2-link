"""tb.devices"""

import collections
import os
import re
import signal
import socket
import subprocess
import threading
import time
from tb.bus import BUS, IMAGES, add_event, log
from tb.digi import ax25_text
from tb.usb import _usb_devices
from tb.util import ROOT, now

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
                    info = fr[16:] if len(fr) >= 17 else b""
                    if len(info) == 138 and info[0] == 0x08 and (info[136] | (info[137] << 8)) == _crc16_x25(info[:136]):
                        IMAGES.ingest("sdr", info[2] | (info[3] << 8), info[4] | (info[5] << 8), info[8:136])      # camera fragment
                    elif t:
                        add_event({"kind": "frame", "src": "sdr", "text": t, "bytes": len(fr)})
        s.close()

    def texts(self):
        return [t for t in (ax25_text(f) for _, f in list(self.frames)) if t]

    def close(self):
        self.stop_ev.set()
        _stop_pipeline(self.proc)
