"""tb.links"""

import glob
import re
import threading
import time
from tb.bus import BUS, IMAGES, add_event, log
from tb.compat import events_mod, serial
from tb.util import OBC_INFO, OBC_READF, OBC_RESET, OBC_STATUS, crc16_ccitt, now, obc_frame

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
                for ev in events_mod.parse_gs_line(txt):
                    if ev["kind"] == "camera":              # stored and rebuilt in the background, test or no test
                        try:
                            IMAGES.ingest("gs", int(ev["pkt"]) - 1, int(ev["total"]), bytes.fromhex(ev["hex"]))
                        except (TypeError, ValueError):
                            pass
                    elif ev["kind"] == "cwraw":
                        BUS.publish("cwraw", **{k: v for k, v in ev.items() if k != "kind"})
                    else:
                        add_event(ev)

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
        self.text_only = False         # CUBUS: the satellite's debug UART, no s2s2obc frames (CPU1 runs the flight OBC firmware)

    def up(self):
        """The serial port is open (text, and frames unless text_only)."""
        return self.ser is not None and self.ser.is_open and getattr(self, "ok", True)

    def connected(self):
        """The s2s2obc protocol can be used: port open and not a text-only link. Every test that talks to CPU1 asks this."""
        return self.up() and not self.text_only

    @staticmethod
    def ports():
        return sorted(glob.glob("/dev/cu.usbmodem*"))

    @staticmethod
    def uart_ports():
        """Candidates for the CUBUS debug UART: a USB serial adapter (other than the ground station's) or any ST-Link VCP."""
        gs = GS.port
        return sorted(p for p in glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/cu.usbmodem*") if p != gs)

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

    def connect(self, port="", baud=115200, text_only=False):
        self.disconnect()
        self.text_only = bool(text_only)
        if not port:
            c = self.uart_ports() if self.text_only else self.ports()
            if not c:
                raise RuntimeError("no serial port found: plug the USB serial adapter of the satellite's debug UART in" if self.text_only
                                   else "no /dev/cu.usbmodem* port: plug the Nucleo's ST-Link USB cable in")
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
            if self.text_only:                                                   # CUBUS debug UART: plain text lines only, never parsed as frames
                j = buf.rfind(b"\n")
                if j >= 0:
                    self._text(buf[:j + 1])
                    buf = buf[j + 1:]
                if len(buf) > 8192:
                    buf = buf[-256:]
                continue
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
        self.wait_ack(OBC_INFO, 0, mk, timeout, abort)
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
