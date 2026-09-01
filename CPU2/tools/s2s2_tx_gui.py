#!/usr/bin/env python3
# @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
# @date   2026-08-30
"""
s2s2_tx_gui.py -- PyQt "backdoor" control panel for the Nucleo-WL55JC over
its USB-serial debug UART: raw TX <hex>, TX power override, beacon on/off,
a full Command Center for the ground station's real CLI keywords
(satellite/gs_main.c), and an audible Morse sidetone reproduction of any
CW beacon the satellite reports sending.

DEBUG_BUILD-only commands (TX/POWER/BEACON) get "UNKNOWN COMMAND" from a
production-built board -- see the architecture README section 9.1. The
Command Center buttons are the ground station's REAL, always-available
CLI keywords (work on either build) -- this tool has no privileged access
of its own; it is exactly as powerful as the firmware it's talking to
lets it be, no more.

Commands sent:
    TX <hex>        -- transmit arbitrary bytes downlink as TYPE_RAW (DEBUG_BUILD)
    POWER <dbm>      -- override TX power, -9..22 dBm (DEBUG_BUILD)
    BEACON ON/OFF    -- toggle all satellite beaconing (DEBUG_BUILD)
    COMMAND, HEARTBEAT, HEALTH, HOUSEKEEPING, BEACON1, BEACON2, ADCS,
    CAMERA, DOWNLOAD, SATLOG, DIGIPEATER, RETRANSMIT, ABORT
                     -- the ground station's real always-on CLI keywords

Works with either PyQt5 or PyQt6 (whichever is installed), pyserial, and
pyqtgraph (for the TX-activity graph). CW sidetone playback additionally
needs sounddevice (optional -- the checkbox just does nothing useful
without it, doesn't crash the GUI).

Install:
    pip install pyserial pyqtgraph PyQt6 sounddevice     # or PyQt5

Run:
    python3 s2s2_tx_gui.py

Usage:
    1. Pick the Nucleo's serial port and baud rate (must match whatever
       uart_debug.c was actually built with -- 115200 by default, or 9600
       if the firmware was built with -DUART_DEBUG_BAUD_9600).
    2. Click Connect.
    3. Type a message (Text mode encodes it as UTF-8 automatically) or
       paste raw hex bytes (Hex mode), then click Send / press Enter --
       this is the raw DEBUG_BUILD "TX <hex>" backdoor.
       Up/Down arrow in the input box recalls previously sent messages.
       "Resend last" repeats the most recent payload with one click.
       "Send file..." reads a local file's raw bytes and sends them
       (rejected up front if it's over the 659-byte payload limit).
    4. "Command Center": one button per real ground-station CLI keyword
       (COMMAND requests a telemetry burst; HEARTBEAT/HEALTH/HOUSEKEEPING/
       BEACON1/BEACON2/ADCS/SATLOG query the satellite; CAMERA/DOWNLOAD/
       DIGIPEATER start a mission; RETRANSMIT asks for gapped frames back;
       ABORT stops whatever mission is active). These work on a
       production-built board too -- they're the satellite's real command
       set, not a debug backdoor.
    5. "Radio Control" (DEBUG_BUILD only): drag the power slider and click
       Apply to send POWER <dbm> (clamped -9..22, the SX126x HP-PA's real
       range); toggle Beacon ON/OFF to silence or resume ALL of the
       satellite's automatic beaconing (5s text ping, 60s CW, 30s
       Beacon1/2). Both reflect the firmware's own confirmation reply,
       not just what you clicked.
    6. "CW Sidetone": when checked, and the satellite reports sending a
       CW beacon ("CW BEACON TX: ..." log line), this GUI SYNTHESIZES and
       plays an audible Morse reproduction of that exact message, timed
       to match the firmware's own cw_beacon.c table (see tools/morse.py)
       -- this is a simulated playback of what was just transmitted, not
       a live over-the-air recording (for that, use s2s2_rx_gui.py's real
       envelope-detected CW Monitor against an actual receiver). WPM,
       sidetone pitch, and volume are all adjustable; "Test tone" plays a
       sample phrase immediately without waiting for a real beacon.
    7. "TX Activity" parses the firmware's own log lines (not just what
       this GUI sent) for every actual radio transmission -- so you see
       real downlink activity even when it wasn't this GUI that
       triggered it.
    8. Anything the Nucleo prints shows up in the log pane, color-coded:
       green for what you sent and "OK ..." replies, red for "ERR"/"FAIL"
       lines, gray for connect/disconnect notices. Timestamp and
       autoscroll are both toggleable. The status bar tracks total
       messages/bytes sent this session.
"""

import os
import re
import sys
import time
from collections import deque

try:
    from PyQt6.QtCore import Qt, QThread, pyqtSignal
    from PyQt6.QtGui import QFont, QTextCursor, QTextCharFormat, QColor
    from PyQt6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QGridLayout, QComboBox, QPushButton, QLineEdit, QPlainTextEdit, QLabel,
        QRadioButton, QButtonGroup, QMessageBox, QStatusBar, QFileDialog,
        QSplitter, QGroupBox, QSlider, QCheckBox, QFrame, QDoubleSpinBox, QSpinBox,
    )
    PYQT_VERSION = 6
except ImportError:
    from PyQt5.QtCore import Qt, QThread, pyqtSignal
    from PyQt5.QtGui import QFont, QTextCursor, QTextCharFormat, QColor
    from PyQt5.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QGridLayout, QComboBox, QPushButton, QLineEdit, QPlainTextEdit, QLabel,
        QRadioButton, QButtonGroup, QMessageBox, QStatusBar, QFileDialog,
        QSplitter, QGroupBox, QSlider, QCheckBox, QFrame, QDoubleSpinBox, QSpinBox,
    )
    PYQT_VERSION = 5

import numpy as np
import pyqtgraph as pg
import serial
import serial.tools.list_ports

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import morse
from audio_player import AudioPlayer

MAX_PAYLOAD_BYTES = 659  # s2s2com.h MAX_PAYLOAD_LEN -- cubus_ax25_fragment's ceiling
BAUD_RATES = [9600, 115200]
DEFAULT_BAUD = 115200
MIN_POWER_DBM = -9    # SX126x HP-PA real range (RadioApp_SetTxPower clamps to this)
MAX_POWER_DBM = 22
DEFAULT_POWER_DBM = 22  # config.h RADIO_TX_POWER_DBM, the production default
ACTIVITY_WINDOW_S = 90   # rolling window kept for the TX-activity graph
ACTIVITY_BIN_S = 1.0     # bin width for the frames/sec bar
CW_AUDIO_SAMPLE_RATE = 8000

# The ground station's real, always-available CLI keywords (satellite/gs_main.c,
# grep str_ieq(command, ...)) -- label, keyword, tooltip.
CLI_COMMANDS = [
    ("Request burst", "COMMAND", "CMD_REQUEST_BURST -- request a telemetry burst"),
    ("Heartbeat", "HEARTBEAT", "CMD_HEARTBEAT -- liveness check"),
    ("Health", "HEALTH", "CMD_GET_HEALTH"),
    ("Housekeeping", "HOUSEKEEPING", "CMD_GET_HOUSEKEEPING"),
    ("Beacon 1", "BEACON1", "CMD_GET_BEACON1 -- power/battery/thermal"),
    ("Beacon 2", "BEACON2", "CMD_GET_BEACON2 -- solar/ADCS"),
    ("ADCS", "ADCS", "CMD_GET_ADCS"),
    ("Camera", "CAMERA", "CMD_RUN_CAMERA mission"),
    ("Download", "DOWNLOAD", "CMD_DOWNLOAD_FILE mission"),
    ("Sat log", "SATLOG", "CMD_GET_SAT_LOG"),
    ("Digipeater", "DIGIPEATER", "CMD_DIGIPEATER -- start the digipeater mission (DPM_ARCHITECTURE.md)"),
    ("Retransmit", "RETRANSMIT", "CMD_REQUEST_RETRANSMIT -- resend gapped tx_history frames"),
    ("Abort mission", "ABORT", "CMD_ABORT_MISSION -- stop whatever mission is active"),
]

_END_MOVE = QTextCursor.MoveOperation.End if PYQT_VERSION == 6 else QTextCursor.End

# -- "backdoor" console aesthetic, matching s2s2_rx_gui.py -----------------
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
QPushButton:checked {{ background-color: #1c4a37; border-color: {WARN}; color: {WARN}; }}
QLineEdit, QComboBox, QPlainTextEdit, QSpinBox, QDoubleSpinBox {{
    background-color: {PANEL}; border: 1px solid #17251f; color: {FG}; selection-background-color: #1c4a37; }}
QCheckBox, QRadioButton {{ color: {FG}; }}
QStatusBar {{ background-color: {PANEL}; color: {FG_DIM}; }}
QLabel {{ color: {FG}; }}
QSlider::groove:horizontal {{ height: 6px; background: #17251f; border-radius: 3px; }}
QSlider::handle:horizontal {{ background: {ACCENT}; width: 14px; margin: -5px 0; border-radius: 7px; }}
QSlider::sub-page:horizontal {{ background: #1c4a37; border-radius: 3px; }}
"""

# Log lines the firmware itself prints for every actual radio TX, whether
# triggered by this GUI, the automatic beacon timer, or a telemetry burst --
# used to drive the "TX Activity" graph off real transmissions, not just
# what this GUI happened to send.
_RE_RADIO_SEND = re.compile(r"RadioApp_Send: len=(\d+)")
_RE_BEACON_TICK = re.compile(r"BEACON TX")
_RE_POWER_ACK = re.compile(r"RadioApp_SetTxPower:\s*(-?\d+)\s*dBm")
_RE_BEACON_STATE = re.compile(r"^BEACON (ON|OFF)$")
_RE_CW_BEACON = re.compile(r'CW BEACON TX: "([^"]*)"')


def run_app_exec(app):
    """exec() on PyQt6, exec_() on PyQt5."""
    fn = getattr(app, "exec", None)
    return fn() if fn is not None else app.exec_()


class HistoryLineEdit(QLineEdit):
    """QLineEdit with shell-style Up/Down history recall."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self._history = []
        self._history_idx = None
        self._draft = ""

    def record_history(self, text: str):
        if text and (not self._history or self._history[-1] != text):
            self._history.append(text)
        self._history_idx = None
        self._draft = ""

    def keyPressEvent(self, event):
        key = event.key()
        if key == Qt.Key.Key_Up if PYQT_VERSION == 6 else Qt.Key_Up:
            self._navigate_history(-1)
            return
        if key == Qt.Key.Key_Down if PYQT_VERSION == 6 else Qt.Key_Down:
            self._navigate_history(1)
            return
        super().keyPressEvent(event)

    def _navigate_history(self, direction: int):
        if not self._history:
            return
        if self._history_idx is None:
            if direction > 0:
                return
            self._draft = self.text()
            self._history_idx = len(self._history) - 1
        else:
            self._history_idx += direction
        if self._history_idx < 0:
            self._history_idx = 0
        if self._history_idx >= len(self._history):
            self._history_idx = None
            self.setText(self._draft)
            return
        self.setText(self._history[self._history_idx])


class SerialReader(QThread):
    data_received = pyqtSignal(bytes)
    error = pyqtSignal(str)

    def __init__(self, ser):
        super().__init__()
        self._ser = ser
        self._running = True

    def run(self):
        while self._running:
            try:
                n = self._ser.in_waiting
                data = self._ser.read(n if n else 1)
                if data:
                    self.data_received.emit(data)
            except serial.SerialException as e:
                self.error.emit(str(e))
                break

    def stop(self):
        self._running = False
        self.wait(1000)


class TxActivityPanel(QWidget):
    """Live graph of real radio transmissions parsed from the firmware's
    own log output -- frames/sec (bar) and cumulative bytes sent (line).
    Driven by record_send()/record_beacon(), fed from parsed log lines,
    not from this GUI's own send button -- so it reflects the satellite's
    real TX activity (beacons included) even when idle."""

    def __init__(self):
        super().__init__()
        self._start_t = time.monotonic()
        self._events = deque()  # (t_offset, bytes)
        self._cum_bytes = 0
        self._cum_bytes_hist = deque(maxlen=600)
        self._cum_time_hist = deque(maxlen=600)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)

        pg.setConfigOptions(antialias=True)

        self.rate_plot = pg.PlotWidget(title="Frames sent / sec")
        self.rate_plot.setBackground(PANEL)
        self.rate_plot.showGrid(x=True, y=True, alpha=0.25)
        self.rate_bar = pg.BarGraphItem(x=[0], height=[0], width=0.8, brush=FG)
        self.rate_plot.addItem(self.rate_bar)
        layout.addWidget(self.rate_plot, 1)

        self.bytes_plot = pg.PlotWidget(title="Cumulative bytes transmitted")
        self.bytes_plot.setBackground(PANEL)
        self.bytes_plot.showGrid(x=True, y=True, alpha=0.25)
        self.bytes_curve = self.bytes_plot.plot(pen=pg.mkPen(ACCENT, width=1.6))
        layout.addWidget(self.bytes_plot, 1)

        self.total_label = QLabel("0 frames / 0 bytes total")
        self.total_label.setStyleSheet(f"color: {ACCENT}; font-weight: bold;")
        layout.addWidget(self.total_label)

    def reset(self):
        self._start_t = time.monotonic()
        self._events.clear()
        self._cum_bytes = 0
        self._cum_bytes_hist.clear()
        self._cum_time_hist.clear()
        self.total_label.setText("0 frames / 0 bytes total")

    def record_send(self, n_bytes: int):
        t = time.monotonic() - self._start_t
        self._events.append((t, n_bytes))
        cutoff = t - ACTIVITY_WINDOW_S
        while self._events and self._events[0][0] < cutoff:
            self._events.popleft()
        self._cum_bytes += n_bytes
        self._cum_time_hist.append(t)
        self._cum_bytes_hist.append(self._cum_bytes)
        self._redraw(t)

    def _redraw(self, now_t: float):
        if not self._events:
            self.rate_bar.setOpts(x=[0], height=[0], width=0.8)
        else:
            n_bins = max(1, int(ACTIVITY_WINDOW_S / ACTIVITY_BIN_S))
            counts = np.zeros(n_bins)
            oldest = now_t - ACTIVITY_WINDOW_S
            for t, _ in self._events:
                idx = int((t - oldest) / ACTIVITY_BIN_S)
                if 0 <= idx < n_bins:
                    counts[idx] += 1
            xs = oldest + np.arange(n_bins) * ACTIVITY_BIN_S
            self.rate_bar.setOpts(x=xs, height=counts, width=ACTIVITY_BIN_S * 0.9)
            self.rate_plot.setXRange(oldest, now_t + 1)

        if self._cum_time_hist:
            self.bytes_curve.setData(np.array(self._cum_time_hist), np.array(self._cum_bytes_hist))

        total_frames = len(self._cum_bytes_hist)
        self.total_label.setText(f"{total_frames} frame(s) / {self._cum_bytes} byte(s) total (this session)")


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("S2S2 Nucleo TX -- Debug Backdoor Console")
        self.resize(1360, 820)

        self.ser = None
        self.reader = None
        self.last_sent_payload = None
        self.bytes_sent_total = 0
        self.messages_sent_total = 0
        self._line_buf = ""
        self._current_power_dbm = DEFAULT_POWER_DBM
        self._beacon_enabled = True
        self._show_timestamps = False
        self._autoscroll = True
        self._audio = AudioPlayer(sample_rate=CW_AUDIO_SAMPLE_RATE)

        central = QWidget()
        self.setCentralWidget(central)
        outer = QVBoxLayout(central)

        # --- connection row ---
        conn_row = QHBoxLayout()
        conn_row.addWidget(QLabel("Port:"))
        self.port_combo = QComboBox()
        self.port_combo.setMinimumWidth(200)
        conn_row.addWidget(self.port_combo)

        self.refresh_btn = QPushButton("Refresh")
        self.refresh_btn.clicked.connect(self.refresh_ports)
        conn_row.addWidget(self.refresh_btn)

        conn_row.addWidget(QLabel("Baud:"))
        self.baud_combo = QComboBox()
        for b in BAUD_RATES:
            self.baud_combo.addItem(str(b), b)
        self.baud_combo.setCurrentIndex(BAUD_RATES.index(DEFAULT_BAUD))
        conn_row.addWidget(self.baud_combo)

        self.connect_btn = QPushButton("Connect")
        self.connect_btn.clicked.connect(self.toggle_connection)
        conn_row.addWidget(self.connect_btn)

        conn_row.addStretch(1)

        self.timestamp_check = QCheckBox("Timestamps")
        self.timestamp_check.toggled.connect(self._on_timestamp_toggled)
        conn_row.addWidget(self.timestamp_check)

        self.autoscroll_check = QCheckBox("Autoscroll")
        self.autoscroll_check.setChecked(True)
        self.autoscroll_check.toggled.connect(self._on_autoscroll_toggled)
        conn_row.addWidget(self.autoscroll_check)

        outer.addLayout(conn_row)

        # --- main split: (send/log) | (radio control + CW + activity) ---
        main_split = QSplitter(Qt.Orientation.Horizontal if PYQT_VERSION == 6 else Qt.Horizontal)

        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(0, 0, 0, 0)

        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setFont(QFont("Menlo", 11) if sys.platform == "darwin" else QFont("Consolas", 10))
        left_layout.addWidget(self.log, 1)

        clear_row = QHBoxLayout()
        clear_row.addStretch(1)
        clear_btn = QPushButton("Clear log")
        clear_btn.clicked.connect(self.log.clear)
        clear_row.addWidget(clear_btn)
        left_layout.addLayout(clear_row)

        cmd_box = QGroupBox("Command Center  (real ground-station CLI keywords -- work on any build)")
        cmd_grid = QGridLayout(cmd_box)
        self.cli_buttons = []
        for i, (label, keyword, tooltip) in enumerate(CLI_COMMANDS):
            btn = QPushButton(label)
            btn.setToolTip(f"{tooltip}\nSends: {keyword}")
            btn.clicked.connect(lambda _checked, kw=keyword: self.send_cli_command(kw))
            cmd_grid.addWidget(btn, i // 5, i % 5)
            self.cli_buttons.append(btn)
        left_layout.addWidget(cmd_box)

        mode_row = QHBoxLayout()
        mode_row.addWidget(QLabel("Raw TX mode:"))
        self.text_radio = QRadioButton("Text")
        self.hex_radio = QRadioButton("Hex")
        self.text_radio.setChecked(True)
        mode_group = QButtonGroup(self)
        mode_group.addButton(self.text_radio)
        mode_group.addButton(self.hex_radio)
        mode_row.addWidget(self.text_radio)
        mode_row.addWidget(self.hex_radio)
        mode_row.addStretch(1)
        self.byte_count_label = QLabel("0 / %d bytes" % MAX_PAYLOAD_BYTES)
        mode_row.addWidget(self.byte_count_label)
        left_layout.addLayout(mode_row)

        send_row = QHBoxLayout()
        self.input = HistoryLineEdit()
        self.input.setPlaceholderText(
            "DEBUG_BUILD raw TX <hex> backdoor -- message or hex bytes... (Up/Down for history)")
        self.input.returnPressed.connect(self.send_message)
        self.input.textChanged.connect(self.update_byte_count)
        send_row.addWidget(self.input, 1)

        self.send_btn = QPushButton("Send")
        self.send_btn.clicked.connect(self.send_message)
        send_row.addWidget(self.send_btn)

        self.resend_btn = QPushButton("Resend last")
        self.resend_btn.setEnabled(False)
        self.resend_btn.clicked.connect(self.resend_last)
        send_row.addWidget(self.resend_btn)

        self.send_file_btn = QPushButton("Send file...")
        self.send_file_btn.clicked.connect(self.send_file)
        send_row.addWidget(self.send_file_btn)
        left_layout.addLayout(send_row)

        main_split.addWidget(left)

        # --- right: radio control + CW sidetone + TX activity ---
        right = QWidget()
        right_layout = QVBoxLayout(right)
        right_layout.setContentsMargins(0, 0, 0, 0)

        radio_box = QGroupBox("Radio Control  (DEBUG_BUILD only -- ignored/UNKNOWN on production firmware)")
        radio_layout = QVBoxLayout(radio_box)

        power_row = QHBoxLayout()
        power_row.addWidget(QLabel("TX power:"))
        self.power_slider = QSlider(Qt.Orientation.Horizontal if PYQT_VERSION == 6 else Qt.Horizontal)
        self.power_slider.setMinimum(MIN_POWER_DBM)
        self.power_slider.setMaximum(MAX_POWER_DBM)
        self.power_slider.setValue(DEFAULT_POWER_DBM)
        self.power_slider.valueChanged.connect(self._on_power_slider_changed)
        power_row.addWidget(self.power_slider, 1)
        self.power_value_label = QLabel(f"{DEFAULT_POWER_DBM:+d} dBm")
        self.power_value_label.setMinimumWidth(60)
        self.power_value_label.setStyleSheet(f"color: {ACCENT}; font-weight: bold;")
        power_row.addWidget(self.power_value_label)
        self.power_apply_btn = QPushButton("Apply")
        self.power_apply_btn.clicked.connect(self.apply_power)
        power_row.addWidget(self.power_apply_btn)
        radio_layout.addLayout(power_row)

        self.power_confirmed_label = QLabel("Confirmed by board: -- (send Apply)")
        self.power_confirmed_label.setStyleSheet(f"color: {FG_DIM};")
        radio_layout.addWidget(self.power_confirmed_label)

        radio_layout.addWidget(_hline())

        beacon_row = QHBoxLayout()
        beacon_row.addWidget(QLabel("All beaconing (text/CW/Beacon1/Beacon2):"))
        self.beacon_on_btn = QPushButton("ON")
        self.beacon_on_btn.setCheckable(True)
        self.beacon_on_btn.setChecked(True)
        self.beacon_on_btn.clicked.connect(lambda: self.set_beacon(True))
        beacon_row.addWidget(self.beacon_on_btn)
        self.beacon_off_btn = QPushButton("OFF")
        self.beacon_off_btn.setCheckable(True)
        self.beacon_off_btn.clicked.connect(lambda: self.set_beacon(False))
        beacon_row.addWidget(self.beacon_off_btn)
        beacon_row.addStretch(1)
        radio_layout.addLayout(beacon_row)

        self.beacon_confirmed_label = QLabel("Confirmed by board: -- (click ON/OFF)")
        self.beacon_confirmed_label.setStyleSheet(f"color: {FG_DIM};")
        radio_layout.addWidget(self.beacon_confirmed_label)

        right_layout.addWidget(radio_box)

        # --- CW sidetone panel ---
        cw_box = QGroupBox("CW Sidetone  (simulated playback of a reported CW beacon)")
        cw_layout = QVBoxLayout(cw_box)

        self.cw_enable_check = QCheckBox("Play audible Morse when a CW beacon is sent")
        cw_layout.addWidget(self.cw_enable_check)

        cw_row1 = QHBoxLayout()
        cw_row1.addWidget(QLabel("WPM:"))
        self.cw_wpm_spin = QSpinBox()
        self.cw_wpm_spin.setRange(5, 60)
        self.cw_wpm_spin.setValue(morse.DEFAULT_WPM)
        self.cw_wpm_spin.setToolTip("Must match the firmware's CW_DEFAULT_WPM (core/Inc/cw_beacon.h) for accurate timing.")
        cw_row1.addWidget(self.cw_wpm_spin)
        cw_row1.addWidget(QLabel("Pitch (Hz):"))
        self.cw_pitch_spin = QSpinBox()
        self.cw_pitch_spin.setRange(200, 1200)
        self.cw_pitch_spin.setValue(morse.DEFAULT_SIDETONE_HZ)
        cw_row1.addWidget(self.cw_pitch_spin)
        cw_layout.addLayout(cw_row1)

        cw_row2 = QHBoxLayout()
        cw_row2.addWidget(QLabel("Volume:"))
        self.cw_volume_slider = QSlider(Qt.Orientation.Horizontal if PYQT_VERSION == 6 else Qt.Horizontal)
        self.cw_volume_slider.setMinimum(0)
        self.cw_volume_slider.setMaximum(100)
        self.cw_volume_slider.setValue(30)
        cw_row2.addWidget(self.cw_volume_slider, 1)
        self.cw_test_btn = QPushButton("Test tone")
        self.cw_test_btn.setToolTip('Plays "DE NEPSAT TEST" immediately, without waiting for a real beacon.')
        self.cw_test_btn.clicked.connect(self.play_cw_test_tone)
        cw_row2.addWidget(self.cw_test_btn)
        cw_layout.addLayout(cw_row2)

        self.cw_last_label = QLabel("Last CW beacon heard: --")
        self.cw_last_label.setStyleSheet(f"color: {FG_DIM};")
        cw_layout.addWidget(self.cw_last_label)

        right_layout.addWidget(cw_box)

        activity_box = QGroupBox("TX Activity  (parsed from the board's own RadioApp_Send log lines)")
        activity_layout = QVBoxLayout(activity_box)
        self.activity = TxActivityPanel()
        activity_layout.addWidget(self.activity)
        right_layout.addWidget(activity_box, 1)

        main_split.addWidget(right)
        main_split.setSizes([760, 600])

        outer.addWidget(main_split, 1)

        self.status = QStatusBar()
        self.setStatusBar(self.status)

        self.set_connected_ui(False)
        self.refresh_ports()
        self.status.showMessage("Disconnected")

    # ---------------------------------------------------------------
    def refresh_ports(self):
        current = self.port_combo.currentData()
        self.port_combo.clear()
        for p in serial.tools.list_ports.comports():
            self.port_combo.addItem("%s (%s)" % (p.device, p.description), p.device)
        if current:
            idx = self.port_combo.findData(current)
            if idx >= 0:
                self.port_combo.setCurrentIndex(idx)

    def set_connected_ui(self, connected):
        self.port_combo.setEnabled(not connected)
        self.baud_combo.setEnabled(not connected)
        self.refresh_btn.setEnabled(not connected)
        self.connect_btn.setText("Disconnect" if connected else "Connect")
        self.send_btn.setEnabled(connected)
        self.input.setEnabled(connected)
        self.send_file_btn.setEnabled(connected)
        self.resend_btn.setEnabled(connected and self.last_sent_payload is not None)
        self.power_apply_btn.setEnabled(connected)
        self.power_slider.setEnabled(connected)
        self.beacon_on_btn.setEnabled(connected)
        self.beacon_off_btn.setEnabled(connected)
        for btn in self.cli_buttons:
            btn.setEnabled(connected)

    def toggle_connection(self):
        if self.ser is not None:
            self.disconnect_serial()
        else:
            self.connect_serial()

    def connect_serial(self):
        port = self.port_combo.currentData()
        if not port:
            QMessageBox.warning(self, "No port", "No serial port selected. Plug in the Nucleo and click Refresh.")
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
        self.bytes_sent_total = 0
        self.messages_sent_total = 0
        self.activity.reset()
        self.power_confirmed_label.setText("Confirmed by board: -- (send Apply)")
        self.beacon_confirmed_label.setText("Confirmed by board: -- (click ON/OFF)")
        self.status.showMessage("Connected to %s @ %d" % (port, baud))
        self.append_log("--- connected to %s @ %d ---" % (port, baud), FG_DIM)

    def disconnect_serial(self):
        if self.reader:
            self.reader.stop()
            self.reader = None
        if self.ser:
            try:
                self.ser.close()
            except serial.SerialException:
                pass
            self.ser = None
        self.set_connected_ui(False)
        self.status.showMessage("Disconnected")
        self.append_log("--- disconnected ---", FG_DIM)

    def on_serial_error(self, message):
        self.append_log("--- serial error: %s ---" % message, ERR)
        self.disconnect_serial()

    def on_data_received(self, data: bytes):
        text = data.decode("latin-1", errors="replace")
        self._line_buf += text
        # Buffer until we have complete lines -- the firmware's own text
        # doesn't align to read() chunk boundaries, so coloring a whole raw
        # chunk (which might span two log lines, or half of one) would
        # color unrelated text together. Split on any of \r\n, \r, \n.
        parts = self._line_buf.replace("\r\n", "\n").replace("\r", "\n").split("\n")
        self._line_buf = parts[-1]
        for line in parts[:-1]:
            self._handle_incoming_line(line)

    def _handle_incoming_line(self, line: str):
        self._append_colored_incoming_line(line)

        m = _RE_RADIO_SEND.search(line)
        if m:
            self.activity.record_send(int(m.group(1)))

        if _RE_BEACON_TICK.search(line):
            pass  # already counted via the RadioApp_Send line it triggers

        m = _RE_POWER_ACK.search(line)
        if m:
            dbm = int(m.group(1))
            self._current_power_dbm = dbm
            self.power_confirmed_label.setText(f"Confirmed by board: {dbm:+d} dBm")
            self.power_confirmed_label.setStyleSheet(f"color: {ACCENT}; font-weight: bold;")

        m = _RE_BEACON_STATE.match(line.strip())
        if m:
            on = (m.group(1) == "ON")
            self._beacon_enabled = on
            self.beacon_on_btn.setChecked(on)
            self.beacon_off_btn.setChecked(not on)
            self.beacon_confirmed_label.setText(f"Confirmed by board: {'ON' if on else 'OFF'}")
            self.beacon_confirmed_label.setStyleSheet(
                f"color: {ACCENT if on else WARN}; font-weight: bold;")

        m = _RE_CW_BEACON.search(line)
        if m:
            message = m.group(1)
            self.cw_last_label.setText(f"Last CW beacon heard: \"{message}\"")
            self.cw_last_label.setStyleSheet(f"color: {ACCENT}; font-weight: bold;")
            if self.cw_enable_check.isChecked():
                self._play_cw_message(message)

    def _append_colored_incoming_line(self, line: str):
        upper = line.upper()
        if "ERR" in upper or "FAIL" in upper:
            color = ERR
        elif line.startswith("OK ") or " OK " in line:
            color = ACCENT
        else:
            color = None
        self.append_log(line, color)

    def append_log(self, line: str, color=None):
        if self._show_timestamps:
            line = time.strftime("[%H:%M:%S] ") + line
        cursor = self.log.textCursor()
        cursor.movePosition(_END_MOVE)
        fmt = QTextCharFormat()
        if color is not None:
            fmt.setForeground(QColor(color))
        cursor.insertText(line + "\n", fmt)
        self.log.setTextCursor(cursor)
        if self._autoscroll:
            self.log.ensureCursorVisible()

    def _on_timestamp_toggled(self, checked: bool):
        self._show_timestamps = checked

    def _on_autoscroll_toggled(self, checked: bool):
        self._autoscroll = checked

    # ---------------------------------------------------------------
    def _on_power_slider_changed(self, value: int):
        self.power_value_label.setText(f"{value:+d} dBm")

    def apply_power(self):
        if self.ser is None:
            return
        dbm = self.power_slider.value()
        line = ("POWER %d\r\n" % dbm).encode("ascii")
        try:
            self.ser.write(line)
        except serial.SerialException as e:
            self.on_serial_error(str(e))
            return
        self.append_log("--- sent POWER %d ---" % dbm, ACCENT)

    def set_beacon(self, enable: bool):
        if self.ser is None:
            self.beacon_on_btn.setChecked(self._beacon_enabled)
            self.beacon_off_btn.setChecked(not self._beacon_enabled)
            return
        self.beacon_on_btn.setChecked(enable)
        self.beacon_off_btn.setChecked(not enable)
        line = (("BEACON ON" if enable else "BEACON OFF") + "\r\n").encode("ascii")
        try:
            self.ser.write(line)
        except serial.SerialException as e:
            self.on_serial_error(str(e))
            return
        self.append_log("--- sent BEACON %s ---" % ("ON" if enable else "OFF"), ACCENT)

    def send_cli_command(self, keyword: str):
        if self.ser is None:
            return
        line = (keyword + "\r\n").encode("ascii")
        try:
            self.ser.write(line)
        except serial.SerialException as e:
            self.on_serial_error(str(e))
            return
        self.append_log("--- sent %s ---" % keyword, ACCENT)

    # ---------------------------------------------------------------
    def _play_cw_message(self, message: str):
        try:
            if not self._audio.is_running:
                self._audio.start()
        except Exception as e:  # noqa: broad -- any audio-device failure (missing lib, no output device, PortAudio error) should just disable the feature, never crash the GUI
            self.append_log(f"--- CW sidetone unavailable: {e} ---", WARN)
            self.cw_enable_check.setChecked(False)
            return
        audio = morse.message_to_audio(
            message, wpm=self.cw_wpm_spin.value(), sidetone_hz=self.cw_pitch_spin.value(),
            sample_rate=CW_AUDIO_SAMPLE_RATE, volume=self.cw_volume_slider.value() / 100.0)
        self._audio.push(audio)

    def play_cw_test_tone(self):
        self._play_cw_message("DE NEPSAT TEST")

    # ---------------------------------------------------------------
    def current_payload_bytes(self):
        """Returns (ok, payload_bytes_or_error_message)."""
        text = self.input.text()
        if self.hex_radio.isChecked():
            cleaned = "".join(text.split()).replace("0x", "").replace("0X", "")
            if len(cleaned) % 2 != 0:
                return False, "hex string must have an even number of characters"
            try:
                payload = bytes.fromhex(cleaned)
            except ValueError:
                return False, "not valid hex (only 0-9/a-f allowed)"
            return True, payload
        else:
            payload = text.encode("utf-8")
            return True, payload

    def update_byte_count(self):
        ok, result = self.current_payload_bytes()
        n = len(result) if ok else 0
        over = n > MAX_PAYLOAD_BYTES
        self.byte_count_label.setText("%d / %d bytes%s" % (n, MAX_PAYLOAD_BYTES, "  (TOO LONG)" if over else ""))
        self.byte_count_label.setStyleSheet(f"color: {ERR};" if (over or not ok) else "")

    def send_message(self):
        if self.ser is None:
            return
        ok, result = self.current_payload_bytes()
        if not ok:
            QMessageBox.warning(self, "Bad input", result)
            return
        payload = result
        if len(payload) == 0:
            return
        if self._send_payload(payload):
            self.input.record_history(self.input.text())
            self.input.clear()

    def resend_last(self):
        if self.last_sent_payload is not None:
            self._send_payload(self.last_sent_payload)

    def send_file(self):
        if self.ser is None:
            return
        path, _ = QFileDialog.getOpenFileName(self, "Select file to send")
        if not path:
            return
        try:
            with open(path, "rb") as f:
                payload = f.read()
        except OSError as e:
            QMessageBox.critical(self, "Read failed", str(e))
            return
        if len(payload) > MAX_PAYLOAD_BYTES:
            QMessageBox.warning(
                self, "Too long",
                "%s is %d bytes, exceeding the %d-byte S2S2 payload limit."
                % (os.path.basename(path), len(payload), MAX_PAYLOAD_BYTES))
            return
        if self._send_payload(payload):
            self.append_log("--- sent file %s (%d bytes) ---" % (os.path.basename(path), len(payload)), FG_DIM)

    def _send_payload(self, payload: bytes) -> bool:
        """Writes one TX <hex> command. Returns True on success."""
        if len(payload) > MAX_PAYLOAD_BYTES:
            QMessageBox.warning(self, "Too long",
                                 "%d bytes exceeds the %d-byte S2S2 payload limit." % (len(payload), MAX_PAYLOAD_BYTES))
            return False

        hex_str = payload.hex()
        line = ("TX " + hex_str + "\r\n").encode("ascii")
        try:
            self.ser.write(line)
        except serial.SerialException as e:
            self.on_serial_error(str(e))
            return False

        self.last_sent_payload = payload
        self.resend_btn.setEnabled(True)
        self.bytes_sent_total += len(payload)
        self.messages_sent_total += 1
        self.status.showMessage(
            "Connected -- %d message(s) sent, %d byte(s) total"
            % (self.messages_sent_total, self.bytes_sent_total))
        self.append_log("--- sent %d byte(s) as TX %s ---" % (len(payload), hex_str), FG)
        return True

    def closeEvent(self, event):
        self.disconnect_serial()
        self._audio.stop()
        event.accept()


def _hline():
    line = QFrame()
    line.setFrameShape(QFrame.Shape.HLine if PYQT_VERSION == 6 else QFrame.HLine)
    line.setStyleSheet("color: #17251f;")
    return line


def main():
    app = QApplication(sys.argv)
    app.setStyleSheet(DARK_STYLESHEET)
    win = MainWindow()
    win.show()
    sys.exit(run_app_exec(app))


if __name__ == "__main__":
    main()
