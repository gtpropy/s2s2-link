#!/usr/bin/env python3
"""
console.py -- Unified web-based bench test tool for the S2S2 link,
replacing the PyQt5 GUIs (tx_gui.py, rx_gui.py, satellite_gui.py)
with a single FastAPI + WebSocket server and a browser front end.

Two independent panes, each switchable between three roles:
  - satellite: DEBUG_BUILD UART backdoor on the satellite board's own
    debug serial port (TX power, send raw message, CW/Beacon1/Beacon2
    test triggers, beacon cycle on/off, satellite-mode toggle, full
    continuous test battery).
  - ground:    the ground station's CLI over its own debug UART
    (COMMAND/HEARTBEAT/HEALTH/... keywords from tx_gui.py).
  - sdr:       a REAL RTL-SDR dongle (pyrtlsdr), running the same GMSK/
    FX.25 demod chain as rx_gui.py/gmsk_demod.py plus the CW
    monitor/Morse decoder from cw_monitor.py/morse.py -- no simulated
    or fabricated data, this drives actual hardware.

No auth, no persistence -- this is a local bench tool, not the mission
control dashboard (mission_control_api.py), which stays separate.

Run: python3 console.py [--host 0.0.0.0] [--port 7910]

@author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
@date   2026-09-13
"""
from __future__ import annotations

import argparse
import asyncio
import glob
import os
import re
import shutil
import subprocess
import threading
import time
import traceback
from collections import deque

import numpy as np
from fastapi import FastAPI, WebSocket, WebSocketDisconnect
from fastapi.responses import FileResponse
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel

import gmsk_demod as gd
import decoder as dec
import morse
from cw_monitor import CwMonitor
from gs_protocol import GsProtocolParser
import fault_injection_suite as fault_suite

HERE = os.path.dirname(os.path.abspath(__file__))
STATIC_DIR = os.path.join(HERE, "console_static")

MIN_POWER_DBM = -9   # SX126x HP-PA real range (RadioApp_SetTxPower clamps to this)
MAX_POWER_DBM = 22

# Same phase-name regex fix applied to mission_control_api.py/satellite_gui.py
# this session, after the CW/Beacon1/Mission/Beacon2 cycle was renamed to
# CW1/Mission1/CW2/Mission2 in ipcc_m0plus_main.c.
_RE_CYCLE_PHASE = re.compile(r"CYCLE PHASE:\s*(CW1|MISSION1|CW2|MISSION2)")

# Satellite-activity-monitor patterns -- exact log-line text printed by
# ipcc_m0plus_main.c (Send_Cw_Beacon/Send_Beacon1/Send_Beacon2/
# Send_Beacon_If_Due/Try_Digipeat), same regexes mission_control_api.py's
# SatelliteLink uses for the digi ones. Purely a live-UART activity count
# -- no fabricated data, just tally of real lines seen.
_RE_CW_TX = re.compile(r'^CW BEACON TX')
_RE_TXCW = re.compile(r'^TXCW: sending')
_RE_BEACON1_TX = re.compile(r'^BEACON1 TX')
_RE_BEACON2_TX = re.compile(r'^BEACON2 TX')
_RE_TEXT_PING_TX = re.compile(r'^BEACON TX \(5s tick\)')
_RE_DIGI_DUP = re.compile(r"DIGI: duplicate, not re-relaying")
_RE_DIGI_RELAYED = re.compile(
    r"DIGI: relayed via hop (\d+) \(([^)]+)\), (\d+) info byte\(s\), rs_corrected=(-?\d+)")
_RE_DIGI_FAILED = re.compile(r"DIGI: relay TX failed")

# Real runtime stack high-water-mark (core/Src/mem_stats.c's stack-
# painting technique), printed by the MEMSTAT UART command -- NOT
# derived/estimated, an actual measurement off the running chip.
_RE_MEMSTAT = re.compile(r"MEMSTAT stack_used=(\d+) stack_total=(\d+)")

# =====================================================================
# STM32 memory profile -- real static numbers from the built .elf (arm-
# none-eabi-size/-nm), not a runtime profiler (bare-metal single-loop
# firmware has no heap/task accounting to instrument without firmware
# changes -- see the accompanying note in the UI). STM32WLE5/WL55: 256
# KiB flash, 64 KiB SRAM total (matches st-flash's own probe output).
CPU2_ROOT = os.path.abspath(os.path.join(HERE, ".."))
ELF_PATH = os.path.join(CPU2_ROOT, "ipcc_m0plus.elf")
FLASH_TOTAL_BYTES = 256 * 1024
RAM_TOTAL_BYTES = 64 * 1024
_TOOLCHAIN_BIN_GLOB = "/Applications/ArmGNUToolchain/*/arm-none-eabi/bin"


def _find_tool(name: str) -> str | None:
    for cand_dir in glob.glob(_TOOLCHAIN_BIN_GLOB):
        cand = os.path.join(cand_dir, name)
        if os.path.isfile(cand):
            return cand
    return shutil.which(name)


def get_memory_profile() -> dict:
    """Real static memory profile from the built .elf (arm-none-eabi-
    size + -nm --size-sort), refreshed on every call so it reflects
    whatever was most recently built/flashed. No runtime instrumentation
    -- this bare-metal single-loop firmware has no heap or task
    scheduler to sample; text/data/bss sizes are the honest number
    available without adding firmware code (a stack-painting high-
    water-mark command would be the next step, see MEMSTAT note)."""
    if not os.path.isfile(ELF_PATH):
        return {"ok": False, "error": f"{ELF_PATH} not found -- build it first (make sat)"}

    size_tool = _find_tool("arm-none-eabi-size")
    nm_tool = _find_tool("arm-none-eabi-nm")
    if size_tool is None or nm_tool is None:
        return {"ok": False, "error": "arm-none-eabi-size/-nm not found on PATH or under "
                                       "/Applications/ArmGNUToolchain"}

    try:
        size_out = subprocess.run([size_tool, ELF_PATH], check=True,
                                   capture_output=True, text=True).stdout.strip().splitlines()
        # size_out[0] is the header, size_out[1] is "text data bss dec hex filename"
        text_b, data_b, bss_b = (int(x) for x in size_out[1].split()[:3])

        nm_out = subprocess.run([nm_tool, "--print-size", "--size-sort", "-C", ELF_PATH],
                                 check=True, capture_output=True, text=True).stdout.strip().splitlines()
        top_symbols = []
        max_plausible = FLASH_TOTAL_BYTES + RAM_TOTAL_BYTES
        for line in reversed(nm_out):  # size-sort is ascending; largest last
            parts = line.split(None, 3)
            if len(parts) < 4:
                continue
            addr_hex, size_hex, sym_type, name = parts
            try:
                size = int(size_hex, 16)
            except ValueError:
                continue
            if size > max_plausible:
                # A linker symbol at the very top of the address map (e.g.
                # _estack) has no "next symbol" for nm's size-sort distance
                # heuristic to subtract against, so it wraps around the
                # address space into a bogus multi-GB "size" -- not a real
                # code/data symbol, skip it rather than let it dominate
                # the top-N list.
                continue
            top_symbols.append({"name": name, "size": size, "type": sym_type, "addr": "0x" + addr_hex})
            if len(top_symbols) >= 15:
                break
    except (subprocess.CalledProcessError, ValueError, IndexError) as e:
        return {"ok": False, "error": f"{e}"}

    flash_used = text_b + data_b
    ram_used = data_b + bss_b
    mtime = os.path.getmtime(ELF_PATH)
    return {
        "ok": True, "elf_path": ELF_PATH, "elf_mtime": mtime,
        "text": text_b, "data": data_b, "bss": bss_b,
        "flash_used": flash_used, "flash_total": FLASH_TOTAL_BYTES,
        "flash_pct": round(100.0 * flash_used / FLASH_TOTAL_BYTES, 1),
        "ram_used": ram_used, "ram_total": RAM_TOTAL_BYTES,
        "ram_pct": round(100.0 * ram_used / RAM_TOTAL_BYTES, 1),
        "top_symbols": top_symbols,
    }


GS_CLI_COMMANDS = [
    "LISTEN", "HEALTH", "FLAGS", "RSVTABLE", "SATLOG", "CAMERA", "CAMFILE",
    "EPDM", "ADCS", "DPM ON", "DPM OFF",
]

DEFAULT_SDR_FREQ_HZ = 435313000.0   # satellite TX / config.h DEFAULT_TX_FREQUENCY_HZ
DEFAULT_SDR_SAMPLE_RATE = 240000.0
DEFAULT_SDR_BAUD = 4800.0           # RADIO_BIT_RATE_BPS -- NOT gmsk_demod.BAUD's stale 9600 default
DEFAULT_CHUNK_SECONDS = 0.5
LOCK_PHASE_WINDOW = 2
DEFAULT_LOCK_MISS_LIMIT = 6
MAX_DEDUPE = 64


# =====================================================================
# Serial link (satellite debug UART / ground station CLI UART)
# =====================================================================

class SerialLink:
    """Generic bench-UART connection, reused for both the satellite's
    DEBUG_BUILD backdoor and the ground station's CLI -- same line-
    framed text protocol on both, just different command vocabularies
    and different physical ports."""

    def __init__(self, role: str, emit_fn):
        self.role = role
        self._emit = emit_fn
        self.ser = None
        self._thread = None
        self._running = False
        self._line_buf = ""
        self.port = None
        self.baud = None
        self.cycle_phase = None
        self._gs_parser = GsProtocolParser() if role == "ground" else None
        # Activity-monitor counters (satellite role) -- real tallies off
        # real UART lines, no synthetic/estimated values.
        self.connected_at = None
        self.lines_total = 0
        self.last_line_ts = None
        self.cw_tx_count = 0
        self.beacon1_tx_count = 0
        self.beacon2_tx_count = 0
        self.text_ping_count = 0
        self.digi_relayed = 0
        self.digi_duplicate = 0
        self.digi_failed = 0
        # Real runtime measurement (see _RE_MEMSTAT) -- None until the
        # first MEMSTAT response, not a static/estimated placeholder.
        self.stack_used_bytes = None
        self.stack_total_bytes = None
        self.stack_pct = None
        self.stack_last_update_ts = None

    @property
    def connected(self) -> bool:
        return self.ser is not None

    def connect(self, port: str, baud: int):
        import serial
        if self.ser is not None:
            self.disconnect()
        self.ser = serial.Serial(port, baud, timeout=0.2)
        self.port, self.baud = port, baud
        self.connected_at = time.time()
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        self._emit({"kind": "CONNECTED", "ts": time.time(), "role": self.role,
                    "port": port, "baud": baud})

    def disconnect(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=1.5)
            self._thread = None
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
            self.ser = None
        self._emit({"kind": "DISCONNECTED", "ts": time.time(), "role": self.role})
        self.port, self.baud = None, None

    def send_line(self, text: str):
        if self.ser is None:
            raise RuntimeError("not connected")
        self.ser.write((text + "\r\n").encode("ascii"))
        self._emit({"kind": "SENT", "ts": time.time(), "text": text})

    def _run(self):
        while self._running:
            try:
                n = self.ser.in_waiting
                data = self.ser.read(n if n else 1)
            except Exception:
                break
            if not data:
                continue
            text = data.decode("latin-1", errors="replace")
            self._line_buf += text
            parts = self._line_buf.replace("\r\n", "\n").replace("\r", "\n").split("\n")
            self._line_buf = parts[-1]
            for line in parts[:-1]:
                self._handle_line(line)

    def _handle_line(self, line: str):
        stripped = line.rstrip()
        if not stripped:
            return
        now = time.time()
        self.lines_total += 1
        self.last_line_ts = now
        event = {"kind": "RAW", "ts": now, "raw_line": line}
        activity_changed = False

        m = _RE_CYCLE_PHASE.search(stripped)
        if m:
            self.cycle_phase = m.group(1)
            event = {"kind": "CYCLE_PHASE", "ts": now, "raw_line": line,
                      "phase": self.cycle_phase}
            activity_changed = True
        elif self._gs_parser is not None:
            parsed = self._gs_parser.feed(stripped)
            if parsed:
                event = {"kind": "GS_PARSED", "ts": now, "raw_line": line,
                          "parsed": parsed}

        if _RE_CW_TX.search(stripped) or _RE_TXCW.search(stripped):
            self.cw_tx_count += 1
            activity_changed = True
        elif _RE_BEACON1_TX.search(stripped):
            self.beacon1_tx_count += 1
            activity_changed = True
        elif _RE_BEACON2_TX.search(stripped):
            self.beacon2_tx_count += 1
            activity_changed = True
        elif _RE_TEXT_PING_TX.search(stripped):
            self.text_ping_count += 1
            activity_changed = True
        elif _RE_DIGI_DUP.search(stripped):
            self.digi_duplicate += 1
            activity_changed = True
        elif _RE_DIGI_RELAYED.search(stripped):
            self.digi_relayed += 1
            activity_changed = True
        elif _RE_DIGI_FAILED.search(stripped):
            self.digi_failed += 1
            activity_changed = True

        m_mem = _RE_MEMSTAT.search(stripped)
        if m_mem:
            self.stack_used_bytes = int(m_mem.group(1))
            self.stack_total_bytes = int(m_mem.group(2))
            self.stack_pct = round(100.0 * self.stack_used_bytes / self.stack_total_bytes, 1) \
                if self.stack_total_bytes else 0.0
            self.stack_last_update_ts = now
            event = {"kind": "MEMSTAT", "ts": now, "raw_line": line,
                      "stack_used_bytes": self.stack_used_bytes,
                      "stack_total_bytes": self.stack_total_bytes,
                      "stack_pct": self.stack_pct}
            activity_changed = True  # folds stack_* into the same ACTIVITY broadcast

        self._emit(event)
        if activity_changed:
            self._emit({"kind": "ACTIVITY", "ts": now, **self.activity_snapshot()})

    def activity_snapshot(self) -> dict:
        return {
            "connected_at": self.connected_at,
            "uptime_s": (time.time() - self.connected_at) if self.connected_at else None,
            "lines_total": self.lines_total,
            "last_line_ts": self.last_line_ts,
            "cycle_phase": self.cycle_phase,
            "cw_tx_count": self.cw_tx_count,
            "beacon1_tx_count": self.beacon1_tx_count,
            "beacon2_tx_count": self.beacon2_tx_count,
            "text_ping_count": self.text_ping_count,
            "digi_relayed": self.digi_relayed,
            "digi_duplicate": self.digi_duplicate,
            "digi_failed": self.digi_failed,
            "stack_used_bytes": self.stack_used_bytes,
            "stack_total_bytes": self.stack_total_bytes,
            "stack_pct": self.stack_pct,
            "stack_last_update_ts": self.stack_last_update_ts,
        }


# =====================================================================
# SDR session -- real RTL-SDR, same demod chain as rx_gui.py's
# RxWorker, ported off Qt onto a plain thread + callback.
# =====================================================================

class SdrSession:
    def __init__(self, emit_fn):
        self._emit = emit_fn
        self._sdr = None
        self._thread = None
        self._running = False
        self.freq_hz = DEFAULT_SDR_FREQ_HZ
        self.sample_rate = DEFAULT_SDR_SAMPLE_RATE
        self.baud = DEFAULT_SDR_BAUD

        self.reassembler = dec.Reassembler()
        self._seen = deque(maxlen=MAX_DEDUPE)
        self._seen_set = set()
        self._completed_seen = deque(maxlen=MAX_DEDUPE)
        self._completed_seen_set = set()
        self._locked = None
        self._lock_miss_streak = 0
        self._decoded_count = 0
        self._rejected_count = 0
        self._candidate_count = 0

        self.cw_monitor = None
        self.cw_decoder = morse.MorseGateDecoder(8000)

    @property
    def connected(self) -> bool:
        return self._sdr is not None

    def connect(self, freq_hz: float, sample_rate: float, gain, baud: float,
                cw_threshold_dbfs: float = -20.0, cw_bandwidth_hz: float = 200.0):
        if self._sdr is not None:
            self.disconnect()
        self.freq_hz = freq_hz
        self.sample_rate = sample_rate
        self.baud = baud
        # threshold_dbfs is the CW mark/space gate -- must sit between your
        # actual noise floor and your actual keyed-carrier level, both of
        # which depend on antenna/coupling/gain on the bench, not something
        # a fixed default can get right for every setup. If env_dbfs (shown
        # live in the UI) never rises above this threshold during a real
        # CW TX burst, lower it (more negative) until it does.
        self.cw_monitor = CwMonitor(sample_rate, bandwidth_hz=cw_bandwidth_hz, audio_rate=8000,
                                     sidetone_hz=morse.DEFAULT_SIDETONE_HZ, threshold_dbfs=cw_threshold_dbfs)
        self._sdr = gd.RtlSdrSource(center_freq_hz=freq_hz, sample_rate_hz=sample_rate, gain=gain)
        self._running = True
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        self._emit({"kind": "SDR_CONNECTED", "ts": time.time(),
                    "freq_hz": freq_hz, "sample_rate": sample_rate, "gain": gain, "baud": baud,
                    "cw_threshold_dbfs": cw_threshold_dbfs, "cw_bandwidth_hz": cw_bandwidth_hz})

    def disconnect(self):
        self._running = False
        if self._thread:
            self._thread.join(timeout=2.0)
            self._thread = None
        if self._sdr:
            try:
                self._sdr.close()
            except Exception:
                pass
            self._sdr = None
        self._emit({"kind": "SDR_DISCONNECTED", "ts": time.time()})

    def _process_cw(self, iq_no_overlap: np.ndarray):
        if len(iq_no_overlap) == 0:
            return
        audio, gate = self.cw_monitor.process(iq_no_overlap)
        self.cw_decoder.feed(gate)
        # wpm = 1.2 / unit_seconds (morse.py's unit_seconds()), derived
        # from the decoder's live self-calibrated dit length -- a real
        # measurement of the on-air keying speed, not the firmware's
        # nominal CW_DEFAULT_WPM=15 constant.
        dit_seconds = self.cw_decoder._known_dit_samples / self.cw_decoder.sample_rate
        wpm_estimate = round(1.2 / dit_seconds, 1) if dit_seconds > 0 else None
        self._emit({"kind": "SDR_CW_TEXT", "ts": time.time(),
                    "text": self.cw_decoder.text, "env_dbfs": self.cw_monitor.last_env_dbfs,
                    "wpm_estimate": wpm_estimate, "char_count": len(self.cw_decoder.text)})

    def _process_buffer(self, iq: np.ndarray):
        power_db = gd.signal_power_dbfs(iq)
        if self._locked is not None:
            phase, invert, order = self._locked
            near = sorted({(phase + d) % gd.SPS for d in range(-LOCK_PHASE_WINDOW, LOCK_PHASE_WINDOW + 1)})
            candidates = gd.extract_candidate_frames(
                iq, self.sample_rate, baud=self.baud, phases=near, orders=[order], inverts=[invert])
            searching = "locked"
        else:
            candidates = gd.extract_candidate_frames(iq, self.sample_rate, baud=self.baud)
            searching = "full-scan"

        self._candidate_count += len(candidates)
        chunk_decoded = 0
        chunk_rejected = 0

        for c in candidates:
            frame = c["frame"]
            if frame in self._seen_set:
                continue
            if len(self._seen) == self._seen.maxlen:
                old = self._seen.popleft()
                self._seen_set.discard(old)
            self._seen.append(frame)
            self._seen_set.add(frame)

            combo = f"order={c['order']} invert={c['invert']} phase={c['phase']}"
            status, info = self.reassembler.accept_frame(frame, c.get("confidence"))

            if status == "rejected":
                chunk_rejected += 1
                self._rejected_count += 1
                continue

            self._locked = (c["phase"], c["invert"], c["order"])
            self._lock_miss_streak = 0

            if status == "partial":
                self._emit({"kind": "SDR_FRAME", "ts": time.time(), "status": "partial",
                            "combo": combo, "frag_index": info["frag_index"],
                            "frag_total": info["frag_total"], "seq_num": info["seq_num"],
                            "session_id": info["session_id"]})
                continue

            msg_key = (info["type"], info["src_id"], info["dst_id"],
                       info["seq_num"], info["session_id"])
            if msg_key in self._completed_seen_set:
                continue
            if len(self._completed_seen) == self._completed_seen.maxlen:
                old = self._completed_seen.popleft()
                self._completed_seen_set.discard(old)
            self._completed_seen.append(msg_key)
            self._completed_seen_set.add(msg_key)

            self._decoded_count += 1
            chunk_decoded += 1
            decoded = dec.decode_payload(info)
            self._emit({"kind": "SDR_FRAME", "ts": time.time(), "status": "complete",
                        "combo": combo, "type_name": decoded["type_name"],
                        "src_id": info["src_id"], "dst_id": info["dst_id"],
                        "seq_num": info["seq_num"], "session_id": info["session_id"],
                        "rs_corrected": info["rs_bytes_corrected"],
                        "payload_len": info["payload_len"],
                        "payload_hex": decoded.get("raw_hex", "")})

        if self._locked is not None and chunk_decoded == 0:
            self._lock_miss_streak += 1
            if self._lock_miss_streak >= DEFAULT_LOCK_MISS_LIMIT:
                self._locked = None
                self._lock_miss_streak = 0

        self._emit({"kind": "SDR_STATUS", "ts": time.time(), "mode": searching,
                    "locked_combo": self._locked, "candidates_total": self._candidate_count,
                    "decoded_total": self._decoded_count, "rejected_total": self._rejected_count,
                    "chunk_candidates": len(candidates), "chunk_decoded": chunk_decoded,
                    "chunk_rejected": chunk_rejected, "signal_power_dbfs": power_db})

    def _run(self):
        chunk = int(self.sample_rate * DEFAULT_CHUNK_SECONDS)
        overlap_tail = np.array([], dtype=np.complex64)
        overlap_n = int(self.sample_rate * 0.2)
        try:
            while self._running:
                block = self._sdr.read(chunk)
                buf = np.concatenate([overlap_tail, block])
                self._process_buffer(buf)
                self._process_cw(block)
                overlap_tail = block[-overlap_n:]
        except Exception as e:
            self._emit({"kind": "SDR_ERROR", "ts": time.time(),
                        "error": f"{e}\n{traceback.format_exc()}"})


# =====================================================================
# Full test battery -- continuous automated bench run against the
# satellite's DEBUG_BUILD UART backdoor.
# =====================================================================

class TestBattery:
    STEPS = [
        ("TESTCW", 5.0),
        ("TESTBEACON1", 3.0),
        ("TESTBEACON2", 3.0),
        ("TX 48656C6C6F", 3.0),  # "Hello" as a known-good raw payload
    ]

    def __init__(self, link: SerialLink, emit_fn):
        self.link = link
        self._emit = emit_fn
        self._thread = None
        self._running = False
        self.cycle_count = 0
        self.step_count = 0

    @property
    def running(self) -> bool:
        return self._running

    def start(self):
        if self._running:
            return
        self._running = True
        self.cycle_count = 0
        self.step_count = 0
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()
        self._emit({"kind": "BATTERY_STARTED", "ts": time.time()})

    def stop(self):
        self._running = False
        self._emit({"kind": "BATTERY_STOPPING", "ts": time.time()})

    def _run(self):
        try:
            while self._running:
                self.cycle_count += 1
                for cmd, delay_s in self.STEPS:
                    if not self._running:
                        break
                    self.step_count += 1
                    try:
                        self.link.send_line(cmd)
                        self._emit({"kind": "BATTERY_STEP", "ts": time.time(),
                                    "cycle": self.cycle_count, "step": self.step_count,
                                    "command": cmd, "ok": True})
                    except Exception as e:
                        self._emit({"kind": "BATTERY_STEP", "ts": time.time(),
                                    "cycle": self.cycle_count, "step": self.step_count,
                                    "command": cmd, "ok": False, "error": str(e)})
                        self._running = False
                        break
                    slept = 0.0
                    while self._running and slept < delay_s:
                        time.sleep(0.1)
                        slept += 0.1
        finally:
            self._running = False
            self._emit({"kind": "BATTERY_STOPPED", "ts": time.time(),
                        "cycles": self.cycle_count, "steps": self.step_count})


# =====================================================================
# Pane -- one of the two independent columns in the UI, holding
# whichever of {SerialLink, SdrSession} is currently active, plus its
# own WebSocket fan-out.
# =====================================================================

class Pane:
    def __init__(self, pane_id: str, loop: asyncio.AbstractEventLoop):
        self.pane_id = pane_id
        self.role = None  # "satellite" | "ground" | "sdr" | None
        self.link: SerialLink | None = None
        self.sdr: SdrSession | None = None
        self.battery: TestBattery | None = None
        self._loop = loop
        self._ws_clients: set[WebSocket] = set()

    def emit(self, event: dict):
        event = dict(event)
        event["pane"] = self.pane_id
        asyncio.run_coroutine_threadsafe(self._broadcast(event), self._loop)

    async def _broadcast(self, event: dict):
        dead = []
        for ws in list(self._ws_clients):
            try:
                await ws.send_json(event)
            except Exception:
                dead.append(ws)
        for ws in dead:
            self._ws_clients.discard(ws)

    def set_role(self, role: str):
        self.teardown()
        self.role = role
        if role == "satellite":
            self.link = SerialLink("satellite", self.emit)
        elif role == "ground":
            self.link = SerialLink("ground", self.emit)
        elif role == "sdr":
            self.sdr = SdrSession(self.emit)

    def teardown(self):
        if self.battery is not None:
            self.battery.stop()
            self.battery = None
        if self.link is not None:
            self.link.disconnect()
            self.link = None
        if self.sdr is not None:
            self.sdr.disconnect()
            self.sdr = None
        self.role = None

    def require_link(self) -> SerialLink:
        if self.link is None:
            raise RuntimeError(f"pane {self.pane_id} is not a serial (satellite/ground) role")
        return self.link

    def require_sdr(self) -> SdrSession:
        if self.sdr is None:
            raise RuntimeError(f"pane {self.pane_id} is not the sdr role")
        return self.sdr

    def status(self) -> dict:
        st = {"pane": self.pane_id, "role": self.role}
        if self.link is not None:
            st.update({"connected": self.link.connected, "port": self.link.port,
                       "baud": self.link.baud, "cycle_phase": self.link.cycle_phase})
            if self.battery is not None:
                st["battery_running"] = self.battery.running
        elif self.sdr is not None:
            st.update({"connected": self.sdr.connected, "freq_hz": self.sdr.freq_hz,
                       "sample_rate": self.sdr.sample_rate, "baud": self.sdr.baud})
        return st


# =====================================================================
# FastAPI app
# =====================================================================

app = FastAPI(title="S2S2 Test Suite")
_panes: dict[str, Pane] = {}

# =====================================================================
# 100-test fault injection suite (fault_injection_suite.py) -- global,
# not tied to a pane, since it runs entirely against the real compiled
# firmware decode math in software, no hardware connection needed.
# =====================================================================
_fault_suite_loop = None
_fault_suite_ws_clients: set = set()
_fault_suite_state = {"running": False, "results": [], "total": 0, "passed": 0,
                       "started_at": None, "finished_at": None}


@app.on_event("startup")
async def _startup():
    loop = asyncio.get_event_loop()
    _panes["a"] = Pane("a", loop)
    _panes["b"] = Pane("b", loop)
    global _fault_suite_loop
    _fault_suite_loop = loop


def _pane(pane_id: str) -> Pane:
    if pane_id not in _panes:
        raise ValueError(f"unknown pane {pane_id!r} (must be 'a' or 'b')")
    return _panes[pane_id]


class RoleRequest(BaseModel):
    role: str  # "satellite" | "ground" | "sdr" | "none"


class SerialConnectRequest(BaseModel):
    port: str
    baud: int = 115200


class SdrConnectRequest(BaseModel):
    freq_hz: float = DEFAULT_SDR_FREQ_HZ
    sample_rate: float = DEFAULT_SDR_SAMPLE_RATE
    gain: str = "auto"
    baud: float = DEFAULT_SDR_BAUD
    cw_threshold_dbfs: float = -20.0
    cw_bandwidth_hz: float = 200.0


class RawCommandRequest(BaseModel):
    text: str


class TxPowerRequest(BaseModel):
    dbm: int


class SendMessageRequest(BaseModel):
    hex: str


class TestDigiRequest(BaseModel):
    hex: str


class TxCwRequest(BaseModel):
    text: str


class OnOffRequest(BaseModel):
    on: bool


@app.get("/api/ports")
def list_ports():
    import serial.tools.list_ports
    return [{"device": p.device, "description": p.description} for p in serial.tools.list_ports.comports()]


@app.post("/api/pane/{pane_id}/role")
def set_role(pane_id: str, req: RoleRequest):
    pane = _pane(pane_id)
    if req.role == "none":
        pane.teardown()
    else:
        if req.role not in ("satellite", "ground", "sdr"):
            return {"ok": False, "error": "role must be satellite, ground, sdr or none"}
        pane.set_role(req.role)
    return {"ok": True, "status": pane.status()}


@app.post("/api/pane/{pane_id}/connect")
def connect(pane_id: str, req: SerialConnectRequest):
    pane = _pane(pane_id)
    try:
        pane.require_link().connect(req.port, req.baud)
        return {"ok": True, "status": pane.status()}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@app.post("/api/pane/{pane_id}/connect_sdr")
def connect_sdr(pane_id: str, req: SdrConnectRequest):
    pane = _pane(pane_id)
    try:
        pane.require_sdr().connect(req.freq_hz, req.sample_rate, req.gain, req.baud,
                                    req.cw_threshold_dbfs, req.cw_bandwidth_hz)
        return {"ok": True, "status": pane.status()}
    except Exception as e:
        return {"ok": False, "error": f"{e}\n{traceback.format_exc()}"}


@app.post("/api/pane/{pane_id}/disconnect")
def disconnect(pane_id: str):
    pane = _pane(pane_id)
    if pane.link is not None:
        pane.link.disconnect()
    if pane.sdr is not None:
        pane.sdr.disconnect()
    return {"ok": True, "status": pane.status()}


@app.get("/api/pane/{pane_id}/status")
def pane_status(pane_id: str):
    return _pane(pane_id).status()


@app.post("/api/pane/{pane_id}/raw")
def send_raw(pane_id: str, req: RawCommandRequest):
    try:
        _pane(pane_id).require_link().send_line(req.text)
        return {"ok": True}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@app.post("/api/pane/{pane_id}/tx_power")
def tx_power(pane_id: str, req: TxPowerRequest):
    if not (MIN_POWER_DBM <= req.dbm <= MAX_POWER_DBM):
        return {"ok": False, "error": f"dbm must be in [{MIN_POWER_DBM},{MAX_POWER_DBM}]"}
    try:
        _pane(pane_id).require_link().send_line(f"POWER {req.dbm}")
        return {"ok": True}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@app.post("/api/pane/{pane_id}/send_message")
def send_message(pane_id: str, req: SendMessageRequest):
    h = req.hex.strip()
    if len(h) % 2 != 0 or not all(c in "0123456789abcdefABCDEF" for c in h):
        return {"ok": False, "error": "hex must be even-length 0-9/a-f characters"}
    try:
        _pane(pane_id).require_link().send_line(f"TX {h}")
        return {"ok": True}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@app.post("/api/pane/{pane_id}/testdigi")
def testdigi(pane_id: str, req: TestDigiRequest):
    h = req.hex.strip()
    expected_len = dec.FX25_FRAME_LEN * 2
    if len(h) != expected_len or not all(c in "0123456789abcdefABCDEF" for c in h):
        return {"ok": False, "error": f"hex must be exactly {expected_len} 0-9/a-f characters "
                                       f"({dec.FX25_FRAME_LEN}-byte FX.25 frame)"}
    try:
        _pane(pane_id).require_link().send_line(f"TESTDIGI {h}")
        return {"ok": True}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@app.post("/api/pane/{pane_id}/txcw")
def txcw(pane_id: str, req: TxCwRequest):
    text = req.text.strip()
    if not text:
        return {"ok": False, "error": "text must not be empty"}
    try:
        _pane(pane_id).require_link().send_line(f"TXCW {text}")
        return {"ok": True}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@app.post("/api/pane/{pane_id}/memstat")
def memstat(pane_id: str):
    """Triggers a real MEMSTAT read off the running chip (core/Src/
    mem_stats.c's stack high-water-mark) -- not the static .elf-based
    /api/memory_profile above. Poll this on an interval from the UI for
    a live-updating number; the firmware answers instantly, no polling
    cost on its side beyond one UART line."""
    try:
        _pane(pane_id).require_link().send_line("MEMSTAT")
        return {"ok": True}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@app.post("/api/pane/{pane_id}/beacon1")
def beacon1(pane_id: str):
    try:
        _pane(pane_id).require_link().send_line("TESTBEACON1")
        return {"ok": True}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@app.post("/api/pane/{pane_id}/beacon2")
def beacon2(pane_id: str):
    try:
        _pane(pane_id).require_link().send_line("TESTBEACON2")
        return {"ok": True}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@app.post("/api/pane/{pane_id}/cw")
def cw(pane_id: str):
    try:
        _pane(pane_id).require_link().send_line("TESTCW")
        return {"ok": True}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@app.post("/api/pane/{pane_id}/beacon_toggle")
def beacon_toggle(pane_id: str, req: OnOffRequest):
    try:
        _pane(pane_id).require_link().send_line("BEACON ON" if req.on else "BEACON OFF")
        return {"ok": True}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@app.post("/api/pane/{pane_id}/satmode")
def satmode(pane_id: str, req: OnOffRequest):
    try:
        _pane(pane_id).require_link().send_line("SATMODE ON" if req.on else "SATMODE OFF")
        return {"ok": True}
    except Exception as e:
        return {"ok": False, "error": str(e)}


@app.post("/api/pane/{pane_id}/battery/start")
def battery_start(pane_id: str):
    pane = _pane(pane_id)
    try:
        link = pane.require_link()
    except Exception as e:
        return {"ok": False, "error": str(e)}
    if not link.connected:
        return {"ok": False, "error": "not connected"}
    if pane.battery is None:
        pane.battery = TestBattery(link, pane.emit)
    pane.battery.start()
    return {"ok": True}


@app.post("/api/pane/{pane_id}/battery/stop")
def battery_stop(pane_id: str):
    pane = _pane(pane_id)
    if pane.battery is not None:
        pane.battery.stop()
    return {"ok": True}


@app.websocket("/ws/fault_suite")
async def ws_fault_suite(websocket: WebSocket):
    # Registered BEFORE the generic /ws/{pane_id} route below on purpose:
    # Starlette matches routes in registration order, and {pane_id} is a
    # wildcard that would otherwise swallow this literal path too,
    # treating "fault_suite" as a pane id and failing _pane() lookup
    # (confirmed the hard way -- this crashed the connection on first try).
    await websocket.accept()
    _fault_suite_ws_clients.add(websocket)
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        _fault_suite_ws_clients.discard(websocket)


@app.websocket("/ws/{pane_id}")
async def ws_endpoint(websocket: WebSocket, pane_id: str):
    await websocket.accept()
    pane = _pane(pane_id)
    pane._ws_clients.add(websocket)
    try:
        while True:
            await websocket.receive_text()  # client sends nothing meaningful; just keep alive
    except WebSocketDisconnect:
        pass
    finally:
        pane._ws_clients.discard(websocket)


@app.get("/api/gs_commands")
def gs_commands():
    return GS_CLI_COMMANDS


async def _broadcast_fault_event(event: dict):
    dead = []
    for ws in list(_fault_suite_ws_clients):
        try:
            await ws.send_json(event)
        except Exception:
            dead.append(ws)
    for ws in dead:
        _fault_suite_ws_clients.discard(ws)


def _emit_fault_event(event: dict):
    if _fault_suite_loop is not None:
        asyncio.run_coroutine_threadsafe(_broadcast_fault_event(event), _fault_suite_loop)


def _run_fault_suite_in_background():
    _fault_suite_state["running"] = True
    _fault_suite_state["results"] = []
    _fault_suite_state["total"] = 0
    _fault_suite_state["passed"] = 0
    _fault_suite_state["started_at"] = time.time()
    _emit_fault_event({"kind": "FAULT_SUITE_STARTED", "ts": time.time()})

    def progress(r, i, n):
        entry = {"id": r.id, "category": r.category, "name": r.name,
                  "passed": r.passed, "detail": r.detail}
        _fault_suite_state["results"].append(entry)
        _fault_suite_state["total"] = n
        _fault_suite_state["passed"] = sum(1 for x in _fault_suite_state["results"] if x["passed"])
        _emit_fault_event({"kind": "FAULT_SUITE_PROGRESS", "ts": time.time(),
                            "index": i, "total": n, **entry})

    try:
        state = fault_suite.run_all(progress_cb=progress)
        _fault_suite_state["finished_at"] = time.time()
        _emit_fault_event({"kind": "FAULT_SUITE_DONE", "ts": time.time(),
                            "passed": state.passed_count, "total": state.total,
                            "link_budget_note": fault_suite.LINK_BUDGET_NOTE})
    except Exception as e:
        _emit_fault_event({"kind": "FAULT_SUITE_ERROR", "ts": time.time(),
                            "error": f"{e}\n{traceback.format_exc()}"})
    finally:
        _fault_suite_state["running"] = False


@app.post("/api/fault_suite/start")
def fault_suite_start():
    if _fault_suite_state["running"]:
        return {"ok": False, "error": "already running"}
    threading.Thread(target=_run_fault_suite_in_background, daemon=True).start()
    return {"ok": True}


@app.get("/api/fault_suite/status")
def fault_suite_status():
    return {"ok": True, **_fault_suite_state}


@app.get("/api/memory_profile")
def memory_profile():
    return get_memory_profile()


@app.get("/api/pane/{pane_id}/activity")
def pane_activity(pane_id: str):
    pane = _pane(pane_id)
    if pane.link is None:
        return {"ok": False, "error": "pane is not a serial (satellite/ground) role"}
    return {"ok": True, **pane.link.activity_snapshot()}


if os.path.isdir(STATIC_DIR):
    app.mount("/static", StaticFiles(directory=STATIC_DIR), name="static")


@app.get("/")
def index():
    return FileResponse(os.path.join(STATIC_DIR, "index.html"))


if __name__ == "__main__":
    import uvicorn
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=7910)
    args = parser.parse_args()
    uvicorn.run(app, host=args.host, port=args.port)
