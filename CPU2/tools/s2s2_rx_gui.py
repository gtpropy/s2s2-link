#!/usr/bin/env python3
"""
s2s2_rx_gui.py -- PyQt receiver GUI for the S2S2 GMSK/AX.25/FX.25 downlink,
using an RTL-SDR dongle (or a recorded IQ file) as the front end.

Pipeline: RTL-SDR/file IQ -> gmsk_demod.py (FM discriminator, resample to
fixed samples/symbol, brute-force polarity/bit-order/timing-phase search,
sync-word correlation) -> s2s2_decoder.py (FX.25 RS(255,223)-shortened
correction, AX.25 FCS check, S2S2 fragment reassembly, command/telemetry
pretty-print). See both modules' docstrings for the exact PHY parameters
this matches (core/Src/radio_app.c) and an honest note on what could not
be verified without real hardware (there is none in this environment --
see test_demod.py for the synthetic self-test that stands in for it).

Works with either PyQt5 or PyQt6, numpy, scipy, pyqtgraph, and (optional,
only needed for the RTL-SDR source, not for File mode) pyrtlsdr.

Install:
    pip install numpy scipy pyqtgraph PyQt6      # or PyQt5
    pip install pyrtlsdr                         # optional, for a real RTL-SDR

Run:
    python3 s2s2_rx_gui.py

Usage:
    1. Pick a source: "RTL-SDR" (live dongle) or "IQ file" (a capture
       recorded with rtl_sdr, or one made with make_demo_capture.py for
       a hardware-free test drive).
    2. Set frequency (default matches config.h's DEFAULT_TX_FREQUENCY_HZ,
       435.000 MHz -- the satellite's downlink/beacon frequency, i.e. what
       a ground receiver needs; DEFAULT_RX_FREQUENCY_HZ, 437.375 MHz, is
       the *satellite's* listening frequency for uplink commands, not
       useful here unless you're monitoring the ground station's own
       uplink instead) and sample rate (240000 recommended -- an exact 25
       samples/symbol at 9600 baud; other rates work too via resampling).
    3. Click Start. Decoded packets appear in the table AND get their full
       field breakdown auto-appended live to the right-hand detail pane
       (Beacon1/2, ACK, command, or raw hex/ASCII for TYPE_RAW/unknown --
       click a row to jump back to that entry). The "Signal / Decode
       Quality" panel tracks search mode (full-scan vs locked), the
       locked (phase, polarity, bit-order) combination once one decode
       succeeds, running decoded/rejected counts, success rate, and a
       relative signal-power reading (dBFS) for tuning antenna placement.
    4. The "RF Analyzer" tabs on the right show a live spectrum plot and
       scrolling waterfall of the tuned IQ (handy for confirming the
       satellite's carrier is actually where you think it is and how
       strong it is above the noise floor), plus rolling link-quality
       graphs: signal power, decoded/rejected rate per chunk, and success
       rate over time -- the same "success rate" number in the stats bar,
       but watchable as it recovers after a lock-loss event instead of
       staying a single misleading cumulative percentage.
    5. The frame log stays quiet by default once locked -- only real
       decodes, partial fragments, and lock-acquired/lock-lost events are
       shown, since most of the other ~99 combinations tried per chunk
       during a full-scan were never going to be right anyway. Check
       "Verbose log" to see every individual rejection if you need to
       debug the demod pipeline itself.

Note on LOCK_MISS_LIMIT: earlier versions of this GUI dropped its lock
after 2 seconds of silence (4 misses x 0.5s chunks), which is *shorter*
than the satellite's 5-second beacon interval -- so between every beacon
it fell back to a full 100-combination brute-force scan, and the resulting
flood of noise-driven false sync hits (each counted as "rejected") tanked
the cumulative success-rate reading even on a perfectly healthy link. Bumped
to comfortably outlast a missed beacon or two; see the constant below.
"""
import os
import sys
import time
import traceback
from collections import deque

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

try:
    from PyQt6.QtCore import Qt, QThread, pyqtSignal
    from PyQt6.QtGui import QFont, QTextCursor
    from PyQt6.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QComboBox, QPushButton, QLineEdit, QPlainTextEdit, QLabel,
        QMessageBox, QStatusBar, QTableWidget, QTableWidgetItem, QSplitter,
        QFileDialog, QHeaderView, QGroupBox, QFormLayout, QCheckBox,
        QTabWidget,
    )
    PYQT_VERSION = 6
except ImportError:
    from PyQt5.QtCore import Qt, QThread, pyqtSignal
    from PyQt5.QtGui import QFont, QTextCursor
    from PyQt5.QtWidgets import (
        QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
        QComboBox, QPushButton, QLineEdit, QPlainTextEdit, QLabel,
        QMessageBox, QStatusBar, QTableWidget, QTableWidgetItem, QSplitter,
        QFileDialog, QHeaderView, QGroupBox, QFormLayout, QCheckBox,
        QTabWidget,
    )
    PYQT_VERSION = 5

import numpy as np
import pyqtgraph as pg

import gmsk_demod as gd
import s2s2_decoder as dec

DEFAULT_FREQ_HZ = 435000000     # config.h DEFAULT_TX_FREQUENCY_HZ -- what the
                                 # SATELLITE transmits its downlink/beacon on.
                                 # (DEFAULT_RX_FREQUENCY_HZ, 437.375 MHz, is what
                                 # the satellite LISTENS on for uplink commands --
                                 # a ground receiver needs the satellite's TX freq.)
DEFAULT_SAMPLE_RATE = 240000    # exact 25 samples/symbol at 9600 baud
CHUNK_SECONDS = 0.5
MAX_DEDUPE = 256
LOCK_MISS_LIMIT = 16            # 16 x 0.5s = 8s -- outlasts a missed 5s beacon (see module docstring)
LOCK_PHASE_WINDOW = 1           # +-N phases around the locked one, in the narrow search
SPECTRUM_NFFT = 4096
WATERFALL_ROWS = 160
STATS_HISTORY = 180             # rolling points kept for the link-quality graphs (~90s at 0.5s/chunk)

# -- "backdoor" console aesthetic -----------------------------------------
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
QLineEdit, QComboBox, QPlainTextEdit, QTableWidget {{
    background-color: {PANEL}; border: 1px solid #17251f; color: {FG}; selection-background-color: #1c4a37; }}
QHeaderView::section {{ background-color: #0f1a15; color: {ACCENT}; border: 1px solid #17251f; padding: 3px; }}
QTabWidget::pane {{ border: 1px solid #17251f; }}
QTabBar::tab {{ background: {PANEL}; color: {FG_DIM}; padding: 6px 14px; border: 1px solid #17251f; }}
QTabBar::tab:selected {{ background: #10241c; color: {ACCENT}; }}
QCheckBox {{ color: {FG}; }}
QStatusBar {{ background-color: {PANEL}; color: {FG_DIM}; }}
QLabel {{ color: {FG}; }}
"""


def run_app_exec(app):
    fn = getattr(app, "exec", None)
    return fn() if fn is not None else app.exec_()


class RxWorker(QThread):
    frame_event = pyqtSignal(str)               # raw log lines
    packet_complete = pyqtSignal(dict, dict)     # (pkt, decoded)
    stats_update = pyqtSignal(dict)              # live signal-quality stats
    error = pyqtSignal(str)
    finished_ok = pyqtSignal()

    def __init__(self, source_kind, source_args, sample_rate, verbose=False):
        super().__init__()
        self.source_kind = source_kind
        self.source_args = source_args
        self.sample_rate = sample_rate
        self.verbose = verbose
        self._running = True
        self.reassembler = dec.Reassembler()
        self._seen = deque(maxlen=MAX_DEDUPE)
        self._seen_set = set()
        # A single physical frame is often independently recovered at
        # several adjacent timing phases (see gmsk_demod.py's brute-force
        # phase search) -- each is a genuine, separately byte-perfect
        # decode of the same on-air message, not a decode error, but the
        # GUI should only report a completed message once.
        self._completed_seen = deque(maxlen=MAX_DEDUPE)
        self._completed_seen_set = set()

        # Combo-locking: once a real decode succeeds at a given (phase,
        # invert, order), subsequent chunks only search a narrow window
        # around it instead of all 100 combinations -- much cheaper, and
        # it stops flooding the log with rejections from combinations
        # that were never going to be right. Falls back to a full
        # re-scan after a few consecutive empty narrow searches (e.g.
        # the signal dropped out, or its timing drifted further than the
        # narrow window covers).
        self._locked = None            # (phase, invert, order) or None
        self._lock_miss_streak = 0

        self._decoded_count = 0
        self._rejected_count = 0
        self._candidate_count = 0

    def stop(self):
        self._running = False

    def _mark_seen(self, frame_bytes: bytes) -> bool:
        """Returns True if new (not a duplicate from an overlapping
        processing window)."""
        if frame_bytes in self._seen_set:
            return False
        if len(self._seen) == self._seen.maxlen:
            old = self._seen.popleft()
            self._seen_set.discard(old)
        self._seen.append(frame_bytes)
        self._seen_set.add(frame_bytes)
        return True

    def _process_buffer(self, iq: np.ndarray):
        if self._locked is not None:
            phase, invert, order = self._locked
            near = sorted({(phase + d) % gd.SPS for d in range(-LOCK_PHASE_WINDOW, LOCK_PHASE_WINDOW + 1)})
            candidates = gd.extract_candidate_frames(
                iq, self.sample_rate, phases=near, orders=[order], inverts=[invert])
            searching = "locked"
        else:
            candidates = gd.extract_candidate_frames(iq, self.sample_rate)
            searching = "full-scan"

        self._candidate_count += len(candidates)
        chunk_decoded = 0
        chunk_rejected = 0

        for c in candidates:
            frame = c["frame"]
            if not self._mark_seen(frame):
                continue
            combo = f"order={c['order']} invert={c['invert']} phase={c['phase']}"
            status, info = self.reassembler.accept_frame(frame)

            if status == "rejected":
                chunk_rejected += 1
                self._rejected_count += 1
                if self.verbose:
                    self.frame_event.emit(f"[{combo}] rejected: {info}")
                continue

            # A real decode (partial or complete) confirms this combo is
            # the right one -- lock onto it (or refresh the lock) so the
            # next chunk searches narrow instead of brute-forcing again.
            self._locked = (c["phase"], c["invert"], c["order"])
            self._lock_miss_streak = 0

            if status == "partial":
                self.frame_event.emit(
                    f"[{combo}] fragment {info['frag_index'] + 1}/{info['frag_total']} "
                    f"accepted (seq={info['seq_num']} session={info['session_id']}, "
                    f"rs_corrected={info['rs_bytes_corrected']})")
                continue

            # complete
            msg_key = (info["type"], info["src_id"], info["dst_id"],
                       info["seq_num"], info["session_id"])
            if msg_key in self._completed_seen_set:
                if self.verbose:
                    self.frame_event.emit(f"[{combo}] (duplicate decode of already-reported "
                                           f"seq={info['seq_num']} session={info['session_id']}, skipped)")
                continue
            if len(self._completed_seen) == self._completed_seen.maxlen:
                old = self._completed_seen.popleft()
                self._completed_seen_set.discard(old)
            self._completed_seen.append(msg_key)
            self._completed_seen_set.add(msg_key)

            self._decoded_count += 1
            chunk_decoded += 1
            decoded = dec.decode_payload(info)
            self.frame_event.emit(
                f"[{combo}] MESSAGE COMPLETE type={decoded['type_name']} "
                f"src={info['src_id']} dst={info['dst_id']} seq={info['seq_num']} "
                f"session={info['session_id']} rs_corrected={info['rs_bytes_corrected']} "
                f"len={info['payload_len']}")
            self.packet_complete.emit(info, decoded)

        if self._locked is not None and chunk_decoded == 0:
            self._lock_miss_streak += 1
            if self._lock_miss_streak >= LOCK_MISS_LIMIT:
                self.frame_event.emit(
                    f"[lock lost after {self._lock_miss_streak} empty windows -- re-scanning all combinations]")
                self._locked = None
                self._lock_miss_streak = 0

        power_db = gd.signal_power_dbfs(iq)
        spectrum = gd.spectrum_db(iq, nfft=SPECTRUM_NFFT)
        self.stats_update.emit({
            "mode": searching,
            "locked_combo": self._locked,
            "candidates_total": self._candidate_count,
            "decoded_total": self._decoded_count,
            "rejected_total": self._rejected_count,
            "chunk_candidates": len(candidates),
            "chunk_decoded": chunk_decoded,
            "chunk_rejected": chunk_rejected,
            "signal_power_dbfs": power_db,
            "spectrum": spectrum,
        })

    def run(self):
        try:
            if self.source_kind == "file":
                path, fmt = self.source_args["path"], self.source_args["format"]
                self.frame_event.emit(f"--- loading {path} ({fmt}) ---")
                iq = gd.read_iq_file_u8(path) if fmt == "u8" else gd.read_iq_file_cf32(path)
                self.frame_event.emit(f"--- {len(iq)} samples loaded, processing ---")
                chunk = int(self.sample_rate * CHUNK_SECONDS)
                overlap = int(self.sample_rate * 0.2)
                pos = 0
                while self._running and pos < len(iq):
                    start = max(0, pos - overlap)
                    end = min(len(iq), pos + chunk)
                    self._process_buffer(iq[start:end])
                    pos += chunk
                self.frame_event.emit("--- file processing complete ---")
                self.finished_ok.emit()
                return

            elif self.source_kind == "rtlsdr":
                src = gd.RtlSdrSource(
                    center_freq_hz=self.source_args["freq"],
                    sample_rate_hz=self.sample_rate,
                    gain=self.source_args.get("gain", "auto"),
                )
                self.frame_event.emit(
                    f"--- RTL-SDR streaming: {self.source_args['freq']/1e6:.4f} MHz "
                    f"@ {self.sample_rate} sps ---")
                chunk = int(self.sample_rate * CHUNK_SECONDS)
                overlap_tail = np.array([], dtype=np.complex64)
                overlap_n = int(self.sample_rate * 0.2)
                try:
                    while self._running:
                        block = src.read(chunk)
                        buf = np.concatenate([overlap_tail, block])
                        self._process_buffer(buf)
                        overlap_tail = block[-overlap_n:]
                finally:
                    src.close()
                self.finished_ok.emit()
                return

            else:
                self.error.emit(f"unknown source kind: {self.source_kind}")
        except Exception as e:  # noqa: broad -- worker thread, must not crash silently
            self.error.emit(f"{e}\n{traceback.format_exc()}")


class RfAnalyzerPanel(QWidget):
    """Spectrum + waterfall + rolling link-quality graphs. Fed one
    stats_update dict per chunk; owns no serial/demod state of its own."""

    def __init__(self, sample_rate: float):
        super().__init__()
        self.sample_rate = sample_rate
        self._t = 0.0

        self._power_hist = deque(maxlen=STATS_HISTORY)
        self._decoded_rate_hist = deque(maxlen=STATS_HISTORY)
        self._rejected_rate_hist = deque(maxlen=STATS_HISTORY)
        self._success_hist = deque(maxlen=STATS_HISTORY)
        self._time_hist = deque(maxlen=STATS_HISTORY)
        self._cum_decoded = 0
        self._cum_rejected = 0

        self._waterfall = np.full((WATERFALL_ROWS, SPECTRUM_NFFT), -140.0, dtype=np.float32)

        pg.setConfigOptions(antialias=True)

        tabs = QTabWidget()
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.addWidget(tabs)

        # --- Spectrum + waterfall tab ---
        spec_tab = QWidget()
        spec_layout = QVBoxLayout(spec_tab)

        self.spectrum_plot = pg.PlotWidget(title="Live spectrum (dB, centered on tuned frequency)")
        self.spectrum_plot.setBackground(PANEL)
        self.spectrum_plot.showGrid(x=True, y=True, alpha=0.25)
        self.spectrum_plot.setLabel("bottom", "Offset from center", units="Hz")
        self.spectrum_plot.setLabel("left", "Magnitude", units="dB")
        freqs = np.fft.fftshift(np.fft.fftfreq(SPECTRUM_NFFT, d=1.0 / sample_rate))
        self._freqs = freqs
        self.spectrum_curve = self.spectrum_plot.plot(freqs, np.full(SPECTRUM_NFFT, -140.0), pen=pg.mkPen(FG, width=1.4))
        spec_layout.addWidget(self.spectrum_plot, 2)

        self.waterfall_plot = pg.PlotWidget(title="Waterfall (scrolling, newest at bottom)")
        self.waterfall_plot.setBackground(PANEL)
        self.waterfall_plot.setLabel("bottom", "Offset from center", units="Hz")
        self.waterfall_img = pg.ImageItem()
        self.waterfall_plot.addItem(self.waterfall_img)
        cmap = pg.colormap.get("viridis")
        self.waterfall_img.setColorMap(cmap)
        self.waterfall_img.setLevels((-100, -20))
        half_span = sample_rate / 2.0
        self.waterfall_img.setRect(pg.QtCore.QRectF(-half_span, 0, sample_rate, WATERFALL_ROWS))
        self.waterfall_plot.setYRange(0, WATERFALL_ROWS)
        self.waterfall_plot.setXRange(-half_span, half_span)
        self.waterfall_plot.getPlotItem().hideAxis("left")
        spec_layout.addWidget(self.waterfall_plot, 3)

        tabs.addTab(spec_tab, "Spectrum / Waterfall")

        # --- Link quality tab ---
        lq_tab = QWidget()
        lq_layout = QVBoxLayout(lq_tab)

        self.power_plot = pg.PlotWidget(title="Signal power (dBFS)")
        self.power_plot.setBackground(PANEL)
        self.power_plot.showGrid(x=True, y=True, alpha=0.25)
        self.power_curve = self.power_plot.plot(pen=pg.mkPen(ACCENT, width=1.6))
        lq_layout.addWidget(self.power_plot, 1)

        self.rate_plot = pg.PlotWidget(title="Decoded / rejected per chunk")
        self.rate_plot.setBackground(PANEL)
        self.rate_plot.showGrid(x=True, y=True, alpha=0.25)
        self.rate_plot.addLegend()
        self.decoded_curve = self.rate_plot.plot(pen=pg.mkPen(FG, width=1.6), name="decoded")
        self.rejected_curve = self.rate_plot.plot(pen=pg.mkPen(ERR, width=1.6), name="rejected")
        lq_layout.addWidget(self.rate_plot, 1)

        self.success_plot = pg.PlotWidget(title="Cumulative success rate (%)")
        self.success_plot.setBackground(PANEL)
        self.success_plot.showGrid(x=True, y=True, alpha=0.25)
        self.success_plot.setYRange(0, 100)
        self.success_curve = self.success_plot.plot(pen=pg.mkPen(WARN, width=1.6))
        lq_layout.addWidget(self.success_plot, 1)

        tabs.addTab(lq_tab, "Link Quality")

    def reset(self, sample_rate: float):
        self.sample_rate = sample_rate
        self._t = 0.0
        self._power_hist.clear()
        self._decoded_rate_hist.clear()
        self._rejected_rate_hist.clear()
        self._success_hist.clear()
        self._time_hist.clear()
        self._cum_decoded = 0
        self._cum_rejected = 0
        self._waterfall[:] = -140.0
        freqs = np.fft.fftshift(np.fft.fftfreq(SPECTRUM_NFFT, d=1.0 / sample_rate))
        self._freqs = freqs
        half_span = sample_rate / 2.0
        self.waterfall_img.setRect(pg.QtCore.QRectF(-half_span, 0, sample_rate, WATERFALL_ROWS))
        self.waterfall_plot.setXRange(-half_span, half_span)
        self.spectrum_plot.setXRange(-half_span, half_span)

    def on_stats(self, stats: dict):
        spectrum = stats.get("spectrum")
        if spectrum is not None:
            self.spectrum_curve.setData(self._freqs, spectrum)
            self._waterfall = np.roll(self._waterfall, -1, axis=0)
            self._waterfall[-1, :] = spectrum
            self.waterfall_img.setImage(self._waterfall.T, autoLevels=False)

        self._t += CHUNK_SECONDS
        self._time_hist.append(self._t)
        self._power_hist.append(stats["signal_power_dbfs"] if stats["signal_power_dbfs"] == stats["signal_power_dbfs"] else -140.0)
        self._decoded_rate_hist.append(stats["chunk_decoded"])
        self._rejected_rate_hist.append(stats["chunk_rejected"])
        self._cum_decoded += stats["chunk_decoded"]
        self._cum_rejected += stats["chunk_rejected"]
        total = self._cum_decoded + self._cum_rejected
        self._success_hist.append(100.0 * self._cum_decoded / total if total else 0.0)

        t = np.array(self._time_hist)
        self.power_curve.setData(t, np.array(self._power_hist))
        self.decoded_curve.setData(t, np.array(self._decoded_rate_hist))
        self.rejected_curve.setData(t, np.array(self._rejected_rate_hist))
        self.success_curve.setData(t, np.array(self._success_hist))


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("S2S2 Nucleo RX -- RTL-SDR GMSK/AX.25/FX.25 Decoder")
        self.resize(1500, 900)
        self.worker = None
        self.packets = []
        self.detail_positions = []

        central = QWidget()
        self.setCentralWidget(central)
        outer = QVBoxLayout(central)

        # --- source config ---
        cfg_box = QGroupBox("Source")
        cfg_layout = QHBoxLayout(cfg_box)

        self.source_combo = QComboBox()
        self.source_combo.addItems(["RTL-SDR", "IQ file"])
        self.source_combo.currentTextChanged.connect(self._on_source_changed)
        cfg_layout.addWidget(QLabel("Source:"))
        cfg_layout.addWidget(self.source_combo)

        self.freq_edit = QLineEdit(str(DEFAULT_FREQ_HZ))
        self.freq_edit.setToolTip(
            "Center frequency, Hz. Default is config.h DEFAULT_TX_FREQUENCY_HZ -- "
            "the satellite's downlink/beacon frequency (what you want to receive). "
            "DEFAULT_RX_FREQUENCY_HZ (437.375 MHz) is what the satellite listens on "
            "for uplink -- wrong frequency for hearing the satellite's own beacon.")
        cfg_layout.addWidget(QLabel("Freq (Hz):"))
        cfg_layout.addWidget(self.freq_edit)

        self.rate_combo = QComboBox()
        self.rate_combo.setEditable(True)
        self.rate_combo.addItems(["240000", "250000", "1024000", "2048000"])
        cfg_layout.addWidget(QLabel("Sample rate:"))
        cfg_layout.addWidget(self.rate_combo)

        self.gain_edit = QLineEdit("auto")
        cfg_layout.addWidget(QLabel("Gain:"))
        cfg_layout.addWidget(self.gain_edit)

        self.file_edit = QLineEdit()
        self.file_edit.setPlaceholderText("path to .iq (u8) or .cfile (cf32) capture")
        self.file_browse_btn = QPushButton("Browse...")
        self.file_browse_btn.clicked.connect(self._browse_file)
        self.file_fmt_combo = QComboBox()
        self.file_fmt_combo.addItems(["u8 (rtl_sdr)", "cf32 (GNU Radio)"])
        cfg_layout.addWidget(self.file_edit, 1)
        cfg_layout.addWidget(self.file_browse_btn)
        cfg_layout.addWidget(self.file_fmt_combo)

        self.verbose_check = QCheckBox("Verbose log")
        self.verbose_check.setToolTip(
            "Log every rejected sync candidate, not just successful decodes and "
            "lock state changes. Off by default -- with the full brute-force search "
            "(before lock is acquired) this can be dozens of lines per 0.5s chunk, "
            "almost all of which are combinations that were never going to be right.")
        cfg_layout.addWidget(self.verbose_check)

        self.start_btn = QPushButton("Start")
        self.start_btn.clicked.connect(self.toggle_run)
        cfg_layout.addWidget(self.start_btn)

        outer.addWidget(cfg_box)
        self._on_source_changed(self.source_combo.currentText())

        # --- live signal-quality stats ---
        stats_box = QGroupBox("Signal / Decode Quality")
        stats_layout = QHBoxLayout(stats_box)
        self.stat_labels = {}
        for key, label in [
            ("mode", "Search mode:"), ("locked_combo", "Locked combo:"),
            ("signal_power_dbfs", "Signal power:"), ("decoded_total", "Decoded:"),
            ("rejected_total", "Rejected:"), ("success_rate", "Success rate:"),
        ]:
            stats_layout.addWidget(QLabel(label))
            val = QLabel("--")
            val.setStyleSheet(f"font-weight: bold; color: {ACCENT};")
            stats_layout.addWidget(val)
            self.stat_labels[key] = val
        stats_layout.addStretch(1)
        outer.addWidget(stats_box)

        # --- main split: (table+detail+log) | (RF analyzer) ---
        main_split = QSplitter(Qt.Orientation.Horizontal if PYQT_VERSION == 6 else Qt.Horizontal)

        left = QWidget()
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(0, 0, 0, 0)

        table_split = QSplitter(Qt.Orientation.Horizontal if PYQT_VERSION == 6 else Qt.Horizontal)
        self.table = QTableWidget(0, 7)
        self.table.setHorizontalHeaderLabels(
            ["Time", "Type", "Src->Dst", "Seq", "Session", "RS fixed", "Summary"])
        header = self.table.horizontalHeader()
        header.setSectionResizeMode(6, QHeaderView.ResizeMode.Stretch if PYQT_VERSION == 6 else QHeaderView.Stretch)
        self.table.itemSelectionChanged.connect(self._on_row_selected)
        self.table.setEditTriggers(QTableWidget.EditTrigger.NoEditTriggers if PYQT_VERSION == 6 else QTableWidget.NoEditTriggers)
        table_split.addWidget(self.table)

        self.detail = QPlainTextEdit()
        self.detail.setReadOnly(True)
        self.detail.setFont(QFont("Menlo", 11) if sys.platform == "darwin" else QFont("Consolas", 10))
        table_split.addWidget(self.detail)
        table_split.setSizes([600, 400])
        left_layout.addWidget(table_split, 1)

        log_label = QLabel("Frame log:")
        left_layout.addWidget(log_label)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(5000)
        self.log.setFont(QFont("Menlo", 10) if sys.platform == "darwin" else QFont("Consolas", 9))
        left_layout.addWidget(self.log, 1)

        clear_row = QHBoxLayout()
        clear_row.addStretch(1)
        clear_btn = QPushButton("Clear all")
        clear_btn.clicked.connect(self._clear_all)
        clear_row.addWidget(clear_btn)
        left_layout.addLayout(clear_row)

        main_split.addWidget(left)

        self.analyzer = RfAnalyzerPanel(DEFAULT_SAMPLE_RATE)
        main_split.addWidget(self.analyzer)
        main_split.setSizes([850, 650])

        outer.addWidget(main_split, 1)

        self.status = QStatusBar()
        self.setStatusBar(self.status)
        self.status.showMessage("Idle")

    # ------------------------------------------------------------
    def _on_source_changed(self, text):
        is_file = (text == "IQ file")
        self.file_edit.setVisible(is_file)
        self.file_browse_btn.setVisible(is_file)
        self.file_fmt_combo.setVisible(is_file)
        self.freq_edit.setVisible(not is_file)
        self.gain_edit.setVisible(not is_file)

    def _browse_file(self):
        path, _ = QFileDialog.getOpenFileName(self, "Select IQ capture")
        if path:
            self.file_edit.setText(path)

    def _clear_all(self):
        self.table.setRowCount(0)
        self.packets = []
        self.detail_positions = []
        self.detail.clear()
        self.log.clear()

    def append_log(self, line: str):
        self.log.appendPlainText(line)

    # ------------------------------------------------------------
    def toggle_run(self):
        if self.worker is not None:
            self.worker.stop()
            self.start_btn.setEnabled(False)
            self.status.showMessage("Stopping...")
            return
        self._start()

    def _start(self):
        try:
            sample_rate = float(self.rate_combo.currentText())
        except ValueError:
            QMessageBox.warning(self, "Bad input", "Sample rate must be numeric.")
            return

        if self.source_combo.currentText() == "IQ file":
            path = self.file_edit.text().strip()
            if not path or not os.path.isfile(path):
                QMessageBox.warning(self, "No file", "Pick a valid IQ capture file first.")
                return
            fmt = "u8" if self.file_fmt_combo.currentIndex() == 0 else "cf32"
            source_kind = "file"
            source_args = {"path": path, "format": fmt}
        else:
            try:
                freq = float(self.freq_edit.text())
            except ValueError:
                QMessageBox.warning(self, "Bad input", "Frequency must be numeric.")
                return
            gain_text = self.gain_edit.text().strip()
            gain = gain_text if gain_text == "auto" else float(gain_text)
            source_kind = "rtlsdr"
            source_args = {"freq": freq, "gain": gain}

        self.analyzer.reset(sample_rate)

        self.worker = RxWorker(source_kind, source_args, sample_rate,
                                verbose=self.verbose_check.isChecked())
        self.worker.frame_event.connect(self.append_log)
        self.worker.packet_complete.connect(self._on_packet_complete)
        self.worker.stats_update.connect(self._on_stats_update)
        self.worker.error.connect(self._on_worker_error)
        self.worker.finished_ok.connect(self._on_worker_finished)
        self.worker.start()

        self.start_btn.setText("Stop")
        self.source_combo.setEnabled(False)
        self.status.showMessage("Running...")

    def _on_worker_error(self, message):
        self.append_log(f"--- ERROR: {message} ---")
        QMessageBox.critical(self, "Receiver error", message.splitlines()[0])
        self._reset_after_stop()

    def _on_worker_finished(self):
        self.append_log("--- stopped ---")
        self._reset_after_stop()

    def _reset_after_stop(self):
        self.worker = None
        self.start_btn.setText("Start")
        self.start_btn.setEnabled(True)
        self.source_combo.setEnabled(True)
        self.status.showMessage("Idle")

    def _on_stats_update(self, stats: dict):
        self.stat_labels["mode"].setText(stats["mode"])
        combo = stats["locked_combo"]
        self.stat_labels["locked_combo"].setText(
            f"phase={combo[0]} invert={combo[1]} order={combo[2]}" if combo else "-- (searching)")
        power = stats["signal_power_dbfs"]
        self.stat_labels["signal_power_dbfs"].setText(
            f"{power:.1f} dBFS" if power == power and power != float("-inf") else "--")
        self.stat_labels["decoded_total"].setText(str(stats["decoded_total"]))
        self.stat_labels["rejected_total"].setText(str(stats["rejected_total"]))
        total_attempts = stats["decoded_total"] + stats["rejected_total"]
        rate = (100.0 * stats["decoded_total"] / total_attempts) if total_attempts else 0.0
        self.stat_labels["success_rate"].setText(f"{rate:.0f}%")
        self.analyzer.on_stats(stats)

    def _on_packet_complete(self, pkt: dict, decoded: dict):
        row = self.table.rowCount()
        self.table.insertRow(row)
        ts = time.strftime("%H:%M:%S")
        src_dst = f"{pkt['src_id']} -> {pkt['dst_id']}"
        summary = _summarize(decoded)
        values = [ts, decoded["type_name"], src_dst, str(pkt["seq_num"]),
                  str(pkt["session_id"]), str(pkt["rs_bytes_corrected"]), summary]
        for col, v in enumerate(values):
            self.table.setItem(row, col, QTableWidgetItem(v))
        self.packets.append((pkt, decoded))
        self.table.scrollToBottom()

        # Auto-append the full breakdown for every message, not just the
        # selected row -- so you get the detailed view live as messages
        # arrive instead of having to click each one.
        header = f"===== #{row + 1}  {ts}  {decoded['type_name']}  seq={pkt['seq_num']} ====="
        cursor = self.detail.textCursor()
        end_move = QTextCursor.MoveOperation.End if PYQT_VERSION == 6 else QTextCursor.End
        cursor.movePosition(end_move)
        block_start = cursor.position()
        self.detail_positions.append(block_start)
        self.detail.appendPlainText(header)
        self.detail.appendPlainText(_format_detail(pkt, decoded))
        self.detail.appendPlainText("")
        self.detail.verticalScrollBar().setValue(self.detail.verticalScrollBar().maximum())

    def _on_row_selected(self):
        rows = self.table.selectionModel().selectedRows()
        if not rows:
            return
        idx = rows[0].row()
        if idx >= len(self.detail_positions):
            return
        cursor = self.detail.textCursor()
        cursor.setPosition(self.detail_positions[idx])
        self.detail.setTextCursor(cursor)
        self.detail.ensureCursorVisible()

    def closeEvent(self, event):
        if self.worker is not None:
            self.worker.stop()
            self.worker.wait(2000)
        event.accept()


def _summarize(decoded: dict) -> str:
    if "beacon1" in decoded and decoded["beacon1"]:
        b = decoded["beacon1"]
        return f"Beacon1: bat_v={b['bat_v']} bat_i={b['bat_i']} sys_stat={b['system_stat']}"
    if "beacon2" in decoded and decoded["beacon2"]:
        b = decoded["beacon2"]
        return f"Beacon2: gyro=({b['gyro_x']},{b['gyro_y']},{b['gyro_z']})"
    if "command" in decoded and decoded["command"]:
        c = decoded["command"]
        return f"CMD {c['opcode_name']} subsys={c['subsystem_name']} data={c['data_id_name']}"
    if "ack" in decoded and decoded["ack"]:
        a = decoded["ack"]
        return f"ACK {a['status_name']} (acked_seq={a['acked_seq']})"
    if "camera_fragment" in decoded and decoded["camera_fragment"]:
        f = decoded["camera_fragment"]
        return f"Image frag {f['fragment_index'] + 1}/{f['total_fragments']} ({f['data_len']}B)"
    if "ascii" in decoded:
        return decoded["ascii"][:60]
    return decoded.get("raw_hex", "")[:60]


def _format_detail(pkt: dict, decoded: dict) -> str:
    lines = [
        f"Type        : {decoded['type_name']} ({pkt['type']})",
        f"Src -> Dst  : {pkt['src_id']} -> {pkt['dst_id']}",
        f"AX.25 addr  : {pkt['src_addr'][0]}-{pkt['src_addr'][1]} -> "
        f"{pkt['dest_addr'][0]}-{pkt['dest_addr'][1]}",
        f"Seq / Sess  : {pkt['seq_num']} / {pkt['session_id']}",
        f"Flags       : 0x{pkt['flags']:02x}",
        f"Payload len : {pkt['payload_len']}",
        f"RS corrected: {pkt['rs_bytes_corrected']} symbol(s) total across fragments",
        "",
    ]
    for key in ("beacon1", "beacon2", "command", "ack", "camera_fragment"):
        if decoded.get(key):
            lines.append(f"-- {key} --")
            for k, v in decoded[key].items():
                if k == "data":
                    lines.append(f"  {k}: {len(v)} bytes")
                else:
                    lines.append(f"  {k}: {v}")
            lines.append("")
    if "raw_hex" in decoded:
        lines.append("-- raw payload --")
        lines.append(decoded["raw_hex"])
        if "ascii" in decoded:
            lines.append("")
            lines.append("ascii: " + decoded["ascii"])
    return "\n".join(lines)


def main():
    app = QApplication(sys.argv)
    app.setStyleSheet(DARK_STYLESHEET)
    win = MainWindow()
    win.show()
    sys.exit(run_app_exec(app))


if __name__ == "__main__":
    main()
