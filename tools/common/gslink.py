"""
gslink.py -- the ground-station board's UART (FTDI, 115200 8N1) with automatic re-connect.

The reader thread splits the stream into lines, hands every line to `on_line(t, text)` and keeps the last lines for a console. The board
resets when DTR/RTS toggle on some adapters, so the port is opened with both lines low.
"""
import glob
import threading
import time

try:
    import serial
except ImportError:                                  # pragma: no cover
    serial = None


class GSLink:
    def __init__(self, on_line, on_state=None, history=2000, want=None):
        self.on_line, self.on_state = on_line, on_state
        self.ser = None
        self.port = None
        self.want = want                             # None = auto (first /dev/cu.usbserial-*), "" = stay disconnected, else a path
        self.baud = 115200
        self.lines = []
        self.history = history
        self.lock = threading.Lock()
        self.rx_bytes = 0
        self.last_rx = 0.0
        self.error = ""
        self.stop = threading.Event()
        threading.Thread(target=self._run, daemon=True).start()

    @staticmethod
    def ports():
        return sorted(glob.glob("/dev/cu.usbserial-*") + glob.glob("/dev/ttyUSB*"))

    def connected(self):
        return self.ser is not None and self.ser.is_open

    def set_port(self, port):
        """port: path, "" (disconnect and stay disconnected) or None (automatic)."""
        self.want = port
        self._close()

    def _close(self):
        s, self.ser = self.ser, None
        if s:
            try:
                s.close()
            except Exception:
                pass
            self._state()

    def _state(self):
        if self.on_state:
            self.on_state(self.connected(), self.port, self.error)

    def send(self, text):
        if not self.connected():
            raise RuntimeError("ground station is not connected")
        self.ser.write((text + "\r\n").encode())

    def _open(self):
        if serial is None:
            self.error = "pyserial is not installed"
            return
        port = self.want if self.want else (self.ports() or [None])[0]
        if not port or self.want == "":
            return
        try:
            s = serial.Serial()
            s.port, s.baudrate, s.timeout = port, self.baud, 0.1
            s.dtr = False
            s.rts = False
            s.open()
            self.ser, self.port, self.error = s, port, ""
            self._state()
        except Exception as e:
            self.error = str(e)

    def _run(self):
        buf = b""
        while not self.stop.is_set():
            if not self.connected():
                self._open()
                if not self.connected():
                    time.sleep(2.0)
                    continue
                buf = b""
            try:
                d = self.ser.read(4096)
            except Exception as e:
                self.error = str(e)
                self._close()
                continue
            if not d:
                continue
            self.rx_bytes += len(d)
            self.last_rx = time.time()
            buf += d
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                txt = raw.decode(errors="replace").replace("\r", "")
                if txt.strip():
                    t = time.time()
                    with self.lock:
                        self.lines.append((t, txt))
                        if len(self.lines) > self.history:
                            del self.lines[:200]
                    self.on_line(t, txt)
            if len(buf) > 65536:
                buf = b""
