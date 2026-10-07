#!/usr/bin/env python3
# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-09-01
"""
response_analyzer.py -- structured "what did the satellite just say"
window for the ground station. Parses gs_main.c's real UART output
(satellite/gs_main.c -- already does on-board FX.25/RS/AX.25 decode and
prints the structured result, see the RX ACK/TELEMETRY/REJECTED/STATS/
GAP REPORT line formats below) into a table instead of a raw scrolling
log. No demod/RS decode happens here -- the GS board already did that;
this is a line parser against its real, already-decoded output.

Two ways to use it:
  - Standalone: `python3 response_analyzer.py` -- has its own port/baud
    picker and serial connection.
  - Embedded: `ResponseAnalyzerWindow(standalone=False)` then feed it
    lines via `.feed_line(line)` from an already-open connection (this
    is how tx_gui.py's "Response Analyzer" button opens it, sharing
    the same serial port instead of trying to open it twice).

Any line that doesn't match a known format still shows in the raw-log
pane at the bottom -- nothing is silently dropped.
"""
import re
import sys
import os
import time

try:
    from PyQt6.QtCore import Qt
    from PyQt6.QtGui import QColor
    from PyQt6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QTableWidget, QTableWidgetItem, QPlainTextEdit, QLabel, QPushButton,
        QComboBox, QSplitter, QHeaderView, QMessageBox,
    )
    PYQT_VERSION = 6
except ImportError:
    from PyQt5.QtCore import Qt
    from PyQt5.QtGui import QColor
    from PyQt5.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QTableWidget, QTableWidgetItem, QPlainTextEdit, QLabel, QPushButton,
        QComboBox, QSplitter, QHeaderView, QMessageBox,
    )
    PYQT_VERSION = 5

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

BG = "#05070a"
PANEL = "#0b0f14"
FG = "#39ff9c"
FG_DIM = "#3f6f5c"
ACCENT = "#22d3ee"
WARN = "#ffb020"
ERR = "#ff4d5e"

DARK_STYLESHEET = f"""
QWidget {{ background-color: {BG}; color: {FG}; font-family: Menlo, Consolas, monospace; }}
QMainWindow {{ background-color: {BG}; }}
QTableWidget {{ background-color: {PANEL}; color: {FG}; gridline-color: #17251f;
                selection-background-color: #1c4a37; }}
QHeaderView::section {{ background-color: #0f1c17; color: {ACCENT}; border: 1px solid #17251f;
                        padding: 4px; }}
QPlainTextEdit, QComboBox {{ background-color: {PANEL}; border: 1px solid #17251f; color: {FG}; }}
QPushButton {{ background-color: #0f1c17; border: 1px solid #1f4a3a; border-radius: 4px;
               padding: 5px 12px; color: {FG}; }}
QPushButton:hover {{ background-color: #143528; border-color: {ACCENT}; }}
QLabel {{ color: {FG}; }}
"""

COLUMNS = ["Time", "Type", "Src", "Dst", "Seq", "Session", "Flags",
           "RS Corr", "RSSI", "SNR", "Status / Detail"]

# --- gs_main.c's real output line formats (satellite/gs_main.c, cited
# in the architecture plan) ---
_RE_ACK = re.compile(
    r"RX ACK: status=0x([0-9A-Fa-f]{2}) \(([^)]*)\) data_id=0x([0-9A-Fa-f]{2}) acked_seq=(\d+)")
_RE_BLOCK_HEADER = re.compile(r"^\s*---\s*(.+?)\s*---\s*$")
_RE_BLOCK_FIELDS1 = re.compile(
    r"version=(\d+) type=(\d+) src=(\d+) dst=(\d+) seq=(\d+) session=0x([0-9A-Fa-f]{4}) flags=0x([0-9A-Fa-f]{2})")
_RE_BLOCK_FIELDS2 = re.compile(
    r"payload_len=(\d+) rs_bytes_corrected=(-?\d+) rssi=(-?\d+) dBm snr=(-?\d+) dB")
_RE_PAYLOAD_TEXT = re.compile(r'Payload \(text\):\s*"(.*)"')
_RE_HEX_LINE = re.compile(r"^([0-9A-Fa-f]{2}\s?)+$")
_RE_REJECTED = re.compile(
    r"FRAME REJECTED.*?rssi=(-?\d+)\s*snr=(-?\d+)")
_RE_STATS = re.compile(
    r"SESSION STATS: (\d+) OK \| (\d+) bad \| (\d+) misaddressed")
_RE_TX_COMMAND = re.compile(r"TX COMMAND 0x([0-9A-Fa-f]{2}) \(seq=(\d+)\)")
_RE_TX_RESULT = re.compile(r"COMMAND (SENT|TIMED OUT) - SWITCHING TO RX")
_RE_GAP_REPORT = re.compile(r"GAP REPORT: (\d+) missing seq number")
_RE_GAP_NONE = re.compile(r"no gaps - session complete")

TYPE_NAMES = {0: "TELEMETRY", 1: "TELECOMMAND", 2: "ACK", 3: "GAP_REPORT", 4: "RAW"}


class ResponseAnalyzerWindow(QMainWindow):
    def __init__(self, standalone: bool = True):
        super().__init__()
        self.setWindowTitle("S2S2 Response Analyzer -- decoded RX")
        self.resize(1300, 700)
        self.setStyleSheet(DARK_STYLESHEET)

        self._standalone = standalone
        self.ser = None
        self.reader = None
        self._line_buf = ""
        self._pending = None  # accumulating a TELEMETRY/non-telemetry block

        central = QWidget()
        self.setCentralWidget(central)
        outer = QVBoxLayout(central)

        if standalone:
            conn_row = QHBoxLayout()
            conn_row.addWidget(QLabel("Port:"))
            self.port_combo = QComboBox()
            self.port_combo.setMinimumWidth(220)
            conn_row.addWidget(self.port_combo)
            refresh_btn = QPushButton("Refresh")
            refresh_btn.clicked.connect(self._refresh_ports)
            conn_row.addWidget(refresh_btn)
            conn_row.addWidget(QLabel("Baud:"))
            self.baud_combo = QComboBox()
            for b in (9600, 115200):
                self.baud_combo.addItem(str(b), b)
            self.baud_combo.setCurrentIndex(1)
            conn_row.addWidget(self.baud_combo)
            self.connect_btn = QPushButton("Connect")
            self.connect_btn.clicked.connect(self._toggle_connection)
            conn_row.addWidget(self.connect_btn)
            conn_row.addStretch(1)
            outer.addLayout(conn_row)
            self._refresh_ports()

        split = QSplitter(Qt.Orientation.Vertical if PYQT_VERSION == 6 else Qt.Vertical)

        self.table = QTableWidget(0, len(COLUMNS))
        self.table.setHorizontalHeaderLabels(COLUMNS)
        self.table.horizontalHeader().setSectionResizeMode(
            QHeaderView.ResizeMode.Stretch if PYQT_VERSION == 6 else QHeaderView.Stretch)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers
                                    if PYQT_VERSION == 6 else QTableWidget.NoEditTriggers)
        split.addWidget(self.table)

        raw_box = QWidget()
        raw_layout = QVBoxLayout(raw_box)
        raw_layout.setContentsMargins(0, 0, 0, 0)
        raw_layout.addWidget(QLabel("Raw / unparsed lines"))
        self.raw_log = QPlainTextEdit()
        self.raw_log.setReadOnly(True)
        raw_layout.addWidget(self.raw_log)
        split.addWidget(raw_box)
        split.setSizes([500, 200])

        outer.addWidget(split, 1)

        self.summary_label = QLabel("no session stats yet")
        self.summary_label.setStyleSheet(f"color: {FG_DIM};")
        outer.addWidget(self.summary_label)

    # ---------------------------------------------------------------
    # standalone connection handling (mirrors tx_gui.py's pattern)
    # ---------------------------------------------------------------
    def _refresh_ports(self):
        import serial.tools.list_ports
        self.port_combo.clear()
        for p in serial.tools.list_ports.comports():
            self.port_combo.addItem(f"{p.device} -- {p.description}", p.device)

    def _toggle_connection(self):
        if self.ser is not None:
            self._disconnect()
        else:
            self._connect()

    def _connect(self):
        import serial
        from tx_gui import SerialReader

        port = self.port_combo.currentData()
        if not port:
            QMessageBox.warning(self, "No port", "Select a serial port first.")
            return
        baud = self.baud_combo.currentData()
        try:
            self.ser = serial.Serial(port, baud, timeout=0.1)
        except serial.SerialException as e:
            QMessageBox.critical(self, "Connection failed", str(e))
            self.ser = None
            return
        self.reader = SerialReader(self.ser)
        self.reader.data_received.connect(self._on_data_received)
        self.reader.error.connect(self._on_serial_error)
        self.reader.start()
        self.connect_btn.setText("Disconnect")
        self._log_raw(f"--- connected to {port} @ {baud} ---")

    def _disconnect(self):
        if self.reader:
            self.reader.stop()
            self.reader = None
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
        self.connect_btn.setText("Connect")
        self._log_raw("--- disconnected ---")

    def _on_serial_error(self, message):
        self._log_raw(f"--- serial error: {message} ---")
        self._disconnect()

    def _on_data_received(self, data: bytes):
        text = data.decode("latin-1", errors="replace")
        self._line_buf += text
        parts = self._line_buf.replace("\r\n", "\n").replace("\r", "\n").split("\n")
        self._line_buf = parts[-1]
        for line in parts[:-1]:
            self.feed_line(line)

    # ---------------------------------------------------------------
    # public: feed one already-received line (used both by our own
    # SerialReader above and by an embedding GUI like tx_gui.py's
    # "Response Analyzer" button, which shares its own open connection
    # instead of letting us open the same port twice).
    # ---------------------------------------------------------------
    def feed_line(self, line: str):
        stripped = line.rstrip()
        if not stripped:
            return

        m = _RE_ACK.match(stripped)
        if m:
            status_hex, status_name, data_id_hex, acked_seq = m.groups()
            self._add_row("ACK", "", "", acked_seq, "", "", "", "", "",
                           f"status=0x{status_hex} ({status_name}) data_id=0x{data_id_hex}")
            return

        m = _RE_BLOCK_HEADER.match(stripped)
        if m:
            self._pending = {"label": m.group(1)}
            return

        if self._pending is not None:
            m = _RE_BLOCK_FIELDS1.search(stripped)
            if m:
                (self._pending["version"], self._pending["type"], self._pending["src"],
                 self._pending["dst"], self._pending["seq"], self._pending["session"],
                 self._pending["flags"]) = m.groups()
                return
            m = _RE_BLOCK_FIELDS2.search(stripped)
            if m:
                (self._pending["payload_len"], self._pending["rs_corrected"],
                 self._pending["rssi"], self._pending["snr"]) = m.groups()
                return
            m = _RE_PAYLOAD_TEXT.search(stripped)
            if m:
                p = self._pending
                type_num = int(p.get("type", -1)) if "type" in p else -1
                type_name = TYPE_NAMES.get(type_num, p["label"])
                self._add_row(
                    type_name, p.get("src", ""), p.get("dst", ""), p.get("seq", ""),
                    p.get("session", ""), p.get("flags", ""), p.get("rs_corrected", ""),
                    p.get("rssi", ""), p.get("snr", ""),
                    f'payload_len={p.get("payload_len", "?")} "{m.group(1)}"')
                self._pending = None
                return
            if _RE_HEX_LINE.match(stripped):
                return  # hex dump line, part of the block, nothing to extract

        m = _RE_REJECTED.search(stripped)
        if m:
            rssi, snr = m.groups()
            self._add_row("REJECTED", "", "", "", "", "", "", rssi, snr,
                           "bad FX.25 tag/RS, bad AX.25 FCS, or malformed")
            return

        m = _RE_STATS.match(stripped)
        if m:
            ok, bad, misaddr = m.groups()
            self.summary_label.setText(f"SESSION STATS: {ok} OK | {bad} bad | {misaddr} misaddressed")
            self.summary_label.setStyleSheet(f"color: {ACCENT}; font-weight: bold;")
            return

        m = _RE_TX_COMMAND.match(stripped)
        if m:
            opcode, seq = m.groups()
            self._add_row("TX", "", "", seq, "", "", "", "", "", f"opcode=0x{opcode}")
            return

        m = _RE_TX_RESULT.search(stripped)
        if m:
            self._add_row("TX", "", "", "", "", "", "", "", "", m.group(1))
            return

        m = _RE_GAP_REPORT.match(stripped)
        if m:
            self._add_row("GAP", "", "", "", "", "", "", "", "", f"{m.group(1)} missing")
            return

        if _RE_GAP_NONE.search(stripped):
            self._add_row("GAP", "", "", "", "", "", "", "", "", "no gaps - session complete")
            return

        self._log_raw(line)

    def _add_row(self, type_name, src, dst, seq, session, flags, rs_corr, rssi, snr, detail):
        row = self.table.rowCount()
        self.table.insertRow(row)
        values = [time.strftime("%H:%M:%S"), type_name, str(src), str(dst), str(seq),
                  str(session), str(flags), str(rs_corr), str(rssi), str(snr), str(detail)]
        color = {
            "ACK": ACCENT, "TELEMETRY": FG, "REJECTED": ERR, "GAP": WARN, "TX": ACCENT,
        }.get(type_name, FG)
        for col, v in enumerate(values):
            item = QTableWidgetItem(v)
            item.setForeground(QColor(color))
            self.table.setItem(row, col, item)
        self.table.scrollToBottom()

    def _log_raw(self, line: str):
        self.raw_log.appendPlainText(line)

    def closeEvent(self, event):
        if self._standalone:
            self._disconnect()
        super().closeEvent(event)


def main():
    app = QApplication(sys.argv)
    win = ResponseAnalyzerWindow(standalone=True)
    win.show()
    fn = getattr(app, "exec", None)
    sys.exit(fn() if fn is not None else app.exec_())


if __name__ == "__main__":
    main()
