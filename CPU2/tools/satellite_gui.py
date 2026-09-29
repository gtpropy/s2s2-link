#!/usr/bin/env python3
# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-09-01
"""
satellite_gui.py -- bench console for the SATELLITE board's own debug
UART (ipcc_m0plus_main.c, CPU2's LPUART1/PA2-PA3 -- a separate physical
port from whatever exposes NuttX/NSH on CPU1; see the architecture plan
for why "/dev/tty.usbmodem-xxxx" often isn't the right one here).

Shows:
  - Live cycle-phase indicator (CW / BEACON1 / MISSION / BEACON2),
    driven by the new "CYCLE PHASE: X" lines Cycle_Tick() now announces.
  - Digipeater / command-ACK / burst activity, parsed from the real
    firmware log lines.
  - Test panel: TESTCW / TESTBEACON1 / TESTBEACON2 / TESTDIGI <hex> --
    the new DEBUG_BUILD-only UART commands added alongside this GUI.
  - SATMODE ON/OFF: locks out the whole test panel (both here in the
    GUI and, more importantly, on the real firmware -- see
    ipcc_m0plus_main.c's s_satellite_mode) and switches this window to
    a stripped "telemetry only" view, so from that point on the only
    way to interact with the board is over real RF via the GS console
    (s2s2_tx_gui.py / response_analyzer.py) -- same as a real deployed
    satellite. SATMODE OFF (still sendable from this same local UART
    cable) restores the test panel.

Run: python3 satellite_gui.py
"""
import re
import sys
import os
import time

try:
    from PyQt6.QtCore import Qt
    from PyQt6.QtGui import QColor, QFont, QTextCursor, QTextCharFormat
    from PyQt6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QGridLayout,
        QComboBox, QPushButton, QLineEdit, QPlainTextEdit, QLabel, QGroupBox,
        QSplitter, QMessageBox, QStatusBar,
    )
    PYQT_VERSION = 6
except ImportError:
    from PyQt5.QtCore import Qt
    from PyQt5.QtGui import QColor, QFont, QTextCursor, QTextCharFormat
    from PyQt5.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout, QGridLayout,
        QComboBox, QPushButton, QLineEdit, QPlainTextEdit, QLabel, QGroupBox,
        QSplitter, QMessageBox, QStatusBar,
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
QGroupBox {{ border: 1px solid #143028; border-radius: 6px; margin-top: 10px;
             color: {ACCENT}; font-weight: bold; padding-top: 8px; }}
QGroupBox::title {{ subcontrol-origin: margin; left: 10px; padding: 0 4px; }}
QPushButton {{ background-color: #0f1c17; border: 1px solid #1f4a3a; border-radius: 4px;
               padding: 5px 12px; color: {FG}; }}
QPushButton:hover {{ background-color: #143528; border-color: {ACCENT}; }}
QPushButton:pressed {{ background-color: #1c4a37; }}
QPushButton:disabled {{ color: #2a4038; border-color: #17251f; }}
QLineEdit, QComboBox, QPlainTextEdit {{
    background-color: {PANEL}; border: 1px solid #17251f; color: {FG}; }}
QStatusBar {{ background-color: {PANEL}; color: {FG_DIM}; }}
QLabel {{ color: {FG}; }}
"""

_END_MOVE = QTextCursor.MoveOperation.End if PYQT_VERSION == 6 else QTextCursor.End

PHASES = ["CW", "BEACON1", "MISSION", "BEACON2"]

_RE_CYCLE_PHASE = re.compile(r"CYCLE PHASE:\s*(CW1|MISSION1|CW2|MISSION2)")
_RE_DIGI_DUP = re.compile(r"DIGI: duplicate, not re-relaying")
_RE_DIGI_RELAYED = re.compile(
    r"DIGI: relayed via hop (\d+) \(([^)]+)\), (\d+) info byte\(s\), rs_corrected=(-?\d+)")
_RE_DIGI_FAILED = re.compile(r"DIGI: relay TX failed")
_RE_TX_ACK = re.compile(r"TX ACK: status=0x([0-9A-Fa-f]{2}) \(([^)]*)\) for seq=(\d+)")
_RE_BURST_START = re.compile(r"BURST START: (\d+) telemetry frames, session=0x([0-9A-Fa-f]{4})")
_RE_BURST_COMPLETE = re.compile(r"BURST COMPLETE \((\d+)/(\d+) sent cleanly, (\d+) failed\)")
_RE_CW_BEACON = re.compile(r'CW BEACON TX: "([^"]*)"')
_RE_SATMODE_ON = re.compile(r"SATELLITE MODE ON")
_RE_SATMODE_OFF = re.compile(r"SATELLITE MODE OFF")
_RE_TESTDIGI_ECHO = re.compile(r"TESTDIGI: feeding a \d+-byte frame.*\(digipeater currently (\w+)")


class SatelliteGui(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("S2S2 Satellite Console")
        self.resize(1200, 780)
        self.setStyleSheet(DARK_STYLESHEET)

        self.ser = None
        self.reader = None
        self._line_buf = ""
        self._satellite_mode = False
        self._cycle_phase = None
        self._digi_relayed = 0
        self._digi_duplicate = 0

        central = QWidget()
        self.setCentralWidget(central)
        outer = QVBoxLayout(central)

        # --- connection row ---
        conn_row = QHBoxLayout()
        conn_row.addWidget(QLabel("Port:"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(220)
        conn_row.addWidget(self.port_combo)
        refresh_btn = QPushButton("Refresh")
        refresh_btn.clicked.connect(self.refresh_ports)
        conn_row.addWidget(refresh_btn)
        conn_row.addWidget(QLabel("Baud:"))
        self.baud_combo = QComboBox()
        for b in (9600, 115200):
            self.baud_combo.addItem(str(b), b)
        self.baud_combo.setCurrentIndex(1)
        conn_row.addWidget(self.baud_combo)
        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self.toggle_connection)
        conn_row.addWidget(self.connect_btn)
        conn_row.addStretch(1)
        outer.addLayout(conn_row)

        # --- cycle phase indicator ---
        phase_box = QGroupBox("Cycle_Tick() phase")
        phase_row = QHBoxLayout(phase_box)
        self.phase_labels = {}
        for name in PHASES:
            lbl = QLabel(name)
            lbl.setAlignment(Qt.AlignmentFlag.AlignCenter if PYQT_VERSION == 6 else Qt.AlignCenter)
            lbl.setFont(QFont("Menlo" if sys.platform == "darwin" else "Consolas", 13, QFont.Weight.Bold
                               if PYQT_VERSION == 6 else QFont.Bold))
            lbl.setStyleSheet(self._phase_style(False))
            phase_row.addWidget(lbl, 1)
            self.phase_labels[name] = lbl
        outer.addWidget(phase_box)

        main_split = QSplitter(Qt.Orientation.Horizontal if PYQT_VERSION == 6 else Qt.Horizontal)

        # --- left: activity log ---
        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(0, 0, 0, 0)
        left_layout.addWidget(QLabel("Activity log"))
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setFont(QFont("Menlo", 11) if sys.platform == "darwin" else QFont("Consolas", 10))
        left_layout.addWidget(self.log, 1)
        self.digi_summary = QLabel("digipeater: 0 relayed, 0 duplicate")
        self.digi_summary.setStyleSheet(f"color: {FG_DIM};")
        left_layout.addWidget(self.digi_summary)
        main_split.addWidget(left)

        # --- right: test panel + satmode ---
        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.setContentsMargins(0, 0, 0, 0)

        self.test_box = QGroupBox("Test panel  (DEBUG_BUILD only, bench UART)")
        test_layout = QVBoxLayout(self.test_box)

        force_row = QHBoxLayout()
        self.testcw_btn = QPushButton("TESTCW")
        self.testcw_btn.setToolTip("Force an immediate CW beacon send")
        self.testcw_btn.clicked.connect(lambda: self.send_line("TESTCW"))
        force_row.addWidget(self.testcw_btn)
        self.testbeacon1_btn = QPushButton("TESTBEACON1")
        self.testbeacon1_btn.clicked.connect(lambda: self.send_line("TESTBEACON1"))
        force_row.addWidget(self.testbeacon1_btn)
        self.testbeacon2_btn = QPushButton("TESTBEACON2")
        self.testbeacon2_btn.clicked.connect(lambda: self.send_line("TESTBEACON2"))
        force_row.addWidget(self.testbeacon2_btn)
        test_layout.addLayout(force_row)

        digi_row = QHBoxLayout()
        digi_row.addWidget(QLabel("TESTDIGI hex:"))
        self.testdigi_input = QLineEdit()
        self.testdigi_input.setPlaceholderText("168-byte FX.25 frame as hex (336 chars)")
        digi_row.addWidget(self.testdigi_input, 1)
        self.testdigi_btn = QPushButton("Send")
        self.testdigi_btn.clicked.connect(self.send_testdigi)
        digi_row.addWidget(self.testdigi_btn)
        test_layout.addLayout(digi_row)

        right_layout.addWidget(self.test_box)

        satmode_box = QGroupBox("Satellite mode")
        satmode_layout = QVBoxLayout(satmode_box)
        satmode_row = QHBoxLayout()
        self.satmode_on_btn = QPushButton("SATMODE ON")
        self.satmode_on_btn.setToolTip(
            "Locks out TX/POWER/BEACON/TEST* on the real firmware and switches this "
            "GUI to telemetry-only -- all further interaction goes through the GS.")
        self.satmode_on_btn.clicked.connect(lambda: self.send_line("SATMODE ON"))
        satmode_row.addWidget(self.satmode_on_btn)
        self.satmode_off_btn = QPushButton("SATMODE OFF")
        self.satmode_off_btn.clicked.connect(lambda: self.send_line("SATMODE OFF"))
        satmode_row.addWidget(self.satmode_off_btn)
        satmode_layout.addLayout(satmode_row)
        self.satmode_status = QLabel("bench mode (debug/test commands available)")
        self.satmode_status.setStyleSheet(f"color: {FG_DIM};")
        satmode_layout.addWidget(self.satmode_status)
        right_layout.addWidget(satmode_box)

        # --- telemetry-only panel (shown instead of the test panel once
        # SATMODE ON is confirmed by the board) ---
        self.telemetry_box = QGroupBox("Telemetry (satellite mode -- talk to it via the GS)")
        telemetry_layout = QVBoxLayout(self.telemetry_box)
        self.last_cw_label = QLabel("Last CW beacon: --")
        telemetry_layout.addWidget(self.last_cw_label)
        self.last_burst_label = QLabel("Last burst: --")
        telemetry_layout.addWidget(self.last_burst_label)
        right_layout.addWidget(self.telemetry_box)
        self.telemetry_box.setVisible(False)

        right_layout.addStretch(1)
        main_split.addWidget(right)
        main_split.setSizes([760, 440])
        outer.addWidget(main_split, 1)

        self.status = QStatusBar()
        self.setStatusBar(self.status)

        self.set_connected_ui(False)
        self.refresh_ports()
        self.status.showMessage("Disconnected")

    # ---------------------------------------------------------------
    def _phase_style(self, active: bool) -> str:
        if active:
            return (f"background-color: #1c4a37; border: 2px solid {ACCENT}; "
                     f"color: {ACCENT}; border-radius: 6px; padding: 10px;")
        return (f"background-color: {PANEL}; border: 1px solid #17251f; "
                f"color: {FG_DIM}; border-radius: 6px; padding: 10px;")

    def refresh_ports(self):
        import serial.tools.list_ports
        current = self.port_combo.currentData()
        self.port_combo.clear()
        for p in serial.tools.list_ports.comports():
            self.port_combo.addItem(f"{p.device} -- {p.description}", p.device)
        if current:
            idx = self.port_combo.findData(current)
            if idx >= 0:
                self.port_combo.setCurrentIndex(idx)

    def set_connected_ui(self, connected: bool):
        self.port_combo.setEnabled(not connected)
        self.baud_combo.setEnabled(not connected)
        self.connect_btn.setText("Disconnect" if connected else "Connect")
        for btn in (self.testcw_btn, self.testbeacon1_btn, self.testbeacon2_btn,
                    self.testdigi_btn, self.satmode_on_btn, self.satmode_off_btn):
            btn.setEnabled(connected)

    def toggle_connection(self):
        if self.ser is not None:
            self.disconnect_serial()
        else:
            self.connect_serial()

    def connect_serial(self):
        import serial
        from s2s2_tx_gui import SerialReader

        port = self.port_combo.currentData()
        if not port:
            QMessageBox.warning(self, "No port", "Select the satellite board's CPU2 debug UART port "
                                                  "(not the NSH/NuttX console port) and click Refresh.")
            return
        baud = self.baud_combo.currentData()
        try:
            self.ser = serial.Serial(port, baud, timeout=0.1)
        except serial.SerialException as e:
            QMessageBox.critical(self, "Connection failed", str(e))
            self.ser = None
            return
        self.reader = SerialReader(self.ser)
        self.reader.data_received.connect(self.on_data_received)
        self.reader.error.connect(self.on_serial_error)
        self.reader.start()
        self.set_connected_ui(True)
        self.status.showMessage(f"Connected to {port} @ {baud}")
        self.append_log(f"--- connected to {port} @ {baud} ---", FG_DIM)

    def disconnect_serial(self):
        if self.reader:
            self.reader.stop()
            self.reader = None
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
        self.set_connected_ui(False)
        self.status.showMessage("Disconnected")
        self.append_log("--- disconnected ---", FG_DIM)

    def on_serial_error(self, message):
        self.append_log(f"--- serial error: {message} ---", ERR)
        self.disconnect_serial()

    def on_data_received(self, data: bytes):
        text = data.decode("latin-1", errors="replace")
        self._line_buf += text
        parts = self._line_buf.replace("\r\n", "\n").replace("\r", "\n").split("\n")
        self._line_buf = parts[-1]
        for line in parts[:-1]:
            self._handle_line(line)

    # ---------------------------------------------------------------
    def send_line(self, keyword: str):
        if self.ser is None:
            return
        try:
            self.ser.write((keyword + "\r\n").encode("ascii"))
        except Exception as e:
            self.on_serial_error(str(e))
            return
        self.append_log(f"--- sent {keyword} ---", ACCENT)

    def send_testdigi(self):
        hex_text = self.testdigi_input.text().strip()
        if not hex_text:
            return
        self.send_line(f"TESTDIGI {hex_text}")

    # ---------------------------------------------------------------
    def _handle_line(self, line: str):
        stripped = line.rstrip()
        if not stripped:
            return
        self._append_colored(stripped)

        m = _RE_CYCLE_PHASE.search(stripped)
        if m:
            self._set_phase(m.group(1))
            return

        if _RE_SATMODE_ON.search(stripped):
            self._set_satellite_mode(True)
            return
        if _RE_SATMODE_OFF.search(stripped):
            self._set_satellite_mode(False)
            return

        if _RE_DIGI_DUP.search(stripped):
            self._digi_duplicate += 1
            self._update_digi_summary()
            return
        m = _RE_DIGI_RELAYED.search(stripped)
        if m:
            self._digi_relayed += 1
            self._update_digi_summary()
            return
        if _RE_DIGI_FAILED.search(stripped):
            self._update_digi_summary()
            return

        m = _RE_CW_BEACON.search(stripped)
        if m:
            self.last_cw_label.setText(f'Last CW beacon: "{m.group(1)}"')
            return

        m = _RE_BURST_COMPLETE.search(stripped)
        if m:
            sent, total, failed = m.groups()
            self.last_burst_label.setText(f"Last burst: {sent}/{total} sent, {failed} failed")
            return

    def _set_phase(self, name: str):
        self._cycle_phase = name
        for phase_name, lbl in self.phase_labels.items():
            lbl.setStyleSheet(self._phase_style(phase_name == name))

    def _set_satellite_mode(self, on: bool):
        self._satellite_mode = on
        self.test_box.setVisible(not on)
        self.telemetry_box.setVisible(on)
        if on:
            self.satmode_status.setText("SATELLITE MODE ON -- interact via the GS over real RF")
            self.satmode_status.setStyleSheet(f"color: {WARN}; font-weight: bold;")
        else:
            self.satmode_status.setText("bench mode (debug/test commands available)")
            self.satmode_status.setStyleSheet(f"color: {FG_DIM};")

    def _update_digi_summary(self):
        self.digi_summary.setText(
            f"digipeater: {self._digi_relayed} relayed, {self._digi_duplicate} duplicate")

    def _append_colored(self, line: str):
        upper = line.upper()
        if "FAIL" in upper or "ERR" in upper:
            color = ERR
        elif "CYCLE PHASE" in upper:
            color = ACCENT
        elif line.startswith("DIGI:") or line.startswith("TX ") or line.startswith("BURST"):
            color = WARN
        else:
            color = None
        self.append_log(line, color)

    def append_log(self, line: str, color=None):
        cursor = self.log.textCursor()
        cursor.movePosition(_END_MOVE)
        fmt = QTextCharFormat()
        if color is not None:
            fmt.setForeground(QColor(color))
        cursor.insertText(line + "\n", fmt)
        self.log.setTextCursor(cursor)
        self.log.ensureCursorVisible()

    def closeEvent(self, event):
        self.disconnect_serial()
        super().closeEvent(event)


def main():
    app = QApplication(sys.argv)
    win = SatelliteGui()
    win.show()
    fn = getattr(app, "exec", None)
    sys.exit(fn() if fn is not None else app.exec_())


if __name__ == "__main__":
    main()
