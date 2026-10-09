#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
STM32WL55JC Ground Station - GMSK Command & Downlink Console
Antarikchya Pratisthan Nepal - APN  |  अन्तरिक्ष प्रतिष्ठान नेपाल
Location: /home/prem/Desktop/nuttxspace/Ground_Station/gs_communicator.py

Talks to Ground Station firmware (gs_app.c / gs_main.c) over UART2 / ST-LINK VCP (115200 8N1).

Key Operational Features:
- DOWNLINK RX: 435.000 MHz (4800 bps GMSK AX.25 UI frames)
- UPLINK TX:   437.375 MHz (13-Byte Telecommands: HK requests, Camera trigger, Flash downloads)
- LIGHT THEME: Crisp modern aerospace interface (white/slate palette)
- DEDICATED TABS:
    1. GMSK Uplink Commands (Send arbitrary number of flash packets, HK requests, ACK monitor)
    2. Downlink HK & Flash Data (Voltages, Temps, Currents, IMU, 128B Hex flash inspector)
    3. Raw Serial Terminal (Color-tagged console, clear, export)

FIXES IN THIS REVISION:
  - Flash telecommand hex builder now correctly emits 0xF2 in data_id byte 6
    (was 0x00, causing satellite-side command rejection).
  - Flash decoder handles variable-length 128-byte payloads safely
    (f_end only checked when len >= 128).
  - Serial writer flushes and has a small inter-write delay.
  - Connection watchdog detects USB unplug and updates UI state.
  - ACK banner auto-clears after configurable timeout.
  - Improved JSON/hex telemetry routing and error handling.
==============================================================================
"""

import sys
import os
import re
import json
import time
import queue
import glob
import math
import threading
import io
import socket
import subprocess
try:
    from PIL import Image, ImageTk, ImageFile
    ImageFile.LOAD_TRUNCATED_IMAGES = True
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("[ERROR] pyserial is required. Install with: pip3 install pyserial")
    sys.exit(1)

# ============================================================================
# BRANDING & SATELLITE MISSION CONSTANTS
# ============================================================================

ORG_EN = "Antarikchya Pratisthan Nepal - APN"
ORG_NE = "अन्तरिक्ष प्रतिष्ठान नेपाल"
DEVANAGARI_FONTS = [
    "/usr/share/fonts/truetype/lohit-devanagari/Lohit-Devanagari.ttf",
    "/usr/share/fonts/truetype/Gargi/Gargi.ttf",
    "/usr/share/fonts/truetype/samyak/Samyak-Devanagari.ttf",
    "/usr/share/fonts/truetype/freefont/FreeSansBold.ttf",
    "C:/Windows/Fonts/Nirmala.ttf",
    "C:/Windows/Fonts/mangal.ttf",
]


def render_devanagari(text, size, color, bg="#FFFFFF"):
    """Tk 8.6 cannot shape Devanagari conjuncts (न्त, क्ष, ष्ठ show broken),
    so draw the text with Pillow (libraqm shaping) and return a PhotoImage."""
    if not HAS_PIL:
        return None
    try:
        from PIL import ImageDraw, ImageFont, features
        if not features.check("raqm"):
            return None
        path = next((f for f in DEVANAGARI_FONTS if os.path.exists(f)), None)
        if not path:
            return None
        font = ImageFont.truetype(path, size, layout_engine=ImageFont.Layout.RAQM)
        l, t, r, b = font.getbbox(text, language="ne")
        img = Image.new("RGB", (r - l + 4, b - t + 4), bg)
        ImageDraw.Draw(img).text((2 - l, 2 - t), text, font=font, fill=color, language="ne")
        return ImageTk.PhotoImage(img)
    except Exception:
        return None
APP_TITLE = "S2S-2 / NEPSAT Ground Station Console"

SAT_CALLSIGN = "9N2SJ"
GS_CALLSIGN = "GROUND"

RX_FREQ_STR = "435.000 MHz (Downlink)"
TX_FREQ_STR = "437.375 MHz (Uplink)"

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
LOGO_PATH = os.path.join(SCRIPT_DIR, "antarikshya_logo.png")
FLAG_PATH = os.path.join(SCRIPT_DIR, "nepal_flag.png")
FLAG_HEADER_PATH = os.path.join(SCRIPT_DIR, "nepal_flag_header.png")
GMSK_LOG_FILE = os.path.join(SCRIPT_DIR, "gmsk_log.csv")
FLASH_DUMP_FILE = os.path.join(SCRIPT_DIR, "downloaded_flash_data.bin")
EXCEL_EXPORT_PATH = os.path.join(SCRIPT_DIR, "ground_station_telemetry.xlsx")

# Firmware opcodes (strictly matching Packet Format.xlsx and firmware)
OPCODE_CAM_DOWN   = bytes([0x1D, 0xD2, 0xF5])  # cam.txt download (MCU 0x01)
OPCODE_HK         = bytes([0x1D, 0xD1, 0xF2])  # sat_health.txt / HK download (MCU 0x01)
OPCODE_FLASH      = bytes([0x1D, 0xD1, 0xF8])  # Raw Flash storage download (MCU 0x01)
OPCODE_CAM_ON     = bytes([0xCC, 0x5E, 0xBD])  # Real camera capture mission (MCU 0x04)
OPCODE_CAM_DUMMY  = bytes([0xCC, 0x5E, 0xBE])  # Dummy camera test (MCU 0x04)
OPCODE_ADCS       = bytes([0xA0, 0x53, 0xCF])  # ADCS mission (MCU 0x03)
OPCODE_EPDM       = bytes([0xEC, 0xCF, 0xCF])  # EPDM mission (MCU 0x05)

MCU_ID_OBC  = 0x01
MCU_ID_ADCS = 0x03
MCU_ID_CAM  = 0x04
MCU_ID_EPDM = 0x05

DATA_ID_DEFAULT = 0x0000
DATA_ID_DUMMY   = 0x00D0

# ACK banner auto-clear
ACK_AUTO_CLEAR_SEC = 12


# ============================================================================
# HELPER PARSERS & PROTOCOL FORMATTERS
# ============================================================================

def format_hex(data):
    return " ".join("%02X" % b for b in data)


def build_telecommand_hex(mcu_id, opcode_bytes, data_id, addr, count):
    """
    Build the 13-byte telecommand hex string used by the firmware:
        53 <MCU_ID> <OP0> <OP1> <OP2> <DATA_HI> <DATA_LO> <ADDR4> <CNT2>

    Returns (hex_string, raw_bytes).
    """
    a3 = (addr >> 24) & 0xFF
    a2 = (addr >> 16) & 0xFF
    a1 = (addr >> 8) & 0xFF
    a0 = addr & 0xFF
    d_hi = (data_id >> 8) & 0xFF
    d_lo = data_id & 0xFF
    c_hi = (count >> 8) & 0xFF
    c_lo = count & 0xFF

    raw = bytes([
        0x53, mcu_id,
        opcode_bytes[0], opcode_bytes[1], opcode_bytes[2],
        d_hi, d_lo,
        a3, a2, a1, a0,
        c_hi, c_lo
    ])
    hex_str = " ".join("%02X" % b for b in raw)
    return hex_str, raw


# ============================================================================
# FLASH PAYLOAD DECODER
# ============================================================================
# Firmware decodes structured 128-byte telemetry between header and footer:
#   Bytes   0..31  : ADC1 (16 channels x int16)
#   Bytes  32..33  : Footer 1 (0xAA55)
#   Bytes  34..57  : ADC2 (12 channels x int16)
#   Bytes  58..69  : IMU  (6 channels x int16: Gyro X/Y/Z, Mag X/Y/Z, signed)
#   Bytes  70..71  : Footer 2 (0xBB66)
#   Bytes 126..127 : Data Footer (0xAACC)  [only if len >= 128]
# ============================================================================

def decode_flash_payload_bytes(raw_bytes):
    import struct
    if len(raw_bytes) < 72:
        return None
    try:
        adc1 = struct.unpack('<16h', raw_bytes[0:32])
        f1 = struct.unpack('<H', raw_bytes[32:34])[0]
        adc2 = struct.unpack('<12h', raw_bytes[34:58])
        imu = struct.unpack('<6h', raw_bytes[58:70])
        f2 = struct.unpack('<H', raw_bytes[70:72])[0]
        # Data end footer is only present when the full 128-byte packet was received
        if len(raw_bytes) >= 128:
            f_end = struct.unpack('<H', raw_bytes[126:128])[0]
        else:
            f_end = None
        return {
            'adc1': adc1, 'f1': f1,
            'adc2': adc2, 'imu': imu, 'f2': f2, 'f_end': f_end
        }
    except Exception:
        return None



# ============================================================================
# SDR & CAMERA CHUNK EXTRACTION ENGINE (ADVANCED)
# ============================================================================

def unkiss_frame(raw: bytes) -> bytes:
    """Extract and un-escape KISS frame payload (RFC-like / AX.25)."""
    if raw.startswith(b"\xc0") and raw.endswith(b"\xc0"):
        raw = raw[1:-1]
    if len(raw) > 0 and (raw[0] & 0x0F) == 0x00:  # Data frame on port 0
        raw = raw[1:]
    out = bytearray()
    i = 0
    while i < len(raw):
        if raw[i] == 0xDB and i + 1 < len(raw):
            if raw[i + 1] == 0xDC:
                out.append(0xC0)
                i += 2
                continue
            elif raw[i + 1] == 0xDD:
                out.append(0xDB)
                i += 2
                continue
        out.append(raw[i])
        i += 1
    return bytes(out)


def ax25_crc16(data: bytes) -> int:
    """AX.25 CCITT CRC-16 (Polynomial 0x8408, initial 0xFFFF, inverted output)."""
    crc = 0xFFFF
    for byte in data:
        for _ in range(8):
            bit = (byte ^ crc) & 1
            crc >>= 1
            if bit:
                crc ^= 0x8408
            byte >>= 1
    return (~crc) & 0xFFFF


def decode_ax25_frame(raw_frame: bytes):
    """
    Decodes an un-scrambled AX.25 UI frame.
    Returns dict with dest, src, ctrl, pid, payload, crc_ok or None if invalid.
    """
    if not raw_frame or len(raw_frame) < 16:
        return None
    frame = raw_frame
    while frame.startswith(b"\x7e"):
        frame = frame[1:]
    while frame.endswith(b"\x7e"):
        frame = frame[:-1]
    if len(frame) < 16:
        return None

    try:
        dest = "".join(chr((b >> 1) & 0x7F) for b in frame[0:6]).strip()
        src = "".join(chr((b >> 1) & 0x7F) for b in frame[7:13]).strip()
        ctrl = frame[14]
        pid = frame[15]
        payload = frame[16:-2] if len(frame) >= 18 else frame[16:]

        crc = 0xFFFF
        for byte in frame:
            for _ in range(8):
                bit = (byte ^ crc) & 1
                crc >>= 1
                if bit:
                    crc ^= 0x8408
                byte >>= 1
        crc_ok = (crc == 0xF0B8)

        return {
            "dest": dest,
            "src": src,
            "ctrl": ctrl,
            "pid": pid,
            "payload": payload,
            "crc_ok": crc_ok,
            "raw_frame": frame
        }
    except Exception:
        return None


def decode_g3ruh_ax25(wire_bytes: bytes):
    """
    Full Software G3RUH 9600/4800 Baud Descrambler + NRZ-I Decoder + HDLC Destuffer + AX.25 Engine.
    Polynomial: 1 + x^12 + x^17 (taps 11 and 16, 17-bit shift register 0x1FFFF).
    Decodes 200-byte wire frames from STM32WL55 or SDR into validated AX.25 frames.
    """
    frames = []
    if not wire_bytes or len(wire_bytes) < 18:
        return frames

    # Try both standard initial shift register state (0x1FFFF) and zero state (0x00000)
    for init_sr in (0x1FFFF, 0x00000):
        sr = init_sr
        last_bit = 1
        flag_shift = 0
        in_frame = False
        frame_bits = []

        for b in wire_bytes:
            for bit_idx in range(7, -1, -1):
                wire_bit = (b >> bit_idx) & 1
                feedback = ((sr >> 11) ^ (sr >> 16)) & 1
                descrambled = wire_bit ^ feedback
                sr = ((sr << 1) | wire_bit) & 0x1FFFF

                data_bit = 1 if descrambled == last_bit else 0
                last_bit = descrambled

                flag_shift = ((flag_shift >> 1) | (data_bit << 7)) & 0xFF

                if flag_shift == 0x7E:
                    if in_frame:
                        if len(frame_bits) >= 7:
                            content_bits = frame_bits[:-7]
                            unstuffed_bits = []
                            ones = 0
                            violation = False
                            for bit in content_bits:
                                if ones == 5:
                                    if bit != 0:
                                        violation = True
                                        break
                                    ones = 0
                                    continue
                                if bit == 1:
                                    ones += 1
                                else:
                                    ones = 0
                                unstuffed_bits.append(bit)

                            if not violation and len(unstuffed_bits) >= (18 * 8) and (len(unstuffed_bits) % 8 == 0):
                                unstuffed = bytearray()
                                for i in range(0, len(unstuffed_bits), 8):
                                    byte_val = 0
                                    for j in range(8):
                                        byte_val |= (unstuffed_bits[i + j] << j)
                                    unstuffed.append(byte_val)

                                crc = 0xFFFF
                                for byte in unstuffed:
                                    for _ in range(8):
                                        bit = (byte ^ crc) & 1
                                        crc >>= 1
                                        if bit:
                                            crc ^= 0x8408
                                        byte >>= 1
                                if crc == 0xF0B8:
                                    frame_data = bytes(unstuffed)
                                    if frame_data not in [f["raw_frame"] for f in frames]:
                                        dest = "".join(chr((b >> 1) & 0x7F) for b in frame_data[0:6]).strip()
                                        src = "".join(chr((b >> 1) & 0x7F) for b in frame_data[7:13]).strip()
                                        ctrl = frame_data[14]
                                        pid = frame_data[15]
                                        payload = frame_data[16:-2]
                                        frames.append({
                                            "dest": dest,
                                            "src": src,
                                            "ctrl": ctrl,
                                            "pid": pid,
                                            "payload": payload,
                                            "raw_frame": frame_data
                                        })
                        frame_bits = []
                    else:
                        in_frame = True
                        frame_bits = []
                    continue

                if in_frame:
                    frame_bits.append(data_bit)
    return frames


def extract_camera_chunks_from_bytes(b: bytes):
    """
    Finds all camera frames in raw bytes.
    Supports:
      1. Direct 0xCA 0xFE (or 0xFE 0xCA) 14-byte header chunks.
      2. G3RUH scrambled on-air bitstreams (descrambled + NRZI + HDLC destuffing).
      3. Unscrambled AX.25 UI frames (payload extracted).
      4. KISS-wrapped frames (RFC / Direwolf).
    Returns list of tuples: (pkt_idx, total_p, offset, data_bytes).
    """
    import struct
    results = []
    if not b:
        return results

    # 1. Unkiss if KISS framed
    if b.startswith(b"\xc0") and b.endswith(b"\xc0"):
        b = unkiss_frame(b)

    def _scan_chunks(data: bytes):
        res = []
        for magic in (b"\xca\xfe", b"\xfe\xca"):
            idx = data.find(magic)
            while idx != -1:
                if len(data) >= idx + 14:
                    hdr = data[idx : idx + 14]
                    p_idx, total_p = struct.unpack("<HH", hdr[2:6])
                    offset = struct.unpack("<I", hdr[6:10])[0]
                    dlen = struct.unpack("<H", hdr[10:12])[0]
                    if dlen == 0 or dlen > 128:
                        dlen = 128
                    if total_p == 0:
                        total_p = 69
                    if total_p <= 500 and p_idx < total_p:
                        if len(data) >= idx + 14 + dlen:
                            chunk_data = data[idx + 14 : idx + 14 + dlen]
                            res.append((p_idx, total_p, offset, chunk_data))
                            idx += 14 + dlen
                            continue
                idx = data.find(magic, idx + 1)
        return res

    # 2. Check directly in b
    direct = _scan_chunks(b)
    if direct:
        results.extend(direct)

    # 3. Try G3RUH descrambler
    g3ruh_frames = decode_g3ruh_ax25(b)
    for f in g3ruh_frames:
        chunks = _scan_chunks(f["payload"])
        if chunks:
            results.extend(chunks)

    # 4. Try AX.25 frame parser (if frame wasn't scrambled or was already descrambled)
    if not results and len(b) >= 30:
        parsed_ax = decode_ax25_frame(b)
        if parsed_ax and parsed_ax.get("payload"):
            chunks = _scan_chunks(parsed_ax["payload"])
            if chunks:
                results.extend(chunks)

    # De-duplicate results while preserving order
    seen = set()
    unique_results = []
    for c in results:
        key = (c[0], c[1], c[2], len(c[3]))
        if key not in seen:
            seen.add(key)
            unique_results.append(c)

    return unique_results


def extract_chunks_from_text(text: str):
    """
    Extracts camera chunks from raw text lines, SDR logs, Dire Wolf output, or hex dumps.
    Extracts tokens across lines for multi-row hex dumps (e.g. 13 rows x 16 bytes = 200 bytes).
    """
    found = []
    if not text:
        return found

    # Step 1: Check individual lines for tags like [CAMERA-HEX]
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if "[CAMERA-HEX]" in line:
            clean = line.replace("[CAMERA-HEX]", "").strip()
            tokens = re.findall(r"\b[0-9a-fA-F]{2}\b", clean)
            if tokens:
                try:
                    raw_bytes = bytes([int(x, 16) for x in tokens])
                    sub = extract_camera_chunks_from_bytes(raw_bytes)
                    if sub:
                        found.extend(sub)
                    elif len(raw_bytes) >= 14:
                        found.append((None, 69, None, raw_bytes))
                except Exception:
                    pass

    # Step 2: Extract ALL hex tokens from the entire text blob
    all_tokens = re.findall(r"\b[0-9a-fA-F]{2}\b", text)
    if len(all_tokens) >= 14:
        try:
            raw_all = bytes([int(x, 16) for x in all_tokens])
            sub = extract_camera_chunks_from_bytes(raw_all)
            if sub:
                found.extend(sub)
        except Exception:
            pass

    # De-duplicate
    seen = set()
    unique_found = []
    for c in found:
        key = (c[0], c[1], c[2], len(c[3]))
        if key not in seen:
            seen.add(key)
            unique_found.append(c)

    return unique_found


class SDRSocketServer:
    """
    Background TCP/UDP listener for Dire Wolf / Soundmodem / SDR frames on port 8001.
    """
    def __init__(self, port, chunk_callback, log_callback=None):
        self.port = port
        self.chunk_callback = chunk_callback
        self.log_callback = log_callback
        self.running = False
        self.tcp_sock = None
        self.udp_sock = None
        self.thread = None

    def start(self):
        if self.running:
            return
        self.running = True
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def stop(self):
        self.running = False
        if self.tcp_sock:
            try: self.tcp_sock.close()
            except Exception: pass
        if self.udp_sock:
            try: self.udp_sock.close()
            except Exception: pass

    def _run(self):
        try:
            self.udp_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.udp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.udp_sock.bind(("0.0.0.0", self.port))
            self.udp_sock.settimeout(0.5)
        except Exception as e:
            if self.log_callback:
                self.log_callback(f"[SDR-UDP] Bind error on port {self.port}: {e}\n", "error")

        try:
            self.tcp_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            self.tcp_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            self.tcp_sock.bind(("0.0.0.0", self.port))
            self.tcp_sock.listen(3)
            self.tcp_sock.settimeout(0.5)
        except Exception as e:
            if self.log_callback:
                self.log_callback(f"[SDR-TCP] Bind error on port {self.port}: {e}\n", "error")

        if self.log_callback:
            self.log_callback(f"[SDR-SERVER] Active on port {self.port} (TCP/UDP KISS & Raw). Ready for SDR.\n", "info")

        while self.running:
            if self.udp_sock:
                try:
                    data, addr = self.udp_sock.recvfrom(2048)
                    if data:
                        self._process_data(data, f"UDP:{addr[0]}")
                except socket.timeout:
                    pass
                except Exception:
                    pass

            if self.tcp_sock:
                try:
                    client, c_addr = self.tcp_sock.accept()
                    client.settimeout(0.2)
                    threading.Thread(target=self._handle_tcp_client, args=(client, c_addr), daemon=True).start()
                except socket.timeout:
                    pass
                except Exception:
                    pass

    def _handle_tcp_client(self, client, c_addr):
        if self.log_callback:
            self.log_callback(f"[SDR-CLIENT] Connected from {c_addr[0]}:{c_addr[1]}\n", "info")
        buf = bytearray()
        while self.running:
            try:
                data = client.recv(1024)
                if not data:
                    break
                buf.extend(data)
                while b"\xc0" in buf:
                    idx = buf.find(b"\xc0")
                    if idx > 0:
                        frame = buf[: idx + 1]
                        buf = buf[idx + 1 :]
                        self._process_data(bytes(frame), f"TCP:{c_addr[0]}")
                    else:
                        idx2 = buf.find(b"\xc0", 1)
                        if idx2 != -1:
                            frame = buf[: idx2 + 1]
                            buf = buf[idx2 + 1 :]
                            self._process_data(bytes(frame), f"TCP:{c_addr[0]}")
                        else:
                            break
                while b"\n" in buf:
                    n_idx = buf.find(b"\n")
                    line_data = buf[:n_idx].decode("utf-8", errors="replace").strip()
                    buf = buf[n_idx + 1 :]
                    if line_data:
                        self._process_text(line_data, f"TCP:{c_addr[0]}")
            except socket.timeout:
                continue
            except Exception:
                break
        try: client.close()
        except Exception: pass

    def _process_data(self, data: bytes, src: str):
        chunks = extract_camera_chunks_from_bytes(data)
        if not chunks:
            try:
                txt = data.decode("utf-8", errors="ignore")
                chunks = extract_chunks_from_text(txt)
            except Exception:
                pass
        for c in chunks:
            self.chunk_callback(c[0], c[1], c[2], c[3], src)

    def _process_text(self, text: str, src: str):
        chunks = extract_chunks_from_text(text)
        for c in chunks:
            self.chunk_callback(c[0], c[1], c[2], c[3], src)


# ============================================================================
# GUI INTERFACE (TKINTER LIGHT THEME)
# ============================================================================

def run_gui(default_port=None, baudrate=115200):
    try:
        import tkinter as tk
        from tkinter import ttk, scrolledtext, messagebox, filedialog
    except ImportError:
        print("[WARN] Tkinter not available. Falling back to CLI mode.")
        run_cli(default_port, baudrate)
        return

    root = tk.Tk()
    root.title(f"{ORG_NE} | {ORG_EN} - {APP_TITLE}")
    root.geometry("1280x880")
    root.minsize(1040, 720)
    root.configure(bg="#F1F5F9")  # Slate-100 Light Background

    # =========================================================================
    # LIGHT AEROSPACE STYLING SYSTEM
    # =========================================================================
    style = ttk.Style(root)
    style.theme_use("clam")

    BG_MAIN = "#F1F5F9"
    BG_CARD = "#FFFFFF"
    BORDER_COL = "#CBD5E1"
    TEXT_MAIN = "#0F172A"
    TEXT_MUTED = "#475569"
    PRIMARY_BLUE = "#003893"
    CRIMSON = "#DC143C"
    ACTION_BLUE = "#0284C7"
    SUCCESS_GREEN = "#16A34A"

    style.configure(".", background=BG_MAIN, foreground=TEXT_MAIN, font=("Segoe UI", 9))
    style.configure("TFrame", background=BG_MAIN)
    style.configure("Card.TFrame", background=BG_CARD)

    style.configure("TLabelframe", background=BG_CARD, foreground=PRIMARY_BLUE,
                    relief="solid", borderwidth=1)
    style.configure("TLabelframe.Label", background=BG_CARD, foreground=PRIMARY_BLUE,
                    font=("Segoe UI", 10, "bold"))

    style.configure("TNotebook", background=BG_MAIN, borderwidth=0)
    style.configure("TNotebook.Tab", background="#E2E8F0", foreground=TEXT_MUTED,
                    padding=[16, 7], font=("Segoe UI", 9, "bold"))
    style.map("TNotebook.Tab",
              background=[("selected", BG_CARD), ("active", "#F8FAFC")],
              foreground=[("selected", PRIMARY_BLUE), ("active", TEXT_MAIN)])

    style.configure("TButton", background="#E2E8F0", foreground=TEXT_MAIN,
                    borderwidth=1, focuscolor="none", font=("Segoe UI", 9, "bold"))
    style.map("TButton",
              background=[("active", "#CBD5E1"), ("pressed", "#94A3B8")])

    style.configure("Action.TButton", background=ACTION_BLUE, foreground="#FFFFFF")
    style.map("Action.TButton",
              background=[("active", "#0369A1"), ("pressed", "#075985")])

    style.configure("Primary.TButton", background=PRIMARY_BLUE, foreground="#FFFFFF")
    style.map("Primary.TButton",
              background=[("active", "#002766"), ("pressed", "#001A44")])

    style.configure("Crimson.TButton", background=CRIMSON, foreground="#FFFFFF")
    style.map("Crimson.TButton",
              background=[("active", "#B91C1C"), ("pressed", "#991B1B")])

    style.configure("Success.TButton", background=SUCCESS_GREEN, foreground="#FFFFFF")
    style.map("Success.TButton",
              background=[("active", "#15803D"), ("pressed", "#166534")])

    # Shared state
    ser = None
    running = False
    reader_thread = None
    rx_queue = queue.Queue()

    state = {
        "packets_rx": 0,
        "acks_rx": 0,
        "flash_chunks_rx": 0,
        "last_rx_rssi": -115,
        "last_rx_snr": 0,
        "flash_data_bytes": bytearray(),
        "flash_bytes_expected": 0,
        "flash_pkt_total": 0,
        "cam_chunks": {},
        "cam_bytes": bytearray(),
        "cam_total_expected": 69,
        "hk_history": [],
        "real_cam_records": {},
        "dummy_cam_records": {},
        "flash_history": [],
        "current_mission_type": "HK",
        "raw_g3ruh_acc": bytearray(),
    }

    auto_save_excel_var = tk.BooleanVar(value=True)

    def export_all_to_excel(filepath=None, silent=False):
        """
        Exports all received Housekeeping (HK) telemetry, Real Camera chunks,
        Dummy Camera chunks, and Flash data into a clean, beautifully formatted,
        multi-sheet Microsoft Excel (.xlsx) file with topics, structured tables,
        styling, and metadata.
        """
        try:
            import openpyxl
            from openpyxl.styles import Font, PatternFill, Alignment, Border, Side
            from openpyxl.utils import get_column_letter
        except ImportError:
            msg = "openpyxl library is required for Excel export.\nInstall with: pip install openpyxl"
            print(f"[ERROR] {msg}")
            if not silent:
                messagebox.showerror("Export Failed", msg)
            return None

        if not filepath:
            if not silent:
                chosen = filedialog.asksaveasfilename(
                    title="Export Mission Telemetry to Excel",
                    defaultextension=".xlsx",
                    initialfile=f"GS_Telemetry_{time.strftime('%Y%m%d_%H%M%S')}.xlsx",
                    filetypes=[("Excel Workbook", "*.xlsx"), ("All files", "*.*")])
                if not chosen:
                    return None
                out_path = chosen
            else:
                out_path = EXCEL_EXPORT_PATH
        else:
            out_path = filepath

        try:
            wb = openpyxl.Workbook()

            FONT_TITLE = Font(name="Segoe UI", size=15, bold=True, color="FFFFFF")
            FONT_SUBTITLE = Font(name="Segoe UI", size=10, italic=True, color="1E293B")
            FONT_SECTION = Font(name="Segoe UI", size=12, bold=True, color="003893")
            FONT_TH = Font(name="Segoe UI", size=10, bold=True, color="FFFFFF")
            FONT_TD = Font(name="Segoe UI", size=9, color="0F172A")
            FONT_TD_MONO = Font(name="Consolas", size=9, color="0F172A")

            FILL_HEADER_BLUE = PatternFill(start_color="003893", end_color="003893", fill_type="solid")
            FILL_SUBHEADER = PatternFill(start_color="E2E8F0", end_color="E2E8F0", fill_type="solid")
            FILL_ZEBRA = PatternFill(start_color="F8FAFC", end_color="F8FAFC", fill_type="solid")
            FILL_WHITE = PatternFill(start_color="FFFFFF", end_color="FFFFFF", fill_type="solid")

            BORDER_THIN = Border(
                left=Side(style='thin', color='CBD5E1'),
                right=Side(style='thin', color='CBD5E1'),
                top=Side(style='thin', color='CBD5E1'),
                bottom=Side(style='thin', color='CBD5E1')
            )

            ALIGN_CENTER = Alignment(horizontal="center", vertical="center")
            ALIGN_LEFT = Alignment(horizontal="left", vertical="center")

            def auto_fit_columns(ws, max_cols=25):
                for col in range(1, max_cols + 1):
                    col_letter = get_column_letter(col)
                    max_len = 0
                    for cell in ws[col_letter]:
                        if cell.value is not None:
                            lines = str(cell.value).split("\n")
                            for l in lines:
                                if len(l) > max_len:
                                    max_len = len(l)
                    ws.column_dimensions[col_letter].width = max(max_len + 3, 11)

            # -----------------------------------------------------------------
            # SHEET 1: OVERVIEW & SUMMARY
            # -----------------------------------------------------------------
            ws1 = wb.active
            ws1.title = "Overview & Summary"
            ws1.views.sheetView[0].showGridLines = True

            ws1.merge_cells("A1:G1")
            t1 = ws1["A1"]
            t1.value = f"  {ORG_NE}  |  {ORG_EN}  —  S2S-2 Ground Station Console"
            t1.font = FONT_TITLE
            t1.fill = FILL_HEADER_BLUE
            t1.alignment = Alignment(horizontal="left", vertical="center")
            ws1.row_dimensions[1].height = 36

            ws1.merge_cells("A2:G2")
            s1 = ws1["A2"]
            s1.value = "  Mission Telemetry Acquisition, Housekeeping Sensors & Camera Payload Data Log"
            s1.font = FONT_SUBTITLE
            s1.fill = FILL_SUBHEADER
            s1.alignment = Alignment(horizontal="left", vertical="center")
            ws1.row_dimensions[2].height = 22

            ws1["A4"] = "Mission & Ground Link Specifications"
            ws1["A4"].font = FONT_SECTION

            specs = [
                ("Satellite Name & Callsign", f"{SAT_CALLSIGN} (S2S-2 / NEPSAT)"),
                ("Ground Station Callsign", f"{GS_CALLSIGN}"),
                ("Downlink Carrier / Modulation", f"{RX_FREQ_STR}  •  4800 bps GMSK"),
                ("Uplink Carrier / Modulation", f"{TX_FREQ_STR}  •  AX.25 UI + G3RUH"),
                ("Export Generation Timestamp", time.strftime("%Y-%m-%d %H:%M:%S")),
                ("Primary Ground Station", "Antarikchya Pratisthan Nepal - APN, Kathmandu, Nepal"),
            ]
            for r_idx, (k, v) in enumerate(specs, start=5):
                c_k = ws1.cell(row=r_idx, column=1, value=k)
                c_k.font = Font(name="Segoe UI", size=10, bold=True, color="334155")
                c_k.fill = FILL_ZEBRA
                c_k.border = BORDER_THIN
                ws1.merge_cells(start_row=r_idx, start_column=2, end_row=r_idx, end_column=4)
                c_v = ws1.cell(row=r_idx, column=2, value=v)
                c_v.font = FONT_TD
                c_v.border = BORDER_THIN
                for col_i in range(2, 5):
                    ws1.cell(row=r_idx, column=col_i).border = BORDER_THIN

            ws1["A12"] = "Payload Acquisition Statistics"
            ws1["A12"].font = FONT_SECTION

            hk_count = len(state.get("hk_history", []))
            real_cam_count = len(state.get("real_cam_records", {}))
            dummy_cam_count = len(state.get("dummy_cam_records", {}))
            flash_count = len(state.get("flash_history", []))

            stats = [
                ("Housekeeping (HK) Telemetry Packets", f"{hk_count} records"),
                ("Real Camera Photo Chunks (Flash Part 2)", f"{real_cam_count}/69 chunks received"),
                ("Dummy Camera Test Chunks (M4 Memory)", f"{dummy_cam_count}/69 chunks received"),
                ("Raw Flash Storage Packets", f"{flash_count} chunks received"),
                ("Total RF Packets Received", f"{state.get('packets_rx', 0) + hk_count + real_cam_count + dummy_cam_count}"),
            ]
            for r_idx, (k, v) in enumerate(stats, start=13):
                c_k = ws1.cell(row=r_idx, column=1, value=k)
                c_k.font = Font(name="Segoe UI", size=10, bold=True, color="334155")
                c_k.fill = FILL_ZEBRA
                c_k.border = BORDER_THIN
                ws1.merge_cells(start_row=r_idx, start_column=2, end_row=r_idx, end_column=4)
                c_v = ws1.cell(row=r_idx, column=2, value=v)
                c_v.font = FONT_TD
                c_v.border = BORDER_THIN
                for col_i in range(2, 5):
                    ws1.cell(row=r_idx, column=col_i).border = BORDER_THIN

            ws1["A20"] = "Data Topics & Table Directory"
            ws1["A20"].font = FONT_SECTION
            topics = [
                ("Sheet: Housekeeping (HK)", "Voltages (Bat, Solar, Bus), Temps (Ant, Bat, BPB), Currents (Unreg, 3V3, 5V, Bat), Gyro & Mag IMU"),
                ("Sheet: Real Camera Photo", "JPEG Photo chunks downlinked from CubeSat camera stored in flash partition 2"),
                ("Sheet: Dummy Camera Test", "Pre-stored dummy camera chunks downlinked directly from OBC M4 memory (camera_dummy_image.h)"),
                ("Sheet: Flash Storage Data", "Raw 128-byte flash storage memory chunks from /dev/hk and /dev/camera"),
            ]
            for r_idx, (k, v) in enumerate(topics, start=21):
                c_k = ws1.cell(row=r_idx, column=1, value=k)
                c_k.font = Font(name="Segoe UI", size=10, bold=True, color="003893")
                c_k.fill = FILL_ZEBRA
                c_k.border = BORDER_THIN
                ws1.merge_cells(start_row=r_idx, start_column=2, end_row=r_idx, end_column=5)
                c_v = ws1.cell(row=r_idx, column=2, value=v)
                c_v.font = FONT_TD
                c_v.border = BORDER_THIN
                for col_i in range(2, 6):
                    ws1.cell(row=r_idx, column=col_i).border = BORDER_THIN

            auto_fit_columns(ws1, 5)

            # -----------------------------------------------------------------
            # SHEET 2: HOUSEKEEPING TELEMETRY (HK)
            # -----------------------------------------------------------------
            ws2 = wb.create_sheet(title="Housekeeping (HK)")
            ws2.views.sheetView[0].showGridLines = True

            ws2.merge_cells("A1:U1")
            t2 = ws2["A1"]
            t2.value = "  Housekeeping (HK) Telemetry Acquisition Table (ADC1, ADC2, Gyro & Magnetometer)"
            t2.font = Font(name="Segoe UI", size=13, bold=True, color="FFFFFF")
            t2.fill = FILL_HEADER_BLUE
            t2.alignment = Alignment(horizontal="left", vertical="center")
            ws2.row_dimensions[1].height = 30

            hk_cols = [
                "Rec #", "Timestamp", "Seq #", "RSSI (dBm)",
                "Bat Volt (V)", "Solar Volt (V)", "Bus Volt (V)",
                "Ant Temp (°C)", "Bat Temp (°C)", "BPB Temp (°C)",
                "Unreg Curr (A)", "3.3V Curr (A)", "5V Curr (A)", "Bat Curr (A)",
                "Gyro X (c-dps)", "Gyro Y (c-dps)", "Gyro Z (c-dps)",
                "Mag X (uT)", "Mag Y (uT)", "Mag Z (uT)", "FCS / CRC"
            ]
            for c_idx, col_name in enumerate(hk_cols, start=1):
                cell = ws2.cell(row=3, column=c_idx, value=col_name)
                cell.font = FONT_TH
                cell.fill = FILL_HEADER_BLUE
                cell.alignment = ALIGN_CENTER
                cell.border = BORDER_THIN
            ws2.row_dimensions[3].height = 24

            hk_list = state.get("hk_history", [])
            if hk_list:
                for row_idx, h in enumerate(hk_list, start=4):
                    fill = FILL_ZEBRA if (row_idx % 2 == 0) else FILL_WHITE
                    row_vals = [
                        row_idx - 3,
                        h.get("timestamp", "-"),
                        h.get("seq", 0),
                        h.get("rssi", -90),
                        h.get("vbat", 0.0),
                        h.get("vsol", 0.0),
                        h.get("vbus", 0.0),
                        h.get("t_ant", 0.0),
                        h.get("t_bat", 0.0),
                        h.get("t_bpb", 0.0),
                        h.get("i_unreg", 0.0),
                        h.get("i_3v3", 0.0),
                        h.get("i_5v", 0.0),
                        h.get("i_bat", 0.0),
                        h.get("gx", 0),
                        h.get("gy", 0),
                        h.get("gz", 0),
                        h.get("mx", 0),
                        h.get("my", 0),
                        h.get("mz", 0),
                        h.get("crc", "VALID")
                    ]
                    for col_idx, val in enumerate(row_vals, start=1):
                        c = ws2.cell(row=row_idx, column=col_idx, value=val)
                        c.font = FONT_TD
                        c.fill = fill
                        c.border = BORDER_THIN
                        c.alignment = ALIGN_CENTER
            else:
                ws2.merge_cells("A4:U4")
                ph = ws2["A4"]
                ph.value = "No live HK packets recorded yet. Downlink live telemetry by clicking 'Request Live HK Telemetry' on Uplink tab."
                ph.font = FONT_SUBTITLE
                ph.alignment = ALIGN_CENTER
                for col_i in range(1, 22):
                    ws2.cell(row=4, column=col_i).border = BORDER_THIN

            auto_fit_columns(ws2, len(hk_cols))

            # -----------------------------------------------------------------
            # SHEET 3: REAL CAMERA PHOTO DATA
            # -----------------------------------------------------------------
            ws3 = wb.create_sheet(title="Real Camera Photo")
            ws3.views.sheetView[0].showGridLines = True

            ws3.merge_cells("A1:K1")
            t3 = ws3["A1"]
            t3.value = "  Real Camera Photo Downlink Chunks (Stored in Flash Partition 2 - JPEG OV2640 / ArduCam)"
            t3.font = Font(name="Segoe UI", size=13, bold=True, color="FFFFFF")
            t3.fill = FILL_HEADER_BLUE
            t3.alignment = Alignment(horizontal="left", vertical="center")
            ws3.row_dimensions[1].height = 30

            cam_cols = [
                "Chunk #", "Total Chunks", "Flash Offset (Hex)", "Offset (Dec)",
                "Length (Bytes)", "SOI (0xFFD8)", "EOI (0xFFD9)", "Magic",
                "Hex Preview (First 16B)", "Timestamp", "Source"
            ]
            for c_idx, col_name in enumerate(cam_cols, start=1):
                cell = ws3.cell(row=3, column=c_idx, value=col_name)
                cell.font = FONT_TH
                cell.fill = FILL_HEADER_BLUE
                cell.alignment = ALIGN_CENTER
                cell.border = BORDER_THIN
            ws3.row_dimensions[3].height = 24

            real_cam_map = state.get("real_cam_records", {})
            if real_cam_map:
                sorted_chunks = sorted(real_cam_map.items(), key=lambda x: x[0])
                for row_idx, (c_idx, c) in enumerate(sorted_chunks, start=4):
                    fill = FILL_ZEBRA if (row_idx % 2 == 0) else FILL_WHITE
                    row_vals = [
                        c.get("pkt_idx", c_idx + 1),
                        c.get("total_p", 69),
                        c.get("offset_hex", f"0x{c_idx*128:04X}"),
                        c.get("offset_dec", c_idx * 128),
                        c.get("length", 128),
                        c.get("soi", "-"),
                        c.get("eoi", "-"),
                        "0xCAFE",
                        c.get("hex_preview", ""),
                        c.get("timestamp", "-"),
                        c.get("source", "RX")
                    ]
                    for col_idx, val in enumerate(row_vals, start=1):
                        cell = ws3.cell(row=row_idx, column=col_idx, value=val)
                        cell.font = FONT_TD_MONO if col_idx in [3, 9] else FONT_TD
                        cell.fill = fill
                        cell.border = BORDER_THIN
                        cell.alignment = ALIGN_LEFT if col_idx == 9 else ALIGN_CENTER
            else:
                ws3.merge_cells("A4:K4")
                ph = ws3["A4"]
                ph.value = "No Real Camera chunks received yet. Trigger camera capture or download using 'Downlink Camera Photo Packets'."
                ph.font = FONT_SUBTITLE
                ph.alignment = ALIGN_CENTER
                for col_i in range(1, 12):
                    ws3.cell(row=4, column=col_i).border = BORDER_THIN

            auto_fit_columns(ws3, len(cam_cols))

            # -----------------------------------------------------------------
            # SHEET 4: DUMMY CAMERA TEST DATA
            # -----------------------------------------------------------------
            ws4 = wb.create_sheet(title="Dummy Camera Test")
            ws4.views.sheetView[0].showGridLines = True

            ws4.merge_cells("A1:K1")
            t4 = ws4["A1"]
            t4.value = "  Dummy Camera Test Chunks (Streamed directly from M4 Memory camera_dummy_image.h - 69 Chunks x 128B = 8774B)"
            t4.font = Font(name="Segoe UI", size=13, bold=True, color="FFFFFF")
            t4.fill = FILL_HEADER_BLUE
            t4.alignment = Alignment(horizontal="left", vertical="center")
            ws4.row_dimensions[1].height = 30

            for c_idx, col_name in enumerate(cam_cols, start=1):
                cell = ws4.cell(row=3, column=c_idx, value=col_name)
                cell.font = FONT_TH
                cell.fill = FILL_HEADER_BLUE
                cell.alignment = ALIGN_CENTER
                cell.border = BORDER_THIN
            ws4.row_dimensions[3].height = 24

            dummy_cam_map = state.get("dummy_cam_records", {})
            if dummy_cam_map:
                sorted_chunks = sorted(dummy_cam_map.items(), key=lambda x: x[0])
                for row_idx, (c_idx, c) in enumerate(sorted_chunks, start=4):
                    fill = FILL_ZEBRA if (row_idx % 2 == 0) else FILL_WHITE
                    row_vals = [
                        c.get("pkt_idx", c_idx + 1),
                        c.get("total_p", 69),
                        c.get("offset_hex", f"0x{c_idx*128:04X}"),
                        c.get("offset_dec", c_idx * 128),
                        c.get("length", 128),
                        c.get("soi", "-"),
                        c.get("eoi", "-"),
                        "0xCAFE",
                        c.get("hex_preview", ""),
                        c.get("timestamp", "-"),
                        c.get("source", "RX")
                    ]
                    for col_idx, val in enumerate(row_vals, start=1):
                        cell = ws4.cell(row=row_idx, column=col_idx, value=val)
                        cell.font = FONT_TD_MONO if col_idx in [3, 9] else FONT_TD
                        cell.fill = fill
                        cell.border = BORDER_THIN
                        cell.alignment = ALIGN_LEFT if col_idx == 9 else ALIGN_CENTER
            else:
                ws4.merge_cells("A4:K4")
                ph = ws4["A4"]
                ph.value = "No Dummy Camera test chunks received yet. Send 'Dummy Camera Test Command' on Uplink tab."
                ph.font = FONT_SUBTITLE
                ph.alignment = ALIGN_CENTER
                for col_i in range(1, 12):
                    ws4.cell(row=4, column=col_i).border = BORDER_THIN

            auto_fit_columns(ws4, len(cam_cols))

            # -----------------------------------------------------------------
            # SHEET 5: FLASH STORAGE DATA
            # -----------------------------------------------------------------
            ws5 = wb.create_sheet(title="Flash Storage Data")
            ws5.views.sheetView[0].showGridLines = True

            ws5.merge_cells("A1:J1")
            t5 = ws5["A1"]
            t5.value = "  Raw Flash Storage Memory Chunks (/dev/hk & /dev/camera Partitions)"
            t5.font = Font(name="Segoe UI", size=13, bold=True, color="FFFFFF")
            t5.fill = FILL_HEADER_BLUE
            t5.alignment = Alignment(horizontal="left", vertical="center")
            ws5.row_dimensions[1].height = 30

            flash_cols = [
                "Chunk #", "Total Chunks", "Flash Addr (Hex)", "Flash Addr (Dec)",
                "Length (Bytes)", "Footer 1 (0xAA55)", "Footer 2 (0xBB66)",
                "End Footer (0xAACC)", "Status", "Timestamp"
            ]
            for c_idx, col_name in enumerate(flash_cols, start=1):
                cell = ws5.cell(row=3, column=c_idx, value=col_name)
                cell.font = FONT_TH
                cell.fill = FILL_HEADER_BLUE
                cell.alignment = ALIGN_CENTER
                cell.border = BORDER_THIN
            ws5.row_dimensions[3].height = 24

            flash_list = state.get("flash_history", [])
            if flash_list:
                for row_idx, f in enumerate(flash_list, start=4):
                    fill = FILL_ZEBRA if (row_idx % 2 == 0) else FILL_WHITE
                    row_vals = [
                        f.get("pkt", row_idx - 3),
                        f.get("total", 1),
                        f.get("addr_hex", "0x00000000"),
                        f.get("addr_dec", 0),
                        f.get("len", 128),
                        f.get("f1", "0xAA55"),
                        f.get("f2", "0xBB66"),
                        f.get("f_end", "0xAACC"),
                        f.get("status", 0),
                        f.get("timestamp", "-")
                    ]
                    for col_idx, val in enumerate(row_vals, start=1):
                        cell = ws5.cell(row=row_idx, column=col_idx, value=val)
                        cell.font = FONT_TD_MONO if col_idx == 3 else FONT_TD
                        cell.fill = fill
                        cell.border = BORDER_THIN
                        cell.alignment = ALIGN_CENTER
            else:
                ws5.merge_cells("A4:J4")
                ph = ws5["A4"]
                ph.value = "No raw flash storage chunks received yet. Download flash data using 'Request Flash Storage Telemetry'."
                ph.font = FONT_SUBTITLE
                ph.alignment = ALIGN_CENTER
                for col_i in range(1, 11):
                    ws5.cell(row=4, column=col_i).border = BORDER_THIN

            auto_fit_columns(ws5, len(flash_cols))

            # Save Workbook
            wb.save(out_path)
            if not silent:
                messagebox.showinfo(
                    "Excel Export Successful",
                    f"All received HK telemetry, camera photos, and flash data were exported to:\n\n{out_path}")
            print(f"[EXCEL] Successfully exported telemetry data to: {out_path}")
            return out_path
        except Exception as e:
            print(f"[ERROR] Failed exporting to Excel: {e}")
            if not silent:
                messagebox.showerror("Export Error", f"Failed saving Excel file:\n{e}")
            return None

    # =========================================================================
    # TOP HEADER WITH APN LOGO, NEPAL FLAG & BRANDING
    # =========================================================================
    header_frame = tk.Frame(root, bg="#FFFFFF", height=88, relief="ridge", bd=1)
    header_frame.pack(fill=tk.X, side=tk.TOP)

    # 1. APN Logo Container
    logo_container = tk.Frame(header_frame, bg="#FFFFFF")
    logo_container.pack(side=tk.LEFT, padx=(12, 4), pady=6)

    logo_img = None
    if os.path.exists(LOGO_PATH) and HAS_PIL:
        try:
            pil_logo = Image.open(LOGO_PATH)
            w, h = pil_logo.size
            target_h = 66
            target_w = int(w * (target_h / h))
            pil_logo = pil_logo.resize((target_w, target_h), Image.Resampling.LANCZOS)
            logo_img = ImageTk.PhotoImage(pil_logo)
            logo_lbl = tk.Label(logo_container, image=logo_img, bg="#FFFFFF")
            logo_lbl.pack(side=tk.LEFT)
            header_frame.logo_img = logo_img
        except Exception as ex:
            print("[WARN] Could not load logo image:", ex)
            logo_img = None

    # 2. Nepal Flag Container
    flag_container = tk.Frame(header_frame, bg="#FFFFFF")
    flag_container.pack(side=tk.LEFT, padx=(4, 12), pady=6)

    flag_img = None
    flag_file_to_use = FLAG_PATH if os.path.exists(FLAG_PATH) else (
        FLAG_HEADER_PATH if os.path.exists(FLAG_HEADER_PATH) else None)
    if flag_file_to_use and HAS_PIL:
        try:
            pil_flag = Image.open(flag_file_to_use)
            w, h = pil_flag.size
            target_h = 66
            target_w = int(w * (target_h / h))
            pil_flag = pil_flag.resize((target_w, target_h), Image.Resampling.LANCZOS)
            flag_img = ImageTk.PhotoImage(pil_flag)
            flag_lbl = tk.Label(flag_container, image=flag_img, bg="#FFFFFF")
            flag_lbl.pack(side=tk.LEFT)
            header_frame.flag_img = flag_img
        except Exception as ex:
            print("[WARN] Could not load flag image with PIL:", ex)
            flag_img = None

    if flag_img is None and flag_file_to_use:
        try:
            flag_img = tk.PhotoImage(file=flag_file_to_use)
            flag_lbl = tk.Label(flag_container, image=flag_img, bg="#FFFFFF")
            flag_lbl.pack(side=tk.LEFT)
            header_frame.flag_img = flag_img
        except Exception:
            flag_img = None

    if flag_img is None and logo_img is None:
        flag_cv = tk.Canvas(flag_container, width=54, height=66, bg="#FFFFFF",
                            highlightthickness=0)
        flag_cv.pack(side=tk.LEFT)
        blue = PRIMARY_BLUE
        crimson = CRIMSON
        flag_cv.create_polygon(4, 4, 48, 32, 20, 32, 52, 64, 4, 64,
                               fill=blue, outline=blue)
        flag_cv.create_polygon(7, 8, 43, 30, 18, 30, 46, 61, 7, 61,
                               fill=crimson, outline=crimson)
        flag_cv.create_oval(14, 18, 26, 26, fill="white", outline="white")
        flag_cv.create_oval(14, 15, 26, 23, fill=crimson, outline=crimson)
        flag_cv.create_oval(15, 44, 25, 54, fill="white", outline="white")

    # 3. Organization Title Box with Correct Nepali & English Names
    org_box = tk.Frame(header_frame, bg="#FFFFFF")
    org_box.pack(side=tk.LEFT, padx=6, pady=4)
    ne_img = render_devanagari(ORG_NE, 22, PRIMARY_BLUE)
    if ne_img is not None:
        header_frame.ne_img = ne_img
        tk.Label(org_box, image=ne_img, bg="#FFFFFF").pack(anchor="w")
    else:
        tk.Label(org_box, text=f"{ORG_NE}", bg="#FFFFFF", fg=PRIMARY_BLUE,
                 font=("Lohit Devanagari", 14, "bold")).pack(anchor="w")
    tk.Label(org_box, text=f"{ORG_EN}", bg="#FFFFFF", fg=CRIMSON,
             font=("Segoe UI", 11, "bold")).pack(anchor="w")
    tk.Label(org_box, text=f"{APP_TITLE}  •  Flight Operations Ground Console",
             bg="#FFFFFF", fg=TEXT_MUTED, font=("Segoe UI", 9)).pack(anchor="w")

    # 4. Right Badge Box with RF Info & One-Click Excel Export Button
    badge_box = tk.Frame(header_frame, bg="#FFFFFF")
    badge_box.pack(side=tk.RIGHT, padx=14, pady=4)

    excel_top_box = tk.Frame(badge_box, bg="#FFFFFF")
    excel_top_box.pack(side=tk.RIGHT, padx=(12, 0))

    ttk.Button(excel_top_box, text="📊 Export Excel (.xlsx)",
               command=lambda: export_all_to_excel(silent=False),
               style="Success.TButton").pack(side=tk.TOP, pady=2)

    ttk.Checkbutton(excel_top_box, text="Auto-Save on RX",
                    variable=auto_save_excel_var).pack(side=tk.TOP, pady=1)

    rf_info = (f"RX: {RX_FREQ_STR}  |  TX: {TX_FREQ_STR}\n"
               f"Callsign: {SAT_CALLSIGN}  |  Modulation: 4800 bps GMSK")
    tk.Label(badge_box, text=rf_info, bg="#FFFFFF", fg=TEXT_MAIN,
             justify=tk.RIGHT, font=("Monospace", 9, "bold")).pack(anchor="e", padx=6)

    accent_bar = tk.Frame(root, bg=CRIMSON, height=3)
    accent_bar.pack(fill=tk.X)
    accent_blue = tk.Frame(root, bg=PRIMARY_BLUE, height=2)
    accent_blue.pack(fill=tk.X)

    # =========================================================================
    # SERIAL CONNECTION BAR
    # =========================================================================
    conn_bar = tk.Frame(root, bg="#FFFFFF", padx=12, pady=6, relief="groove", bd=1)
    conn_bar.pack(fill=tk.X, padx=8, pady=(6, 2))

    ttk.Label(conn_bar, text="Serial Port:").pack(side=tk.LEFT, padx=4)
    port_var = tk.StringVar()
    port_combo = ttk.Combobox(conn_bar, textvariable=port_var, width=16)
    port_combo.pack(side=tk.LEFT, padx=4)

    def refresh_ports():
        all_p = [p.device for p in serial.tools.list_ports.comports()]
        # Prioritize real USB/ACM serial devices over PC motherboard /dev/ttyS* ports
        usb_ports = [p for p in all_p if "ttyUSB" in p or "ttyACM" in p]
        if not usb_ports:
            usb_ports = glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
        other_ports = [p for p in all_p if p not in usb_ports and not p.startswith("/dev/ttyS")]
        ports = usb_ports + other_ports
        if not ports:
            ports = glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*") + ["/dev/ttyUSB0", "/dev/ttyACM0"]
        # Remove duplicates while preserving order
        seen = set()
        dedup_ports = [x for x in ports if not (x in seen or seen.add(x))]
        port_combo["values"] = dedup_ports
        if default_port and default_port in dedup_ports:
            port_var.set(default_port)
        elif usb_ports and usb_ports[0] in dedup_ports:
            port_var.set(usb_ports[0])
        elif port_var.get() not in dedup_ports and dedup_ports:
            port_var.set(dedup_ports[0])

    refresh_ports()
    ttk.Button(conn_bar, text="⟳ Refresh", command=refresh_ports,
               width=9).pack(side=tk.LEFT, padx=4)

    ttk.Label(conn_bar, text="Baud:").pack(side=tk.LEFT, padx=(12, 4))
    baud_var = tk.StringVar(value=str(baudrate))
    ttk.Combobox(conn_bar, textvariable=baud_var, width=8,
                 values=["9600", "57600", "115200", "230400"]).pack(side=tk.LEFT)

    btn_connect = ttk.Button(conn_bar, text="Connect",
                             style="Primary.TButton", width=12)
    btn_connect.pack(side=tk.LEFT, padx=(12, 6))

    style.configure("StatusDisconnected.TLabel", background="#FFFFFF",
                    foreground=CRIMSON, font=("Segoe UI", 10, "bold"))
    style.configure("StatusConnected.TLabel", background="#FFFFFF",
                    foreground=SUCCESS_GREEN, font=("Segoe UI", 10, "bold"))

    status_var = tk.StringVar(value="● DISCONNECTED")
    status_lbl = ttk.Label(conn_bar, textvariable=status_var,
                           style="StatusDisconnected.TLabel")
    status_lbl.pack(side=tk.LEFT, padx=10)

    quick_link_var = tk.StringVar(value="RF: STANDBY | Packets: 0 | ACKs: 0")
    tk.Label(conn_bar, textvariable=quick_link_var, bg="#F1F5F9", fg=PRIMARY_BLUE,
             font=("Monospace", 9, "bold"), padx=10, pady=4,
             relief="solid", bd=1).pack(side=tk.RIGHT, padx=4)

    # =========================================================================
    # LIVE CW DECODE (always at top, all tabs)
    # =========================================================================
    cw_frame = tk.Frame(root, bg="#0F172A", relief="solid", bd=1, padx=10, pady=8)
    cw_frame.pack(fill=tk.X, padx=8, pady=(6, 4))

    cw_row1 = tk.Frame(cw_frame, bg="#0F172A")
    cw_row1.pack(fill=tk.X)
    tk.Label(cw_row1, text="CW DECODE  435.000 MHz", bg="#0F172A", fg="#38BDF8",
             font=("Segoe UI", 10, "bold")).pack(side=tk.LEFT, padx=(0, 12))
    cw_state_var = tk.StringVar(value="IDLE")
    cw_state_lbl = tk.Label(cw_row1, textvariable=cw_state_var, bg="#334155",
                            fg="#FFFFFF", font=("Segoe UI", 10, "bold"),
                            padx=10, pady=2)
    cw_state_lbl.pack(side=tk.LEFT, padx=4)
    cw_rssi_var = tk.StringVar(value="RSSI: -- dBm")
    tk.Label(cw_row1, textvariable=cw_rssi_var, bg="#0F172A", fg="#FBBF24",
             font=("Courier", 12, "bold")).pack(side=tk.LEFT, padx=12)

    cw_row2 = tk.Frame(cw_frame, bg="#0F172A")
    cw_row2.pack(fill=tk.X, pady=(6, 0))
    tk.Label(cw_row2, text="LIVE", bg="#0F172A", fg="#64748B",
             font=("Segoe UI", 8, "bold")).pack(side=tk.LEFT, padx=(0, 8))
    cw_live_var = tk.StringVar(value="(waiting for Morse)")
    tk.Label(cw_row2, textvariable=cw_live_var, bg="#0F172A", fg="#4ADE80",
             font=("Courier", 18, "bold"), anchor="w").pack(side=tk.LEFT, fill=tk.X, expand=True)

    cw_row3 = tk.Frame(cw_frame, bg="#0F172A")
    cw_row3.pack(fill=tk.X, pady=(2, 0))
    tk.Label(cw_row3, text="LAST", bg="#0F172A", fg="#64748B",
             font=("Segoe UI", 8, "bold")).pack(side=tk.LEFT, padx=(0, 8))
    cw_last_var = tk.StringVar(value="—")
    tk.Label(cw_row3, textvariable=cw_last_var, bg="#0F172A", fg="#E2E8F0",
             font=("Courier", 14, "bold"), anchor="w").pack(side=tk.LEFT, fill=tk.X, expand=True)

    def update_cw_panel(state_txt=None, rssi=None, live=None, last=None, color=None):
        if state_txt is not None:
            cw_state_var.set(state_txt)
            bg = color or ("#15803D" if state_txt in ("CARRIER", "DECODED") else
                           "#B45309" if state_txt == "KEYING" else "#334155")
            cw_state_lbl.config(bg=bg, fg="#FFFFFF")
        if rssi is not None:
            cw_rssi_var.set(f"RSSI: {rssi} dBm")
        if live is not None:
            cw_live_var.set(live if live else "(waiting for Morse)")
        if last is not None:
            cw_last_var.set(last)

    # =========================================================================
    # DEDICATED ACK / RESPONSE BANNER
    # =========================================================================
    ack_frame = tk.Frame(root, bg="#FFFFFF", relief="solid", bd=1, padx=10, pady=6)
    ack_frame.pack(fill=tk.X, padx=8, pady=(0, 6))

    tk.Label(ack_frame, text="SATELLITE ACK STATUS:", bg="#FFFFFF", fg=PRIMARY_BLUE,
             font=("Segoe UI", 10, "bold")).pack(side=tk.LEFT, padx=(4, 8))

    style.configure("Standby.TLabel", background="#F1F5F9", foreground=TEXT_MUTED,
                    font=("Segoe UI", 10, "bold"), padding=4)
    style.configure("Awaiting.TLabel", background="#FEF3C7", foreground="#B45309",
                    font=("Segoe UI", 10, "bold"), padding=4)
    style.configure("Success.TLabel", background="#DCFCE7", foreground="#15803D",
                    font=("Segoe UI", 10, "bold"), padding=4)
    style.configure("Danger.TLabel", background="#FEE2E2", foreground="#B91C1C",
                    font=("Segoe UI", 10, "bold"), padding=4)

    ack_badge_var = tk.StringVar(value="STANDBY - NO COMMAND PENDING")
    ack_badge_lbl = ttk.Label(ack_frame, textvariable=ack_badge_var,
                              style="Standby.TLabel")
    ack_badge_lbl.pack(side=tk.LEFT, padx=4)

    ack_details_var = tk.StringVar(value="Ready to transmit commands to satellite.")
    tk.Label(ack_frame, textvariable=ack_details_var, bg="#FFFFFF", fg=TEXT_MAIN,
             font=("Segoe UI", 9)).pack(side=tk.LEFT, padx=12)

    ack_clear_after_id = {"id": None}

    def reset_ack_banner():
        ack_badge_var.set("STANDBY - NO COMMAND PENDING")
        ack_badge_lbl.config(style="Standby.TLabel")
        ack_details_var.set("Ready to transmit commands to satellite.")

    def schedule_ack_clear():
        if ack_clear_after_id["id"] is not None:
            try:
                root.after_cancel(ack_clear_after_id["id"])
            except Exception:
                pass
        ack_clear_after_id["id"] = root.after(
            ACK_AUTO_CLEAR_SEC * 1000, reset_ack_banner)

    def set_ack_state(kind, title, details):
        if kind == "ok":
            ack_badge_var.set(f"✔ ACK RECEIVED [{time.strftime('%H:%M:%S')}]")
            ack_badge_lbl.config(style="Success.TLabel")
        elif kind == "await":
            ack_badge_var.set("⏳ AWAITING RESPONSE...")
            ack_badge_lbl.config(style="Awaiting.TLabel")
        else:
            ack_badge_var.set(f"✖ NACK / TIMEOUT [{time.strftime('%H:%M:%S')}]")
            ack_badge_lbl.config(style="Danger.TLabel")
        ack_details_var.set(f"{title}: {details}")
        quick_link_var.set(
            f"RF: ACTIVE | Packets: {state['packets_rx']} | ACKs: {state['acks_rx']}")
        if kind in ("ok", "error"):
            schedule_ack_clear()

    def set_ack_received(title, details, is_ok=True):
        state["acks_rx"] += 1
        set_ack_state("ok" if is_ok else "error", title, details)

    # =========================================================================
    # MAIN NOTEBOOK: 3 FOCUSED TABS
    # =========================================================================
    nb = ttk.Notebook(root)
    nb.pack(fill=tk.BOTH, expand=True, padx=8, pady=4)

    tab_cmd = ttk.Frame(nb)
    tab_downlink = ttk.Frame(nb)
    tab_camera = ttk.Frame(nb)
    tab_log = ttk.Frame(nb)

    nb.add(tab_cmd, text="  🕹️ GMSK Uplink Commands  ")
    nb.add(tab_downlink, text="  📡 Downlink HK & Flash Data  ")
    nb.add(tab_camera, text="  📷 Live Camera Photo  ")
    nb.add(tab_log, text="  📜 Raw Serial Terminal  ")

    # =========================================================================
    # TAB 1: GMSK UPLINK TELECOMMANDS
    # =========================================================================
    cmd_paned = ttk.PanedWindow(tab_cmd, orient=tk.HORIZONTAL)
    cmd_paned.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

    cmd_left = ttk.Frame(cmd_paned)
    cmd_paned.add(cmd_left, weight=3)

    cmd_right = ttk.Frame(cmd_paned)
    cmd_paned.add(cmd_right, weight=2)

    raw_log = None
    autoscroll_var = None

    def append_log(text, tag=None):
        if raw_log is not None:
            try:
                raw_log.insert(tk.END, text, tag)
                if autoscroll_var and autoscroll_var.get():
                    raw_log.see(tk.END)
            except Exception:
                pass
        else:
            print(text, end="")

    def send_text(text):
        if ser and ser.is_open:
            try:
                ser.write((text.strip() + "\r\n").encode("utf-8"))
                ser.flush()
                append_log(f">>> [UPLINK-TX] {text}\n", "tx")
                cmd_hist_list.insert(0, f"{time.strftime('%H:%M:%S')}  {text}")
                if cmd_hist_list.size() > 200:
                    cmd_hist_list.delete(200, tk.END)
            except Exception as e:
                append_log(f"!!! [TX ERROR] {e}\n", "error")
        else:
            append_log("!!! [ERROR] Serial port not connected! Connect first.\n",
                       "error")

    def send_hex_bytes(b):
        send_text("HEX " + format_hex(b))

    # --- Flash / Telemetry / Mission Download Telecommand Generator ---
    flash_cmd_box = ttk.LabelFrame(
        cmd_left, text=" 💾 Mission & Flash Download Telecommand Generator (128 Bytes / Packet) ")
    flash_cmd_box.pack(fill=tk.X, padx=6, pady=6)

    f_desc = ("Unified 13-Byte Telecommand: 53 [MCU_ID:1B] [OPCODE:3B] [DATA_ID:2B] [ADDR:4B] [PKTS:2B]\n"
              "Select target file/subsystem, enter start flash address and packet count:")
    tk.Label(flash_cmd_box, text=f_desc, bg="#FFFFFF", fg=TEXT_MUTED,
             font=("Segoe UI", 9), justify=tk.LEFT).pack(anchor="w", padx=10, pady=(6, 4))

    target_row = tk.Frame(flash_cmd_box, bg="#FFFFFF")
    target_row.pack(fill=tk.X, padx=10, pady=2)
    tk.Label(target_row, text="Target Subsystem / File:", bg="#FFFFFF", fg=TEXT_MAIN,
             font=("Segoe UI", 9, "bold")).pack(side=tk.LEFT, padx=4)

    target_options = {
        "📊 sat_health.txt (HK Telemetry) - Opcode 1D D1 F2": (MCU_ID_OBC, OPCODE_HK, 0x0000),
        "💾 raw_flash.bin (Flash Storage) - Opcode 1D D1 F8": (MCU_ID_OBC, OPCODE_FLASH, 0x0000),
        "📷 cam.txt (Camera Photo Download) - Opcode 1D D2 F5": (MCU_ID_OBC, OPCODE_CAM_DOWN, 0x0000),
    }
    target_var = tk.StringVar(value="📊 sat_health.txt (HK Telemetry) - Opcode 1D D1 F2")
    target_combo = ttk.Combobox(target_row, textvariable=target_var,
                                values=list(target_options.keys()), state="readonly", width=48)
    target_combo.pack(side=tk.LEFT, padx=4)

    f_inputs = tk.Frame(flash_cmd_box, bg="#FFFFFF")
    f_inputs.pack(fill=tk.X, padx=10, pady=4)

    tk.Label(f_inputs, text="Start Address:", bg="#FFFFFF", fg=TEXT_MAIN,
             font=("Segoe UI", 9, "bold")).grid(row=0, column=0, sticky="w", padx=4, pady=4)
    addr_var = tk.StringVar(value="0x00000000")
    addr_entry = ttk.Entry(f_inputs, textvariable=addr_var, width=16,
                           font=("Monospace", 9))
    addr_entry.grid(row=0, column=1, sticky="w", padx=4, pady=4)

    tk.Label(f_inputs, text="Number of Packets (N):", bg="#FFFFFF", fg=TEXT_MAIN,
             font=("Segoe UI", 9, "bold")).grid(row=0, column=2, sticky="w",
                                                padx=(16, 4), pady=4)
    pkts_var = tk.StringVar(value="5")
    pkts_entry = ttk.Entry(f_inputs, textvariable=pkts_var, width=12,
                           font=("Monospace", 9))
    pkts_entry.grid(row=0, column=3, sticky="w", padx=4, pady=4)

    calc_info_var = tk.StringVar(value="Size: 5 Packets × 128 B = 640 Bytes | "
                                       "Range: 0x00000000 - 0x00000280")
    tk.Label(flash_cmd_box, textvariable=calc_info_var, bg="#FFFFFF", fg=ACTION_BLUE,
             font=("Segoe UI", 9, "bold")).pack(anchor="w", padx=10, pady=2)

    hex_preview_var = tk.StringVar(value="HEX: 53 01 1D D1 F2 00 00 00 00 00 00 00 05")
    tk.Label(flash_cmd_box, textvariable=hex_preview_var, bg="#F8FAFC",
             fg=PRIMARY_BLUE, font=("Monospace", 10, "bold"), relief="solid",
             bd=1, padx=8, pady=4).pack(fill=tk.X, padx=10, pady=4)

    def update_flash_cmd_preview(*args):
        try:
            a_str = addr_var.get().strip()
            addr = int(a_str, 16) if (a_str.startswith("0x") or a_str.startswith("0X")) \
                else int(a_str)
            pkts = int(pkts_var.get().strip())
            if pkts < 0:
                pkts = 0
            if pkts > 65535:
                pkts = 65535
            total_bytes = pkts * 128 if pkts > 0 else 128
            end_addr = addr + (pkts * 128 if pkts > 0 else 128)
            pkt_desc = f"{pkts} Packets" if pkts > 0 else "All Packets (to EOF)"
            calc_info_var.set(
                f"Size: {pkt_desc} × 128 B ≈ {total_bytes} Bytes "
                f"({total_bytes/1024:.2f} KB) | "
                f"Range: 0x{addr:08X} - 0x{end_addr:08X}")

            t_val = target_var.get()
            mcu_id, opcode, data_id = target_options.get(
                t_val, (MCU_ID_OBC, OPCODE_HK, 0x0000))
            hex_str, _ = build_telecommand_hex(
                mcu_id, opcode, data_id, addr, pkts)
            hex_preview_var.set(f"HEX: {hex_str}")
        except Exception:
            calc_info_var.set("Invalid Address or Packet count")
            hex_preview_var.set("HEX: (invalid)")

    target_var.trace_add("write", update_flash_cmd_preview)
    addr_var.trace_add("write", update_flash_cmd_preview)
    pkts_var.trace_add("write", update_flash_cmd_preview)
    update_flash_cmd_preview()

    def on_send_flash_cmd():
        try:
            a_str = addr_var.get().strip()
            addr = int(a_str, 16) if (a_str.startswith("0x") or a_str.startswith("0X")) \
                else int(a_str)
            pkts = int(pkts_var.get().strip())
            if pkts < 0:
                pkts = 0
            if pkts > 65535:
                pkts = 65535

            t_val = target_var.get()
            mcu_id, opcode, data_id = target_options.get(
                t_val, (MCU_ID_OBC, OPCODE_HK, 0x0000))

            if "cam" in t_val.lower():
                state["current_mission_type"] = "REAL_CAMERA"
                state["cam_bytes"] = bytearray()
            elif "sat_health" in t_val.lower() or "hk" in t_val.lower():
                state["current_mission_type"] = "HK"
            else:
                state["current_mission_type"] = "FLASH"

            _, raw = build_telecommand_hex(
                mcu_id, opcode, data_id, addr, pkts)
            send_hex_bytes(raw)

            state["flash_bytes_expected"] = pkts * 128 if pkts > 0 else 69 * 128
            state["flash_pkt_total"] = pkts if pkts > 0 else 69
            set_ack_state("await", "Mission Download",
                          f"Requested {pkts} packets (Start 0x{addr:08X})")
        except Exception as e:
            messagebox.showerror("Command Error",
                                 f"Failed to build download command: {e}")

    f_btn_row = tk.Frame(flash_cmd_box, bg="#FFFFFF")
    f_btn_row.pack(fill=tk.X, padx=10, pady=(4, 8))
    ttk.Button(f_btn_row, text="🚀 Transmit Download Command",
               command=on_send_flash_cmd,
               style="Action.TButton").pack(side=tk.LEFT, padx=4)

    for p_preset in [1, 5, 10, 20, 50, 69, 100]:
        ttk.Button(f_btn_row, text=f"{p_preset} Pkts",
               command=lambda p=p_preset: pkts_var.set(str(p)),
               width=7).pack(side=tk.LEFT, padx=2)

    # --- Quick Mission Command Buttons ---
    quick_box = ttk.LabelFrame(
        cmd_left, text=" Pre-Configured Satellite Telecommands (437.375 MHz) ")
    quick_box.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)

    def send_preset_cmd(name, hex_data, desc):
        if "dummy" in name.lower():
            state["current_mission_type"] = "DUMMY_CAMERA"
        elif "cam" in name.lower() or "photo" in name.lower():
            state["current_mission_type"] = "REAL_CAMERA"
        elif "hk" in name.lower() or "health" in name.lower():
            state["current_mission_type"] = "HK"
        elif "flash" in name.lower():
            state["current_mission_type"] = "FLASH"

        if "cam" in name.lower() or "photo" in name.lower():
            try:
                clear_camera_buffer()
                nb.select(tab_camera)
            except Exception:
                state["cam_bytes"] = bytearray()
        n = name.lower()
        if "dummy" in n:
            send_text("DC")
        elif "ping" in n:
            send_text("PING")
        elif "hk" in n or "health" in n:
            send_text("HK1")
        elif "real camera capture" in n:
            send_text("CAMON")
        elif "downlink camera" in n:
            send_text("CAM")
        elif "adcs" in n:
            send_text("ADCS")
        elif "epdm" in n:
            send_text("EPDM")
        else:
            send_hex_bytes(hex_data)
        set_ack_state("await", name, desc)

    # Build preset commands using the shared builder strictly matching Packet Format.xlsx
    hk_hex, hk_raw = build_telecommand_hex(
        MCU_ID_OBC, OPCODE_HK, 0x0000, 0x00000000, 1)
    flash_hex, flash_raw = build_telecommand_hex(
        MCU_ID_OBC, OPCODE_FLASH, 0x0000, 0x00000000, 5)
    ping_hex, ping_raw = build_telecommand_hex(
        MCU_ID_OBC, bytes([0x1D, 0xD1, 0xF5]), 0x0000, 0x00000000, 1)
    cam_dummy_hex, cam_dummy_raw = build_telecommand_hex(
        MCU_ID_CAM, OPCODE_CAM_DUMMY, DATA_ID_DUMMY, 0x00000000, 69)
    cam_on_hex, cam_on_raw = build_telecommand_hex(
        MCU_ID_CAM, OPCODE_CAM_ON, 0x0000, 0x00000000, 0)
    cam_down_hex, cam_down_raw = build_telecommand_hex(
        MCU_ID_OBC, OPCODE_CAM_DOWN, 0x0000, 0x00000000, 69)
    adcs_hex, adcs_raw = build_telecommand_hex(
        MCU_ID_ADCS, OPCODE_ADCS, 0x0000, 0x00000000, 1)
    epdm_hex, epdm_raw = build_telecommand_hex(
        MCU_ID_EPDM, OPCODE_EPDM, 0x0000, 0x00000000, 1)

    preset_cmds = [
        ("📡 Ping Satellite OBC (Ping -> ACK)",
         ping_raw, "Ping command sent (53 01 1D D1 F5 00 00 00 00 00 00 00 01)"),
        ("📊 Request Live HK Telemetry (sat_health.txt)",
         hk_raw, f"HK request sent ({hk_hex})"),
        ("💾 Request Flash Storage Telemetry (5 Chunks @ 128B)",
         flash_raw, f"Flash read sent ({flash_hex})"),
        ("🧪 Send Dummy Camera Test Command (All 69 Chunks - 8.8KB)",
         cam_dummy_raw, f"Dummy Camera Test sent ({cam_dummy_hex} - 69 Chunks)"),
        ("📷 Real Camera Capture (UART2 /dev/ttyS1 Handshake)",
         cam_on_raw, f"Real Camera trigger sent ({cam_on_hex})"),
        ("📥 Downlink Camera Photo Packets (All 69 Chunks @ 128B)",
         cam_down_raw, f"Camera photo download request sent ({cam_down_hex})"),
        ("🧭 ADCS ON: Run ADCS Mission (Opcode A0 53 CF)",
         adcs_raw, f"ADCS Mission trigger sent ({adcs_hex})"),
        ("🔬 EPDM ON: Run EPDM Mission (Opcode EC CF CF)",
         epdm_raw, f"EPDM Mission trigger sent ({epdm_hex})"),
        ("🌊 Request 50-Packet GMSK Burst Stream",
         "BURST", "Requesting 50-packet GMSK burst telemetry downlink"),
        ("📶 Transmit Continuous Test Carrier @ 437.375 MHz (5s - SDR Test)",
         "CARRIER 5",
         "Transmitting test carrier on 437.375 MHz (+22 dBm RFO_HP) for 5 seconds"),
        ("ℹ️ Query Ground Station Radio Status (Counters & Link)",
         None, "Querying GS firmware link counters"),
    ]

    for label, hex_payload, desc in preset_cmds:
        f = tk.Frame(quick_box, bg="#FFFFFF")
        f.pack(fill=tk.X, padx=8, pady=3)
        if isinstance(hex_payload, (bytes, bytearray)):
            btn = ttk.Button(f, text=label,
                             command=lambda l=label, h=hex_payload, d=desc:
                                 send_preset_cmd(l, h, d),
                             style="Primary.TButton")
        elif isinstance(hex_payload, str):
            btn = ttk.Button(f, text=label,
                             command=lambda h=hex_payload: send_text(h),
                             style="Action.TButton")
        else:
            btn = ttk.Button(f, text=label, command=lambda: send_text("S"))
        btn.pack(side=tk.LEFT, fill=tk.X, expand=True)

    # --- Right Column: Custom HEX / ASCII Telecommand Sender & History ---
    custom_box = ttk.LabelFrame(cmd_right, text=" Custom Telecommand / Raw HEX Uplink ")
    custom_box.pack(fill=tk.X, padx=6, pady=6)

    tk.Label(custom_box, text="Enter 13-Byte Telecommand HEX or ASCII String:",
             bg="#FFFFFF", fg=TEXT_MUTED).pack(anchor="w", padx=8, pady=(4, 2))
    cmd_entry = ttk.Entry(custom_box, font=("Monospace", 10))
    cmd_entry.pack(fill=tk.X, padx=8, pady=4)
    cmd_entry.insert(0, ping_hex)

    def on_custom_send_ascii():
        t = cmd_entry.get().strip()
        # If user inadvertently clicks 'Send ASCII Text' on a hex command, route to hex send
        cleaned_up = t.upper()
        if cleaned_up.startswith("HEX ") or cleaned_up.startswith("HHEX ") or (len(t) >= 26 and " " in t and all(c in "0123456789ABCDEFabcdef " for c in t)):
            on_custom_send_hex()
            return
        if t:
            send_text(t)

    def on_custom_send_hex():
        raw = cmd_entry.get().strip()
        while raw.upper().startswith("H"):
            if raw.upper().startswith("HEX "):
                raw = raw[4:].strip()
                break
            elif raw.upper().startswith("HEX"):
                raw = raw[3:].strip()
                break
            raw = raw[1:].strip()
        if raw.upper().startswith("HEX "):
            raw = raw[4:].strip()
        clean = raw.replace(" ", "").replace(",", "").replace("0x", "").replace("0X", "")
        try:
            b = bytes.fromhex(clean)
            send_hex_bytes(b)
        except ValueError:
            messagebox.showerror(
                "Invalid HEX",
                "Enter valid hex bytes, for example:\n"
                "53 01 1D D2 F5 00 00 00 00 00 00 00 01")

    btn_row = tk.Frame(custom_box, bg="#FFFFFF")
    btn_row.pack(fill=tk.X, padx=8, pady=6)
    ttk.Button(btn_row, text="Send Telecommand (HEX)",
               command=on_custom_send_hex,
               style="Action.TButton").pack(side=tk.LEFT, padx=3)
    ttk.Button(btn_row, text="Clear",
               command=lambda: cmd_entry.delete(0, tk.END)).pack(side=tk.LEFT, padx=3)

    cmd_hist_box = ttk.LabelFrame(cmd_right, text=" Uplink Command Transmission History ")
    cmd_hist_box.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)

    cmd_hist_list = tk.Listbox(cmd_hist_box, bg="#FFFFFF", fg=TEXT_MAIN,
                               font=("Courier", 9), relief="solid", bd=1,
                               selectbackground=PRIMARY_BLUE)
    cmd_hist_scroll = ttk.Scrollbar(cmd_hist_box, orient="vertical",
                                    command=cmd_hist_list.yview)
    cmd_hist_list.configure(yscrollcommand=cmd_hist_scroll.set)
    cmd_hist_list.pack(side=tk.LEFT, fill=tk.BOTH, expand=True,
                       padx=(4, 0), pady=4)
    cmd_hist_scroll.pack(side=tk.RIGHT, fill=tk.Y, pady=4)

    # =========================================================================
    # TAB 2: DOWNLINK HK & FLASH DATA
    # =========================================================================
    downlink_paned = ttk.PanedWindow(tab_downlink, orient=tk.HORIZONTAL)
    downlink_paned.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

    dl_left = ttk.Frame(downlink_paned)
    downlink_paned.add(dl_left, weight=3)

    dl_right = ttk.Frame(downlink_paned)
    downlink_paned.add(dl_right, weight=3)

    hk_card_box = ttk.LabelFrame(
        dl_left, text=" Satellite Housekeeping Telemetry (Combined 80-Byte GMSK Packet) ")
    hk_card_box.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)

    hk_grid = tk.Frame(hk_card_box, bg="#FFFFFF")
    hk_grid.pack(fill=tk.BOTH, expand=True, padx=8, pady=6)

    hk_vars = {}
    hk_fields = [
        ("vbat", "Battery Voltage", "V", "#0284C7"),
        ("vsol", "Solar Panel Voltage", "V", "#0284C7"),
        ("vbus", "3V3 Regulated Bus", "V", "#0284C7"),
        ("t_ant", "Antenna Temperature", "°C", "#EA580C"),
        ("t_bat", "Battery Temperature", "°C", "#EA580C"),
        ("t_bpb", "BPB Temperature", "°C", "#EA580C"),
        ("i_unreg", "Unregulated Current", "A", "#7C3AED"),
        ("i_3v3", "3V3 Rail Current", "A", "#7C3AED"),
        ("i_5v", "5V Rail Current", "A", "#7C3AED"),
        ("i_bat", "Battery Current", "A", "#7C3AED"),
        ("gx", "Gyro X (Angular Rate)", "c-dps", "#059669"),
        ("gy", "Gyro Y (Angular Rate)", "c-dps", "#059669"),
        ("gz", "Gyro Z (Angular Rate)", "c-dps", "#059669"),
        ("mx", "Magnetometer X", "uT", "#0D9488"),
        ("my", "Magnetometer Y", "uT", "#0D9488"),
        ("mz", "Magnetometer Z", "uT", "#0D9488"),
    ]

    for i, (k, name, u, col) in enumerate(hk_fields):
        r, c = divmod(i, 2)
        card = tk.Frame(hk_grid, bg="#F8FAFC", relief="solid", bd=1)
        card.grid(row=r, column=c, sticky="nsew", padx=4, pady=3)
        hk_grid.columnconfigure(c, weight=1)
        hk_grid.rowconfigure(r, weight=1)

        tk.Label(card, text=name + ":", bg="#F8FAFC", fg=TEXT_MUTED,
                 font=("Segoe UI", 9)).pack(side=tk.LEFT, padx=8, pady=4)
        v = tk.StringVar(value="--")
        hk_vars[k] = (v, u)
        tk.Label(card, textvariable=v, bg="#F8FAFC", fg=col,
                 font=("Courier", 11, "bold")).pack(side=tk.RIGHT, padx=8, pady=4)

    hk_meta_var = tk.StringVar(
        value="Last Telemetry Sync: None | Packet Seq: -- | CRC: --")
    tk.Label(hk_card_box, textvariable=hk_meta_var, bg="#FFFFFF", fg=TEXT_MUTED,
             font=("Segoe UI", 9, "italic")).pack(side=tk.BOTTOM, pady=6)

    flash_viewer_box = ttk.LabelFrame(
        dl_right, text=" External Flash Data Downlink Viewer (128 Bytes / Chunk) ")
    flash_viewer_box.pack(fill=tk.BOTH, expand=True, padx=6, pady=6)

    flash_header_var = tk.StringVar(
        value="Flash Packets Received: 0 | Address: -- | Status: Ready")
    tk.Label(flash_viewer_box, textvariable=flash_header_var, bg="#FFFFFF",
             fg=PRIMARY_BLUE, font=("Segoe UI", 9, "bold")).pack(anchor="w",
                                                                 padx=8, pady=4)

    flash_text = scrolledtext.ScrolledText(flash_viewer_box, bg="#F8FAFC",
                                           fg=TEXT_MAIN, font=("Monospace", 9),
                                           height=18, relief="solid", bd=1)
    flash_text.pack(fill=tk.BOTH, expand=True, padx=8, pady=4)

    def save_flash_dump():
        if not state["flash_data_bytes"]:
            messagebox.showinfo("Empty",
                                "No flash data packets have been received yet.")
            return
        path = filedialog.asksaveasfilename(
            defaultextension=".bin",
            filetypes=[("Binary files", "*.bin"), ("All files", "*.*")])
        if path:
            with open(path, "wb") as f:
                f.write(state["flash_data_bytes"])
            messagebox.showinfo(
                "Saved",
                f"Exported {len(state['flash_data_bytes'])} bytes of flash data to:\n{path}")

    f_tool_row = tk.Frame(flash_viewer_box, bg="#FFFFFF")
    f_tool_row.pack(fill=tk.X, padx=8, pady=4)
    ttk.Button(f_tool_row, text="📊 Export All to Excel (.xlsx)...",
               command=lambda: export_all_to_excel(silent=False),
               style="Success.TButton").pack(side=tk.LEFT, padx=4)
    ttk.Button(f_tool_row, text="💾 Export Flash Data to File...",
               command=save_flash_dump).pack(side=tk.LEFT, padx=4)
    ttk.Button(f_tool_row, text="Clear Flash Viewer",
               command=lambda: flash_text.delete("1.0", tk.END)).pack(side=tk.RIGHT,
                                                                     padx=4)

    # =========================================================================
    # TAB 3: LIVE CAMERA PHOTO DOWNLINK & JPEG VIEWER
    # =========================================================================
    cam_top_frame = tk.Frame(tab_camera, bg="#FFFFFF", padx=10, pady=8, relief="solid", bd=1)
    cam_top_frame.pack(fill=tk.X, padx=6, pady=6)

    cam_info_var = tk.StringVar(value="Camera Downlink Status: Ready | Chunks: 0/69 | 0 Bytes")
    tk.Label(cam_top_frame, textvariable=cam_info_var, bg="#FFFFFF", fg=PRIMARY_BLUE,
             font=("Segoe UI", 10, "bold")).pack(side=tk.LEFT, padx=6)

    cam_soi_var = tk.StringVar(value="SOI (FFD8): [WAITING]")
    cam_soi_lbl = tk.Label(cam_top_frame, textvariable=cam_soi_var, bg="#FEF3C7", fg="#B45309",
                           font=("Monospace", 9, "bold"), padx=6, pady=2, relief="solid", bd=1)
    cam_soi_lbl.pack(side=tk.LEFT, padx=6)

    cam_eoi_var = tk.StringVar(value="EOI (FFD9): [WAITING]")
    cam_eoi_lbl = tk.Label(cam_top_frame, textvariable=cam_eoi_var, bg="#FEF3C7", fg="#B45309",
                           font=("Monospace", 9, "bold"), padx=6, pady=2, relief="solid", bd=1)
    cam_eoi_lbl.pack(side=tk.LEFT, padx=6)

    cam_prog = ttk.Progressbar(cam_top_frame, orient="horizontal", mode="determinate", length=220)
    cam_prog.pack(side=tk.RIGHT, padx=8)

    # Main split: Left is Image Display Canvas & Controls, Right is 69-Chunk Matrix, SDR Ingest & Log
    cam_paned = ttk.PanedWindow(tab_camera, orient=tk.HORIZONTAL)
    cam_paned.pack(fill=tk.BOTH, expand=True, padx=6, pady=(0, 6))

    cam_left = ttk.LabelFrame(cam_paned, text=" Live photo from GS radio (GMSK 435.000 MHz, not SDR) ")
    cam_paned.add(cam_left, weight=3)

    cam_right = ttk.Frame(cam_paned)
    cam_paned.add(cam_right, weight=3)

    cam_canvas = tk.Canvas(cam_left, bg="#0F172A", highlightthickness=0)
    cam_canvas.pack(fill=tk.BOTH, expand=True, padx=8, pady=(8, 4))
    cam_canvas.create_text(
        240, 160, text="Waiting for GMSK camera chunks from this GS radio\n(435.000 MHz AX.25 / G3RUH — not SDR)",
        fill="#94A3B8", font=("Segoe UI", 11), justify=tk.CENTER)

    cam_left_btns = tk.Frame(cam_left, bg="#FFFFFF")
    cam_left_btns.pack(fill=tk.X, padx=8, pady=(2, 6))
    ttk.Button(cam_left_btns, text="📊 Export Camera & Telemetry to Excel (.xlsx)",
               command=lambda: export_all_to_excel(silent=False),
               style="Success.TButton").pack(side=tk.LEFT, padx=4)
    ttk.Button(cam_left_btns, text="🗑 Clear Photo Buffer",
               command=lambda: clear_camera_buffer()).pack(side=tk.RIGHT, padx=4)

    # --- 69-Chunk Visual Status Matrix Widget ---
    cam_matrix_box = ttk.LabelFrame(cam_right, text=" 📊 69-Chunk Downlink Status Matrix (128B each) ")
    cam_matrix_box.pack(fill=tk.X, padx=4, pady=(2, 4))

    cam_matrix_status_var = tk.StringVar(value="Received: 0/69 chunks (0%) | Missing: 69 chunks (All)")
    tk.Label(cam_matrix_box, textvariable=cam_matrix_status_var, bg="#FFFFFF", fg=PRIMARY_BLUE,
             font=("Segoe UI", 9, "bold")).pack(anchor="w", padx=6, pady=(4, 2))

    cam_matrix_cells = []
    matrix_grid = tk.Frame(cam_matrix_box, bg="#FFFFFF")
    matrix_grid.pack(fill=tk.X, padx=4, pady=4)

    def inspect_chunk(c_idx):
        if c_idx in state.get("cam_chunks", {}):
            cdata = state["cam_chunks"][c_idx]
            stamp = time.strftime("%H:%M:%S")
            cam_chunk_log.insert(tk.END, f"\n[{stamp}] --- Chunk #{c_idx + 1}/69 Inspector (Offset 0x{c_idx*128:04X}, {len(cdata)} B) ---\n")
            hex_str = ' '.join(f'{b:02X}' for b in cdata)
            cam_chunk_log.insert(tk.END, f"HEX: {hex_str}\n")
            ascii_str = ''.join(chr(b) if 32 <= b < 127 else '.' for b in cdata)
            cam_chunk_log.insert(tk.END, f"ASC: {ascii_str}\n\n")
            cam_chunk_log.see(tk.END)
        else:
            messagebox.showinfo("Missing Chunk", f"Chunk #{c_idx + 1} (Offset 0x{c_idx*128:04X}) has not been received yet.")

    for row in range(3):
        r_f = tk.Frame(matrix_grid, bg="#FFFFFF")
        r_f.pack(fill=tk.X, pady=1)
        for col in range(23):
            c_no = row * 23 + col
            if c_no < 69:
                lbl = tk.Label(r_f, text=f"{c_no + 1}", width=3, height=1,
                               bg="#E2E8F0", fg="#475569", font=("Monospace", 8, "bold"),
                               relief="solid", bd=1, cursor="hand2")
                lbl.pack(side=tk.LEFT, padx=1)
                def _make_handler(idx=c_no):
                    return lambda e: inspect_chunk(idx)
                lbl.bind("<Button-1>", _make_handler(c_no))
                cam_matrix_cells.append(lbl)

    # --- STM32WL55 Radio / Raw Packet Ingest Console ---
    sdr_ingest_box = ttk.LabelFrame(cam_right, text=" Optional: paste SDR hex only if the GS radio missed chunks ")
    sdr_ingest_box.pack(fill=tk.X, padx=4, pady=4)

    sdr_paste_txt = scrolledtext.ScrolledText(sdr_ingest_box, height=3, font=("Monospace", 8),
                                              bg="#F8FAFC", relief="solid", bd=1)
    sdr_paste_txt.pack(fill=tk.X, padx=4, pady=2)
    sdr_paste_txt.insert(tk.END, "# Paste STM32WL55 radio / raw G3RUH / AX.25 hex text here and click Ingest\n")

    sdr_btn_row = tk.Frame(sdr_ingest_box, bg="#FFFFFF")
    sdr_btn_row.pack(fill=tk.X, padx=4, pady=3)

    # --- Chunk Inspector Log ---
    cam_log_box = ttk.LabelFrame(cam_right, text=" 📜 Chunk Inspector Log ")
    cam_log_box.pack(fill=tk.BOTH, expand=True, padx=4, pady=(2, 4))

    cam_chunk_log = scrolledtext.ScrolledText(cam_log_box, bg="#F8FAFC", fg=TEXT_MAIN,
                                              font=("Monospace", 9), relief="solid", bd=1)
    cam_chunk_log.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

    def update_camera_photo_display():
        chunks = state.get("cam_chunks", {})
        if not chunks:
            return

        tot = max(state.get("cam_total_expected", 69), 69)
        full_buf = bytearray(tot * 128)
        for p_idx, cdata in chunks.items():
            if p_idx < tot:
                full_buf[p_idx * 128 : p_idx * 128 + len(cdata)] = cdata
        state["cam_bytes"] = full_buf

        raw_b = full_buf
        has_soi = b"\xff\xd8" in raw_b
        has_eoi = b"\xff\xd9" in raw_b

        if has_soi:
            cam_soi_var.set("SOI (FFD8): ✔ DETECTED")
            cam_soi_lbl.config(bg="#DCFCE7", fg="#15803D")
        if has_eoi:
            cam_eoi_var.set("EOI (FFD9): ✔ COMPLETE")
            cam_eoi_lbl.config(bg="#DCFCE7", fg="#15803D")

        if HAS_PIL and has_soi:
            try:
                soi_idx = raw_b.find(b"\xff\xd8")
                img_data = raw_b[soi_idx:]
                if not has_eoi:
                    img_data = img_data + b"\xff\xd9"
                pil_img = Image.open(io.BytesIO(img_data))
                cw = max(cam_canvas.winfo_width(), 320)
                ch = max(cam_canvas.winfo_height(), 240)
                scale = min((cw - 20) / pil_img.width, (ch - 20) / pil_img.height, 2.5)
                nw = max(int(pil_img.width * scale), 10)
                nh = max(int(pil_img.height * scale), 10)
                resized = pil_img.resize((nw, nh), Image.Resampling.LANCZOS)
                tk_img = ImageTk.PhotoImage(resized)
                cam_canvas.delete("all")
                cam_canvas.create_image(cw // 2, ch // 2, image=tk_img, anchor=tk.CENTER)
                cam_canvas.image = tk_img
            except Exception:
                pass

        if has_soi and (has_eoi or len(chunks) >= tot):
            try:
                soi = raw_b.find(b"\xff\xd8")
                eoi = raw_b.rfind(b"\xff\xd9") + 2 if b"\xff\xd9" in raw_b else len(raw_b)
                photo_data = bytes(raw_b[soi:eoi])
                photo_path = os.path.join(os.path.dirname(__file__), "gs_downloaded_photo.jpg")
                with open(photo_path, "wb") as pf:
                    pf.write(photo_data)
                set_ack_received("Camera Photo Saved", f"{len(photo_data)} B saved to gs_downloaded_photo.jpg", is_ok=True)
                cam_info_var.set(f"Photo Complete & Saved! {len(photo_data)} Bytes -> gs_downloaded_photo.jpg")
            except Exception:
                pass

    def ingest_camera_chunk(pkt_idx, total_p, offset, cdata, source="RX"):
        if "cam_chunks" not in state:
            state["cam_chunks"] = {}

        if total_p and total_p > 0:
            state["cam_total_expected"] = total_p
        tot = state.get("cam_total_expected", 69)

        if pkt_idx is None:
            if offset is not None:
                pkt_idx = offset // 128
            else:
                for i in range(tot):
                    if i not in state["cam_chunks"]:
                        pkt_idx = i
                        break
                if pkt_idx is None:
                    pkt_idx = len(state["cam_chunks"])

        state["cam_chunks"][pkt_idx] = cdata

        # Update visual matrix badge
        if pkt_idx < len(cam_matrix_cells):
            cam_matrix_cells[pkt_idx].config(bg="#16A34A", fg="#FFFFFF")

        num_recv = len(state["cam_chunks"])
        missing = [i + 1 for i in range(tot) if i not in state["cam_chunks"]]

        cam_matrix_status_var.set(
            f"Received: {num_recv}/{tot} chunks ({int(num_recv/tot*100)}%) | Missing: {len(missing)} chunks"
        )
        cam_prog["maximum"] = tot
        cam_prog["value"] = num_recv
        cam_info_var.set(
            f"[{source}] Chunk #{pkt_idx + 1}/{tot} ({len(cdata)} B) | Progress: {int(num_recv/tot*100)}%"
        )

        stamp = time.strftime("%H:%M:%S")
        cam_chunk_log.insert(tk.END, f"[{stamp}] [{source}] Chunk #{pkt_idx + 1}/{tot} (off=0x{pkt_idx*128:04X}, len={len(cdata)} B)\n")
        cam_chunk_log.insert(tk.END, f"  {' '.join(f'{b:02X}' for b in cdata[:16])}...\n")
        cam_chunk_log.see(tk.END)

        # Check JPEG markers
        is_soi = False
        is_eoi = False
        for i in range(len(cdata) - 1):
            if cdata[i] == 0xFF and cdata[i + 1] == 0xD8:
                is_soi = True
            if cdata[i] == 0xFF and cdata[i + 1] == 0xD9:
                is_eoi = True

        hex_preview = " ".join(f"{b:02X}" for b in cdata[:16])
        rec = {
            "pkt_idx": (pkt_idx + 1) if pkt_idx is not None else 1,
            "total_p": tot,
            "offset_hex": f"0x{pkt_idx * 128:04X}" if pkt_idx is not None else (f"0x{offset:04X}" if offset is not None else "0x0000"),
            "offset_dec": pkt_idx * 128 if pkt_idx is not None else (offset if offset is not None else 0),
            "length": len(cdata),
            "soi": "0xFFD8 [SOI]" if is_soi else "-",
            "eoi": "0xFFD9 [EOI]" if is_eoi else "-",
            "hex_preview": hex_preview,
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "source": source,
        }

        # Check mission type: dummy camera test vs real camera photo
        if state.get("current_mission_type") == "DUMMY_CAMERA" or "dummy" in source.lower():
            if "dummy_cam_records" not in state:
                state["dummy_cam_records"] = {}
            state["dummy_cam_records"][pkt_idx] = rec
        else:
            if "real_cam_records" not in state:
                state["real_cam_records"] = {}
            state["real_cam_records"][pkt_idx] = rec

        update_camera_photo_display()

        if auto_save_excel_var.get() and (len(state["cam_chunks"]) % 10 == 0 or is_eoi or len(state["cam_chunks"]) >= tot):
            export_all_to_excel(silent=True)

    def on_ingest_pasted_sdr():
        txt = sdr_paste_txt.get("1.0", tk.END).strip()
        if not txt:
            messagebox.showwarning("Empty", "Ingest box is empty. Paste SDR/G3RUH/AX.25 hex data first.")
            return

        chunks = extract_chunks_from_text(txt)
        if chunks:
            for c in chunks:
                ingest_camera_chunk(c[0], c[1], c[2], c[3], source="SDR-PASTE")
            b_count = len(state.get("cam_bytes", b""))
            messagebox.showinfo("Ingested", f"Successfully ingested {len(chunks)} camera chunks from pasted SDR data!\nPhoto Buffer: {b_count} Bytes.")
            return

        # Check if non-camera AX.25 G3RUH frames (HK, Telecommands, ACKs) were present
        all_tokens = re.findall(r"\b[0-9a-fA-F]{2}\b", txt)
        if len(all_tokens) >= 18:
            raw_all = bytes([int(x, 16) for x in all_tokens])
            frames = decode_g3ruh_ax25(raw_all)
            if frames:
                for df in frames:
                    dest = df["dest"]
                    src = df["src"]
                    pl = df["payload"]
                    append_log(f"[SDR-PASTE] Decoded G3RUH AX.25: FROM={src} TO={dest} ({len(pl)}B) | CRC=OK", "ack")
                    if len(pl) == 128:
                        handle_flash_hex_line("[FLASH-HEX] " + " ".join(f"{b:02X}" for b in pl))
                messagebox.showinfo("AX.25 Decoded", f"Successfully decoded {len(frames)} AX.25 G3RUH frames!\nPayloads processed into telemetry/log.")
                return

        messagebox.showwarning("No Chunks", "No camera frames (0xCAFE / G3RUH / AX.25) found in pasted text.")

    def on_load_sdr_file():
        path = filedialog.askopenfilename(
            title="Select SDR Decoded / Log File",
            filetypes=[("Log / Text / Binary", "*.txt *.log *.hex *.bin *.dat"), ("All files", "*.*")])
        if path:
            try:
                with open(path, "rb") as f:
                    content = f.read()
                chunks = extract_camera_chunks_from_bytes(content)
                if not chunks:
                    txt = content.decode("utf-8", errors="ignore")
                    chunks = extract_chunks_from_text(txt)
                if not chunks:
                    messagebox.showwarning("No Chunks", f"No camera frames found in:\n{path}")
                    return
                for c in chunks:
                    ingest_camera_chunk(c[0], c[1], c[2], c[3], source="SDR-FILE")
                messagebox.showinfo("File Loaded", f"Successfully ingested {len(chunks)} chunks from:\n{os.path.basename(path)}")
            except Exception as ex:
                messagebox.showerror("Error", f"Failed reading file: {ex}")

    ttk.Button(sdr_btn_row, text="⚡ Ingest & Decode Pasted Hex", command=on_ingest_pasted_sdr,
               style="Action.TButton").pack(side=tk.LEFT, padx=3)
    ttk.Button(sdr_btn_row, text="📂 Load Hex / Packet File...", command=on_load_sdr_file).pack(side=tk.LEFT, padx=3)
    ttk.Button(sdr_btn_row, text="Clear Ingest Box", command=lambda: sdr_paste_txt.delete("1.0", tk.END)).pack(side=tk.LEFT, padx=3)

    def retransmit_missing_chunks():
        tot = state.get("cam_total_expected", 69)
        missing = [i for i in range(tot) if i not in state.get("cam_chunks", {})]
        if not missing:
            messagebox.showinfo("Complete", "All chunks have been received! No missing chunks.")
            return
        miss_str = ', '.join(str(m + 1) for m in missing[:15])
        if len(missing) > 15:
            miss_str += f"... (+{len(missing)-15} more)"
        q = f"Detected {len(missing)} missing chunks:\n[{miss_str}]\n\nGenerate and send retransmission telecommands now?"
        if messagebox.askyesno("Retransmit Missing Chunks", q):
            for m in missing:
                addr = m * 128
                _, raw = build_telecommand_hex(MCU_ID_OBC, OPCODE_CAM_DOWN, 0x0000, addr, 1)
                send_hex_bytes(raw)
                time.sleep(0.04)
            set_ack_state("await", "Retransmit Missing", f"Requested {len(missing)} missing chunks")

    def open_photo_external():
        photo_path = os.path.join(os.path.dirname(__file__), "gs_downloaded_photo.jpg")
        if os.path.exists(photo_path):
            try:
                if sys.platform.startswith("linux"):
                    subprocess.Popen(["xdg-open", photo_path])
                elif sys.platform.startswith("win"):
                    os.startfile(photo_path)
                elif sys.platform.startswith("darwin"):
                    subprocess.Popen(["open", photo_path])
            except Exception as e:
                messagebox.showerror("Error", f"Failed to open image: {e}")
        else:
            messagebox.showinfo("Not Found", "No photo saved yet. Downlink chunks or ingest SDR data first.")

    def save_camera_photo():
        raw_b = state.get("cam_bytes", bytearray())
        if len(raw_b) == 0:
            messagebox.showinfo("Empty", "No camera data downloaded yet.")
            return
        soi = raw_b.find(b"\xff\xd8") if b"\xff\xd8" in raw_b else 0
        eoi = raw_b.rfind(b"\xff\xd9") + 2 if b"\xff\xd9" in raw_b else len(raw_b)
        path = filedialog.asksaveasfilename(
            defaultextension=".jpg",
            filetypes=[("JPEG files", "*.jpg *.jpeg"), ("All files", "*.*")])
        if path:
            with open(path, "wb") as f:
                f.write(raw_b[soi:eoi])
            messagebox.showinfo("Saved", f"Photo successfully saved ({eoi-soi} bytes):\n{path}")

    def clear_camera_buffer():
        state["cam_chunks"] = {}
        state["cam_bytes"] = bytearray()
        cam_info_var.set("Camera Downlink Status: Cleared | Chunks: 0/69 | 0 Bytes")
        cam_matrix_status_var.set("Received: 0/69 chunks (0%) | Missing: 69 chunks (All)")
        cam_soi_var.set("SOI (FFD8): [WAITING]")
        cam_soi_lbl.config(bg="#FEF3C7", fg="#B45309")
        cam_eoi_var.set("EOI (FFD9): [WAITING]")
        cam_eoi_lbl.config(bg="#FEF3C7", fg="#B45309")
        cam_prog["value"] = 0
        for cell in cam_matrix_cells:
            cell.config(bg="#E2E8F0", fg="#475569")
        cam_canvas.delete("all")
        cam_canvas.create_text(
            240, 160, text="Waiting for GMSK camera chunks from this GS radio\n(435.000 MHz AX.25 / G3RUH — not SDR)",
            fill="#94A3B8", font=("Segoe UI", 11), justify=tk.CENTER)
        cam_chunk_log.delete("1.0", tk.END)

    # Interactive Camera Downlink & Command Controls
    cam_ctrl_frame = ttk.LabelFrame(cam_left, text=" 🎮 Camera Telecommands (437.375 MHz) & SDR Controls ")
    cam_ctrl_frame.pack(fill=tk.X, padx=8, pady=(0, 6))

    c_row1 = tk.Frame(cam_ctrl_frame, bg="#FFFFFF")
    c_row1.pack(fill=tk.X, padx=4, pady=3)

    tk.Label(c_row1, text="Start Addr:", bg="#FFFFFF", fg=TEXT_MAIN,
             font=("Segoe UI", 9, "bold")).pack(side=tk.LEFT, padx=2)
    cam_addr_var = tk.StringVar(value="0x00000000")
    ttk.Entry(c_row1, textvariable=cam_addr_var, width=12,
              font=("Monospace", 9)).pack(side=tk.LEFT, padx=4)

    tk.Label(c_row1, text="Packets (0=All/69):", bg="#FFFFFF", fg=TEXT_MAIN,
             font=("Segoe UI", 9, "bold")).pack(side=tk.LEFT, padx=(10, 2))
    cam_pkts_var = tk.StringVar(value="69")
    ttk.Entry(c_row1, textvariable=cam_pkts_var, width=8,
              font=("Monospace", 9)).pack(side=tk.LEFT, padx=4)

    def on_cam_downlink_btn():
        try:
            a_str = cam_addr_var.get().strip()
            c_addr = int(a_str, 16) if (a_str.startswith("0x") or a_str.startswith("0X")) else int(a_str)
            c_pkts = int(cam_pkts_var.get().strip())
            if c_pkts < 0: c_pkts = 0
            if c_pkts > 65535: c_pkts = 65535
            state["cam_bytes"] = bytearray()
            _, raw = build_telecommand_hex(MCU_ID_OBC, OPCODE_CAM_DOWN, 0x0000, c_addr, c_pkts)
            send_hex_bytes(raw)
            set_ack_state("await", "Camera Photo Download",
                          f"Downlinking {c_pkts if c_pkts > 0 else 69} chunks from 0x{c_addr:08X}")
        except Exception as e:
            messagebox.showerror("Error", f"Failed to build camera download command: {e}")

    ttk.Button(c_row1, text="📥 Downlink Photo (1D D2 F5)", command=on_cam_downlink_btn,
               style="Action.TButton").pack(side=tk.LEFT, padx=6)
    ttk.Button(c_row1, text="📥 Request Missing Chunks", command=retransmit_missing_chunks,
               style="Primary.TButton").pack(side=tk.LEFT, padx=4)

    c_row2 = tk.Frame(cam_ctrl_frame, bg="#FFFFFF")
    c_row2.pack(fill=tk.X, padx=4, pady=3)

    def on_cam_dummy_btn():
        clear_camera_buffer()
        send_text("DC")
        set_ack_state("await", "Dummy Camera Test", "Downlinking all 69 chunks (8.8KB image)")

    def on_cam_real_capture_btn():
        send_hex_bytes(cam_on_raw)
        set_ack_state("await", "Real Camera Capture", "Triggering UART2 0xCA capture of 2 photos to flash")

    ttk.Button(c_row2, text="🧪 Dummy Camera (All 69 Chunks)", command=on_cam_dummy_btn).pack(side=tk.LEFT, padx=2)
    ttk.Button(c_row2, text="📷 Real Camera Capture (UART2)", command=on_cam_real_capture_btn).pack(side=tk.LEFT, padx=3)
    ttk.Button(c_row2, text="💾 Save Photo As...", command=save_camera_photo,
               style="Action.TButton").pack(side=tk.LEFT, padx=3)
    ttk.Button(c_row2, text="🖼️ Open Photo", command=open_photo_external).pack(side=tk.LEFT, padx=3)
    ttk.Button(c_row2, text="Clear Buffer", command=clear_camera_buffer).pack(side=tk.LEFT, padx=2)

    # --- Row 3: SDR Network Listener Status & Control ---
    c_row3 = tk.Frame(cam_ctrl_frame, bg="#FFFFFF")
    c_row3.pack(fill=tk.X, padx=4, pady=3)

    sdr_server = None
    sdr_status_var = tk.StringVar(value="📡 SDR Listener [Port 8001]: OFF")

    def toggle_sdr_server():
        nonlocal sdr_server
        if sdr_server and sdr_server.running:
            sdr_server.stop()
            sdr_server = None
            sdr_status_var.set("📡 SDR Listener [Port 8001]: OFF")
            btn_sdr_toggle.config(text="Start SDR Listener", style="Action.TButton")
            append_log("[SDR-SERVER] Stopped.\n", "info")
        else:
            sdr_server = SDRSocketServer(
                port=8001,
                chunk_callback=lambda idx, tot, off, dat, src: root.after(0, ingest_camera_chunk, idx, tot, off, dat, src),
                log_callback=lambda msg, tag: root.after(0, append_log, msg, tag)
            )
            sdr_server.start()
            sdr_status_var.set("📡 SDR Listener [Port 8001]: ACTIVE")
            btn_sdr_toggle.config(text="Stop SDR Listener", style="Crimson.TButton")
            append_log("[SDR-SERVER] Listening on 0.0.0.0:8001 (KISS / UDP / TCP)...\n", "info")

    tk.Label(c_row3, textvariable=sdr_status_var, bg="#FFFFFF", fg=PRIMARY_BLUE,
             font=("Monospace", 9, "bold")).pack(side=tk.LEFT, padx=4)
    btn_sdr_toggle = ttk.Button(c_row3, text="Start SDR Listener", command=toggle_sdr_server,
                                style="Action.TButton")
    btn_sdr_toggle.pack(side=tk.LEFT, padx=6)
    # SDR listener is started after GUI initialization completes below

    # =========================================================================
    # TAB 4: RAW SERIAL TERMINAL
    # =========================================================================
    log_top = tk.Frame(tab_log, bg="#FFFFFF", padx=8, pady=6,
                       relief="solid", bd=1)
    log_top.pack(fill=tk.X, padx=4, pady=(4, 0))

    autoscroll_var = tk.BooleanVar(value=True)
    ttk.Checkbutton(log_top, text="Autoscroll",
                    variable=autoscroll_var).pack(side=tk.LEFT, padx=4)

    raw_log = scrolledtext.ScrolledText(tab_log, bg="#FFFFFF", fg=TEXT_MAIN,
                                        font=("Monospace", 9), height=18,
                                        relief="solid", bd=1)
    raw_log.pack(fill=tk.BOTH, expand=True, padx=4, pady=4)

    for tag, colour in [
        ("tx", "#0284C7"),
        ("rx", "#16A34A"),
        ("ack", "#059669"),
        ("flash", "#7C3AED"),
        ("error", "#DC2626"),
        ("info", "#003893"),
        ("telem", "#D97706"),
    ]:
        raw_log.tag_config(tag, foreground=colour)

    ttk.Button(log_top, text="Clear Console",
               command=lambda: raw_log.delete("1.0", tk.END)).pack(side=tk.RIGHT,
                                                                   padx=4)

    def save_full_log():
        path = filedialog.asksaveasfilename(
            defaultextension=".log",
            filetypes=[("Log files", "*.log"), ("All files", "*.*")])
        if path:
            with open(path, "w", encoding="utf-8") as f:
                f.write(raw_log.get("1.0", tk.END))
            messagebox.showinfo("Log Saved", f"Log successfully exported to:\n{path}")

    ttk.Button(log_top, text="Export Log...",
               command=save_full_log).pack(side=tk.RIGHT, padx=4)

    # =========================================================================
    # INCOMING DATA DISPATCHERS
    # =========================================================================
    def _set_hk_card(key, val_str):
        if key in hk_vars:
            hk_vars[key][0].set(val_str)

    def handle_hk_telemetry(obj):
        state["packets_rx"] += 1
        seq = obj.get("seq", 0)
        crc_ok = obj.get("crc", 1)

        _set_hk_card("vbat", f"{obj.get('vbat', 0)/100.0:.2f} V")
        _set_hk_card("vsol", f"{obj.get('vsol', 0)/100.0:.2f} V")
        _set_hk_card("vbus", f"{obj.get('vbus', 0)/100.0:.2f} V")

        _set_hk_card("t_ant", f"{obj.get('t_ant', 0)/10.0:.1f} °C")
        _set_hk_card("t_bat", f"{obj.get('t_bat', 0)/10.0:.1f} °C")
        _set_hk_card("t_bpb", f"{obj.get('t_bpb', 0)/10.0:.1f} °C")

        _set_hk_card("i_unreg", f"{obj.get('i_unreg', 0)/100.0:.2f} A")
        _set_hk_card("i_3v3", f"{obj.get('i_3v3', 0)/100.0:.2f} A")
        _set_hk_card("i_5v", f"{obj.get('i_5v', 0)/100.0:.2f} A")
        _set_hk_card("i_bat", f"{obj.get('i_bat', 0)/100.0:.2f} A")

        _set_hk_card("gx", f"{obj.get('gx', 0):+d} c-dps")
        _set_hk_card("gy", f"{obj.get('gy', 0):+d} c-dps")
        _set_hk_card("gz", f"{obj.get('gz', 0):+d} c-dps")

        _set_hk_card("mx", f"{obj.get('mx', 0):+d} uT")
        _set_hk_card("my", f"{obj.get('my', 0):+d} uT")
        _set_hk_card("mz", f"{obj.get('mz', 0):+d} uT")

        stamp = time.strftime("%H:%M:%S")
        crc_str = "OK" if crc_ok else "ERR"
        hk_meta_var.set(
            f"Last Telemetry Sync: {stamp} | Packet Seq: #{seq} | "
            f"CRC: [{crc_str}] | RSSI: {obj.get('rssi', -90)} dBm")
        set_ack_received("Telemetry Received",
                         f"Live HK Packet #{seq} (CRC {crc_str})",
                         is_ok=bool(crc_ok))

        # Record into structured HK history for Excel export
        hk_entry = {
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
            "seq": seq,
            "rssi": obj.get("rssi", -90),
            "vbat": round(obj.get("vbat", 0)/100.0, 2),
            "vsol": round(obj.get("vsol", 0)/100.0, 2),
            "vbus": round(obj.get("vbus", 0)/100.0, 2),
            "t_ant": round(obj.get("t_ant", 0)/10.0, 1),
            "t_bat": round(obj.get("t_bat", 0)/10.0, 1),
            "t_bpb": round(obj.get("t_bpb", 0)/10.0, 1),
            "i_unreg": round(obj.get("i_unreg", 0)/100.0, 2),
            "i_3v3": round(obj.get("i_3v3", 0)/100.0, 2),
            "i_5v": round(obj.get("i_5v", 0)/100.0, 2),
            "i_bat": round(obj.get("i_bat", 0)/100.0, 2),
            "gx": obj.get("gx", 0),
            "gy": obj.get("gy", 0),
            "gz": obj.get("gz", 0),
            "mx": obj.get("mx", 0),
            "my": obj.get("my", 0),
            "mz": obj.get("mz", 0),
            "crc": "VALID" if crc_ok else "ERR"
        }
        if "hk_history" not in state:
            state["hk_history"] = []
        state["hk_history"].append(hk_entry)

        if auto_save_excel_var.get():
            export_all_to_excel(silent=True)

    def handle_flash_chunk_json(obj):
        state["packets_rx"] += 1
        state["flash_chunks_rx"] += 1
        pkt = obj.get("pkt", 1)
        total = obj.get("total", 1)
        addr = obj.get("addr", 0)
        status = obj.get("status", 0)
        dlen = obj.get("len", 128)
        flash_header_var.set(
            f"Flash Packet: {pkt}/{total} | Addr: 0x{addr:08X} | "
            f"Status: {status} (OK) | Len: {dlen} B")
        set_ack_received("Flash Packet Received",
                         f"Packet {pkt} of {total} @ 0x{addr:08X}")

        # Update telemetry cards if fields are present in JSON
        if "vbat" in obj:
            _set_hk_card("vbat", f"{obj['vbat']/100.0:.2f} V")
            _set_hk_card("vsol", f"{obj['vsol']/100.0:.2f} V")
            _set_hk_card("vbus", f"{obj['vbus']/100.0:.2f} V")
            _set_hk_card("t_ant", f"{obj['t_ant']/10.0:.1f} °C")
            _set_hk_card("t_bat", f"{obj['t_bat']/10.0:.1f} °C")
            _set_hk_card("t_bpb", f"{obj['t_bpb']/10.0:.1f} °C")
            _set_hk_card("i_unreg", f"{obj['i_unreg']/100.0:.2f} A")
            _set_hk_card("i_3v3", f"{obj['i_3v3']/100.0:.2f} A")
            _set_hk_card("i_5v", f"{obj['i_5v']/100.0:.2f} A")
            _set_hk_card("i_bat", f"{obj['i_bat']/100.0:.2f} A")
            _set_hk_card("gx", f"{obj['gx']:+d} c-dps")
            _set_hk_card("gy", f"{obj['gy']:+d} c-dps")
            _set_hk_card("gz", f"{obj['gz']:+d} c-dps")
            _set_hk_card("mx", f"{obj['mx']:+d} uT")
            _set_hk_card("my", f"{obj['my']:+d} uT")
            _set_hk_card("mz", f"{obj['mz']:+d} uT")
            stamp = time.strftime("%H:%M:%S")
            hk_meta_var.set(
                f"Last Flash Sync: {stamp} | Packet {pkt}/{total} | "
                f"Addr: 0x{addr:08X} "
                f"(Footers: 0x{obj.get('f1', 0):04X}, 0x{obj.get('f2', 0):04X})")

            # Also record to HK table
            hk_entry = {
                "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
                "seq": pkt,
                "rssi": state.get("last_rx_rssi", -90),
                "vbat": round(obj.get('vbat', 0)/100.0, 2),
                "vsol": round(obj.get('vsol', 0)/100.0, 2),
                "vbus": round(obj.get('vbus', 0)/100.0, 2),
                "t_ant": round(obj.get('t_ant', 0)/10.0, 1),
                "t_bat": round(obj.get('t_bat', 0)/10.0, 1),
                "t_bpb": round(obj.get('t_bpb', 0)/10.0, 1),
                "i_unreg": round(obj.get('i_unreg', 0)/100.0, 2),
                "i_3v3": round(obj.get('i_3v3', 0)/100.0, 2),
                "i_5v": round(obj.get('i_5v', 0)/100.0, 2),
                "i_bat": round(obj.get('i_bat', 0)/100.0, 2),
                "gx": obj.get('gx', 0),
                "gy": obj.get('gy', 0),
                "gz": obj.get('gz', 0),
                "mx": obj.get('mx', 0),
                "my": obj.get('my', 0),
                "mz": obj.get('mz', 0),
                "crc": "VALID"
            }
            if "hk_history" not in state:
                state["hk_history"] = []
            state["hk_history"].append(hk_entry)

        # Record to flash history
        f_entry = {
            "pkt": pkt,
            "total": total,
            "addr_hex": f"0x{addr:08X}",
            "addr_dec": addr,
            "len": dlen,
            "f1": f"0x{obj.get('f1', 0):04X}",
            "f2": f"0x{obj.get('f2', 0):04X}",
            "f_end": "0xAACC",
            "status": status,
            "timestamp": time.strftime("%Y-%m-%d %H:%M:%S")
        }
        if "flash_history" not in state:
            state["flash_history"] = []
        state["flash_history"].append(f_entry)

        if auto_save_excel_var.get():
            export_all_to_excel(silent=True)

    def handle_flash_hex_line(hex_str):
        clean = (hex_str.replace("[FLASH-HEX]", "")
                        .replace("[FLASH-FRAME-HEX]", "")
                        .strip())
        tokens = clean.split()
        if not tokens:
            return
        try:
            raw_bytes = bytes([int(x, 16) for x in tokens])
            state["flash_data_bytes"].extend(raw_bytes)
        except Exception:
            raw_bytes = b""

        stamp = time.strftime("%H:%M:%S")
        flash_text.insert(
            tk.END,
            f"\n{'='*74}\n[{stamp}] FLASH PACKET "
            f"(HEX STREAM - {len(tokens)} BYTES)\n{'='*74}\n")

        parsed = decode_flash_payload_bytes(raw_bytes)
        if parsed:
            a1 = parsed['adc1']
            a2 = parsed['adc2']
            imu = parsed['imu']
            f1 = parsed['f1']
            f2 = parsed['f2']
            f_end = parsed['f_end']

            f1_ok = "[VALID 0xAA55]" if f1 == 0xAA55 else f"[0x{f1:04X}]"
            f2_ok = "[VALID 0xBB66]" if f2 == 0xBB66 else f"[0x{f2:04X}]"
            if f_end is None:
                f_end_ok = "[not present]"
            else:
                f_end_ok = "[VALID 0xAACC]" if f_end == 0xAACC \
                    else f"[0x{f_end:04X}]"

            # Update live telemetry dashboard cards
            _set_hk_card("vbat", f"{a1[0]/100.0:.2f} V")
            _set_hk_card("vsol", f"{a1[1]/100.0:.2f} V")
            _set_hk_card("vbus", f"{a1[2]/100.0:.2f} V")
            _set_hk_card("t_ant", f"{a1[8]/10.0:.1f} °C")
            _set_hk_card("t_bat", f"{a1[9]/10.0:.1f} °C")
            _set_hk_card("t_bpb", f"{a1[10]/10.0:.1f} °C")
            _set_hk_card("i_unreg", f"{a2[0]/100.0:.2f} A")
            _set_hk_card("i_3v3", f"{a2[1]/100.0:.2f} A")
            _set_hk_card("i_5v", f"{a2[2]/100.0:.2f} A")
            _set_hk_card("i_bat", f"{a2[3]/100.0:.2f} A")
            _set_hk_card("gx", f"{imu[0]:+d} c-dps")
            _set_hk_card("gy", f"{imu[1]:+d} c-dps")
            _set_hk_card("gz", f"{imu[2]:+d} c-dps")
            _set_hk_card("mx", f"{imu[3]:+d} uT")
            _set_hk_card("my", f"{imu[4]:+d} uT")
            _set_hk_card("mz", f"{imu[5]:+d} uT")
            hk_meta_var.set(
                f"Last Flash Sync: {stamp} | Decoded ADC1 & ADC2 with IMU "
                f"({f1_ok} {f2_ok} {f_end_ok})")

            # Record to HK history
            hk_entry = {
                "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
                "seq": len(state.get("hk_history", [])) + 1,
                "rssi": state.get("last_rx_rssi", -90),
                "vbat": round(a1[0]/100.0, 2),
                "vsol": round(a1[1]/100.0, 2),
                "vbus": round(a1[2]/100.0, 2),
                "t_ant": round(a1[8]/10.0, 1),
                "t_bat": round(a1[9]/10.0, 1),
                "t_bpb": round(a1[10]/10.0, 1),
                "i_unreg": round(a2[0]/100.0, 2),
                "i_3v3": round(a2[1]/100.0, 2),
                "i_5v": round(a2[2]/100.0, 2),
                "i_bat": round(a2[3]/100.0, 2),
                "gx": imu[0],
                "gy": imu[1],
                "gz": imu[2],
                "mx": imu[3],
                "my": imu[4],
                "mz": imu[5],
                "crc": "VALID" if (f1 == 0xAA55 and f2 == 0xBB66) else "ERR"
            }
            if "hk_history" not in state:
                state["hk_history"] = []
            state["hk_history"].append(hk_entry)

            # Record to flash history
            f_entry = {
                "pkt": len(state.get("flash_history", [])) + 1,
                "total": state.get("flash_pkt_total", 1),
                "addr_hex": f"0x{len(state.get('flash_history', [])) * 128:08X}",
                "addr_dec": len(state.get("flash_history", [])) * 128,
                "len": len(raw_bytes),
                "f1": f"0x{f1:04X}",
                "f2": f"0x{f2:04X}",
                "f_end": f"0x{f_end:04X}" if f_end else "-",
                "status": 0,
                "timestamp": time.strftime("%Y-%m-%d %H:%M:%S")
            }
            if "flash_history" not in state:
                state["flash_history"] = []
            state["flash_history"].append(f_entry)

            if auto_save_excel_var.get():
                export_all_to_excel(silent=True)

            summary = (
                f"► DECODED TELEMETRY (BETWEEN HEADER & FOOTER):\n"
                f"  ADC1: Bat={a1[0]/100.0:.2f}V, Sol={a1[1]/100.0:.2f}V, "
                f"Bus={a1[2]/100.0:.2f}V, AntT={a1[8]/10.0:.1f}°C, "
                f"BatT={a1[9]/10.0:.1f}°C, BPBT={a1[10]/10.0:.1f}°C  {f1_ok}\n"
                f"  ADC2: Unreg={a2[0]/100.0:.2f}A, 3V3={a2[1]/100.0:.2f}A, "
                f"5V={a2[2]/100.0:.2f}A, Bat={a2[3]/100.0:.2f}A\n"
                f"  IMU : Gyro=[{imu[0]:+d}, {imu[1]:+d}, {imu[2]:+d}] c-dps  |  "
                f"Mag=[{imu[3]:+d}, {imu[4]:+d}, {imu[5]:+d}] uT  {f2_ok}\n"
                f"  Packet End Footer: {f_end_ok}\n\n"
            )
            flash_text.insert(tk.END, summary)

        # Print Formatted 16-Bytes-Per-Row HEX & ASCII Grid
        flash_text.insert(tk.END, "► RAW HEX BYTE GRID:\n")
        for i in range(0, len(tokens), 16):
            chunk_tokens = tokens[i:i+16]
            hex_part = " ".join(chunk_tokens)
            ascii_part = "".join(
                chr(int(x, 16)) if 32 <= int(x, 16) < 127 else "."
                for x in chunk_tokens)
            flash_text.insert(
                tk.END,
                f"+{i:04X}  {hex_part:<48}  |{ascii_part}|\n")
        flash_text.see(tk.END)

    def process_incoming_line(raw):
        line = raw.strip()
        if not line:
            return

        # JSON payloads
        if line.startswith("{") and line.endswith("}"):
            try:
                obj = json.loads(line)
                t = obj.get("type")
                if t == "HK":
                    handle_hk_telemetry(obj)
                    append_log(raw, "telem")
                    return
                elif t == "CAMERA":
                    pkt = obj.get("pkt", 1)
                    total = obj.get("total", 69)
                    dlen = obj.get("len", 128)
                    offset = obj.get("offset", (pkt - 1) * 128)
                    state["last_cam_pkt"] = pkt
                    state["last_cam_total"] = total
                    state["last_cam_offset"] = offset
                    hexstr = obj.get("hex", "")
                    if hexstr:
                        try:
                            raw_bytes = bytes.fromhex(hexstr)
                            ingest_camera_chunk(pkt - 1, total, offset, raw_bytes,
                                                source="GS-RADIO")
                        except ValueError:
                            pass
                    cam_prog["maximum"] = total
                    cam_prog["value"] = len(state.get("cam_chunks", {}))
                    b_count = len(state.get("cam_bytes", b""))
                    cam_info_var.set(
                        f"[GS radio] Photo chunk {pkt}/{total} | "
                        f"{len(state.get('cam_chunks', {}))}/{total} stored | {b_count} B")
                    set_ack_received("Camera Photo Packet",
                                     f"GS radio chunk {pkt}/{total} ({dlen} B)", is_ok=True)
                    append_log(raw, "flash")
                    return
                elif t == "FLASH":
                    handle_flash_chunk_json(obj)
                    append_log(raw, "flash")
                    return
                elif t == "ACK":
                    msg = obj.get("msg", "")
                    set_ack_received("Command Confirmed", msg, is_ok=True)
                    append_log(raw, "ack")
                    return
                elif t == "NACK":
                    msg = obj.get("msg", "")
                    set_ack_received("Command Rejected (NACK)", msg, is_ok=False)
                    append_log(raw, "error")
                    return
                elif t == "GMSK":
                    append_log(raw, "rx")
                    return
                elif t == "CWRX":
                    txt = obj.get("text", "")
                    update_cw_panel(state_txt="DECODED", live=txt, last=txt, color="#15803D")
                    append_log(raw, "ack")
                    return
                elif t == "CWSIG":
                    on = obj.get("on", 0)
                    rssi = obj.get("rssi", None)
                    update_cw_panel(state_txt="CARRIER" if on else "IDLE", rssi=rssi,
                                    color="#15803D" if on else "#334155")
                    append_log(raw, "info")
                    return
                elif t == "CWCAR":
                    rssi = obj.get("rssi", None)
                    ms = obj.get("ms", 0)
                    update_cw_panel(state_txt="CARRIER", rssi=rssi,
                                    live=f"Tuning tone {ms} ms", color="#15803D")
                    append_log(raw, "info")
                    return
                elif t == "CWCHAR":
                    ch = obj.get("ch", "")
                    cur = cw_live_var.get()
                    if cur.startswith("("):
                        cur = ""
                    update_cw_panel(state_txt="KEYING", live=(cur + ch)[-48:], color="#B45309")
                    return
                elif t == "WF":
                    # Waterfall samples: skip logging (too spammy)
                    return
            except ValueError:
                pass

        # Check for G3RUH / SDR Raw Bytes Hex Stream Start Header
        if "[GS RX] Raw G3RUH" in line or "[STEP 1: READ RAW]" in line or "Raw G3RUH on-air bytes" in line:
            state["raw_g3ruh_acc"] = bytearray()
            append_log(raw, "rx")
            return

        # Pure hex line accumulator (e.g. "55 35 B9 89 1A 91 FF EC 25 7F F8 64 BB 4A C8 24")
        hex_tokens = re.findall(r"\b[0-9a-fA-F]{2}\b", line)
        is_pure_hex = (len(hex_tokens) >= 8 and len(line.split()) == len(hex_tokens))
        if is_pure_hex:
            try:
                line_bytes = bytes([int(x, 16) for x in hex_tokens])
                if "raw_g3ruh_acc" not in state:
                    state["raw_g3ruh_acc"] = bytearray()
                state["raw_g3ruh_acc"].extend(line_bytes)
                append_log(raw, "rx")

                # If full frame (>= 200 bytes) reached, trigger G3RUH AX.25 software decode
                if len(state["raw_g3ruh_acc"]) >= 200:
                    wire_pkt = bytes(state["raw_g3ruh_acc"][:200])
                    state["raw_g3ruh_acc"] = state["raw_g3ruh_acc"][200:]
                    decoded_frames = decode_g3ruh_ax25(wire_pkt)
                    if decoded_frames:
                        for df in decoded_frames:
                            dest = df["dest"]
                            src = df["src"]
                            pl = df["payload"]
                            append_log(f"★ [G3RUH AX.25 RX] DECODE SUCCESS: FROM={src} TO={dest} ({len(pl)}B Payload) | CRC=OK", "ack")
                            cam_chunks = extract_camera_chunks_from_bytes(pl)
                            if cam_chunks:
                                for c in cam_chunks:
                                    ingest_camera_chunk(c[0], c[1], c[2], c[3], source="GS-RADIO")
                            # 2. Check if payload is HK 128B Telemetry
                            elif len(pl) == 128 and ((pl[32] == 0x55 and pl[33] == 0xAA) or (pl[32] == 0xAA and pl[33] == 0x55)):
                                handle_flash_hex_line("[FLASH-HEX] " + " ".join(f"{b:02X}" for b in pl))
                            # 3. Check if ACK / NACK
                            elif len(pl) >= 1 and pl[0] in (0xAC, 0xEE, 0xAA):
                                set_ack_received("Satellite ACK (G3RUH)", f"Byte 0=0x{pl[0]:02X} len={len(pl)}", is_ok=True)
                            elif len(pl) >= 1 and pl[0] in (0xFF, 0x55):
                                set_ack_received("Satellite NACK (G3RUH)", f"Byte 0=0x{pl[0]:02X} len={len(pl)}", is_ok=False)
                            elif len(pl) == 13 and pl[0] == 0x53:
                                append_log(f"★ [AX.25 TC] Telecommand Decoded: {pl.hex(' ').upper()}", "ack")
                    else:
                        cam_chunks = extract_camera_chunks_from_bytes(wire_pkt)
                        if cam_chunks:
                            for c in cam_chunks:
                                ingest_camera_chunk(c[0], c[1], c[2], c[3], source="RAW-WIRE")
                return
            except Exception:
                pass
        elif "raw_g3ruh_acc" in state and len(state["raw_g3ruh_acc"]) >= 100:
            wire_pkt = bytes(state["raw_g3ruh_acc"])
            state["raw_g3ruh_acc"] = bytearray()
            decoded_frames = decode_g3ruh_ax25(wire_pkt)
            if decoded_frames:
                for df in decoded_frames:
                    dest = df["dest"]
                    src = df["src"]
                    pl = df["payload"]
                    append_log(f"★ [G3RUH AX.25 RX] DECODE SUCCESS: FROM={src} TO={dest} ({len(pl)}B) | CRC=OK", "ack")
                    cam_chunks = extract_camera_chunks_from_bytes(pl)
                    if cam_chunks:
                        for c in cam_chunks:
                                    ingest_camera_chunk(c[0], c[1], c[2], c[3], source="GS-RADIO")
                    elif len(pl) == 128 and ((pl[32] == 0x55 and pl[33] == 0xAA) or (pl[32] == 0xAA and pl[33] == 0x55)):
                        handle_flash_hex_line("[FLASH-HEX] " + " ".join(f"{b:02X}" for b in pl))
                    elif len(pl) >= 1 and pl[0] in (0xAC, 0xEE, 0xAA):
                        set_ack_received("Satellite ACK (G3RUH)", f"Byte 0=0x{pl[0]:02X}", is_ok=True)
                    elif len(pl) >= 1 and pl[0] in (0xFF, 0x55):
                        set_ack_received("Satellite NACK (G3RUH)", f"Byte 0=0x{pl[0]:02X}", is_ok=False)
            else:
                # Direct check: might be already-unscrambled 144B camera chunk or 128B HK telemetry
                cam_chunks = extract_camera_chunks_from_bytes(wire_pkt)
                if cam_chunks:
                    for c in cam_chunks:
                        ingest_camera_chunk(c[0], c[1], c[2], c[3], source="GS-RADIO")
                elif len(wire_pkt) == 128:
                    handle_flash_hex_line("[FLASH-HEX] " + " ".join(f"{b:02X}" for b in wire_pkt))

        # Check for [GS CAM] or *** CAMERA CHUNK status headers from STM32WL55JC2
        if "[GS CAM]" in line:
            m_cam = re.search(r"Pkt\s+(\d+)/(\d+)\s+off=0x([0-9a-fA-F]+)\s+len=(\d+)", line)
            if m_cam:
                pkt = int(m_cam.group(1))
                total = int(m_cam.group(2))
                offset = int(m_cam.group(3), 16)
                dlen = int(m_cam.group(4))
                state["last_cam_pkt"] = pkt
                state["last_cam_total"] = total
                state["last_cam_offset"] = offset
                cam_prog["maximum"] = total
                cam_prog["value"] = pkt
                b_count = len(state.get("cam_bytes", b""))
                cam_info_var.set(f"Receiving Photo from STM32WL55 Radio... Chunk {pkt}/{total} | {b_count} B")
            if "DOWNLOAD COMPLETE" in line:
                set_ack_received("Camera Download Complete", line.strip(), is_ok=True)
                cam_info_var.set("Photo Download Complete (All chunks received)!")
            append_log(raw, "info")
            return
        elif "*** CAMERA CHUNK" in line:
            m_chunk = re.search(r"pkt_idx=(\d+)\s+total=(\d+)", line)
            if m_chunk:
                p_idx = int(m_chunk.group(1))
                total = int(m_chunk.group(2))
                state["last_cam_pkt"] = p_idx + 1
                state["last_cam_total"] = total
                state["last_cam_offset"] = p_idx * 128
            append_log(raw, "flash")
            return

        if "[CW DECODED]" in line:
            txt = line.split("[CW DECODED]", 1)[-1].strip()
            update_cw_panel(state_txt="DECODED", live=txt, last=txt, color="#15803D")
            append_log(raw, "ack")
            return
        if "[CW] Carrier detected" in line:
            m = re.search(r"RSSI\s+(-?\d+)", line)
            update_cw_panel(state_txt="CARRIER",
                            rssi=int(m.group(1)) if m else None, color="#15803D")
            append_log(raw, "info")
            return
        if "[CW] Tuning carrier" in line:
            m = re.search(r"(\d+)\s+ms.*RSSI\s+(-?\d+)", line)
            ms = int(m.group(1)) if m else 0
            rssi = int(m.group(2)) if m else None
            update_cw_panel(state_txt="CARRIER", rssi=rssi,
                            live=f"Tuning tone {ms} ms", color="#15803D")
            append_log(raw, "info")
            return

        # Text lines
        if "[SATELLITE-ACK]" in line or "ACK: CAM" in line or "ACK: PING" in line:
            set_ack_received("Satellite ACK",
                             line.replace("[SATELLITE-ACK]", "").strip(),
                             is_ok=True)
            append_log(raw, "ack")
        elif "[SATELLITE-NACK]" in line:
            set_ack_received("Satellite NACK",
                             line.replace("[SATELLITE-NACK]", "").strip(),
                             is_ok=False)
            append_log(raw, "error")
        elif "[CAMERA-HEX]" in line or "[CAMERA-CHUNK]" in line or "[CAMERA]" in line:
            if "[CAMERA-HEX]" in line:
                clean = line.replace("[CAMERA-HEX]", "").strip()
                tokens = re.findall(r"\b[0-9a-fA-F]{2}\b", clean)
                if tokens:
                    try:
                        raw_bytes = bytes([int(x, 16) for x in tokens])
                        sub = extract_camera_chunks_from_bytes(raw_bytes)
                        if sub:
                            for c in sub:
                                    ingest_camera_chunk(c[0], c[1], c[2], c[3], source="GS-RADIO")
                        else:
                            cur_pkt = state.get("last_cam_pkt", None)
                            p_idx = (cur_pkt - 1) if (cur_pkt and cur_pkt > 0) else None
                            tot = state.get("last_cam_total", 69)
                            off = state.get("last_cam_offset", None)
                            ingest_camera_chunk(p_idx, tot, off, raw_bytes, source="GS-RADIO")
                    except Exception:
                        pass
            append_log(raw, "flash")
            return
        elif any(magic in line for magic in ("CA FE", "FE CA", "ca fe", "fe ca")):
            sdr_chunks = extract_chunks_from_text(line)
            if sdr_chunks:
                for c in sdr_chunks:
                    ingest_camera_chunk(c[0], c[1], c[2], c[3], source="RADIO-RAW")
                append_log(raw, "flash")
                return
        elif "[GS CAM]" in line:
            if "DOWNLOAD COMPLETE" in line:
                set_ack_received("Camera Download Complete", line.strip(), is_ok=True)
                cam_info_var.set("Photo Download Complete (All chunks received)!")
            append_log(raw, "info")
        elif "[FLASH-HEX]" in line and "[FLASH-FRAME-HEX]" not in line:
            handle_flash_hex_line(line)
            append_log(raw, "flash")
        elif "[FLASH-CHUNK]" in line:
            append_log(raw, "flash")
        elif "[RX-DOWNLINK]" in line:
            append_log(raw, "rx")
        elif "CMD-TX" in line or "[UPLINK-TX]" in line:
            append_log(raw, "tx")
        elif "ERR" in line or "TIMEOUT" in line or "FAIL" in line:
            append_log(raw, "error")
        else:
            append_log(raw, "info")

    def pump():
        try:
            for _ in range(200):
                process_incoming_line(rx_queue.get_nowait())
        except queue.Empty:
            pass
        root.after(40, pump)

    reconnecting = False

    def find_reconnect_port(old_port, usb_serial):
        """The USB-UART re-enumerates (e.g. ttyUSB0 -> ttyUSB1) after a drop;
        find it again by USB serial number, else by the old device name."""
        ports = list(serial.tools.list_ports.comports())
        if usb_serial:
            for p in ports:
                if p.serial_number == usb_serial:
                    return p.device
        for p in ports:
            if p.device == old_port:
                return p.device
        return None

    def serial_reader():
        nonlocal ser, reconnecting
        while running and ser is not None:
            try:
                line = ser.readline()
                if line:
                    rx_queue.put(line.decode("utf-8", errors="replace"))
                continue
            except Exception as e:
                if not running:
                    break
                rx_queue.put(f"!!! [SERIAL READ ERROR] {e}\n")

            # USB-UART dropped (typically RF/EMI from the GS's own +22 dBm TX).
            # Keep the session alive and reopen the adapter when it comes back.
            old_port = ser.port
            usb_serial = None
            for p in serial.tools.list_ports.comports():
                if p.device == old_port:
                    usb_serial = p.serial_number
            usb_serial = usb_serial or state.get("usb_serial")
            reconnecting = True
            try:
                ser.close()
            except Exception:
                pass
            rx_queue.put(f"!!! [SERIAL] Link to {old_port} lost - waiting for the USB-UART to come back...\n")
            while running:
                time.sleep(0.5)
                p = find_reconnect_port(old_port, usb_serial)
                if not p:
                    continue
                try:
                    new_ser = serial.Serial(p, int(baud_var.get()), timeout=0.1)
                except Exception:
                    continue
                ser = new_ser
                reconnecting = False
                rx_queue.put(f"--- [SERIAL] Reconnected on {p} ---\n")
                root.after(0, lambda p=p: (port_var.set(p), update_connection_state_ui(True, p)))
                break
            reconnecting = False

    def update_connection_state_ui(connected, port_str=""):
        if connected:
            status_var.set(f"● CONNECTED ({port_str})")
            status_lbl.config(style="StatusConnected.TLabel")
            btn_connect.config(text="Disconnect", style="Crimson.TButton")
        else:
            status_var.set("● DISCONNECTED")
            status_lbl.config(style="StatusDisconnected.TLabel")
            btn_connect.config(text="Connect", style="Primary.TButton")

    def on_serial_disconnect(reason=""):
        """Called when the reader thread detects a fatal serial error."""
        nonlocal ser, running
        running = False
        try:
            if ser and ser.is_open:
                ser.close()
        except Exception:
            pass
        ser = None
        update_connection_state_ui(False)
        if reason:
            append_log(f"--- Serial link lost: {reason} ---\n", "error")
        else:
            append_log("--- Serial port disconnected ---\n", "info")

    def toggle_conn():
        nonlocal ser, running, reader_thread
        if ser and ser.is_open:
            running = False
            try:
                ser.close()
            except Exception:
                pass
            ser = None
            update_connection_state_ui(False)
            append_log("--- Serial port disconnected ---\n", "info")
        else:
            p = port_var.get()
            try:
                ser = serial.Serial(p, int(baud_var.get()), timeout=0.1)
                ser.reset_input_buffer()
                ser.reset_output_buffer()
                state["usb_serial"] = next((x.serial_number for x in serial.tools.list_ports.comports()
                                            if x.device == p), None)
                running = True
                update_connection_state_ui(True, p)
                append_log(
                    f"--- Connected to {p} @ {baud_var.get()} 8N1 ---\n",
                    "info")
                reader_thread = threading.Thread(target=serial_reader,
                                                 daemon=True)
                reader_thread.start()
                # Reset counters on new connection
                state["packets_rx"] = 0
                state["acks_rx"] = 0
                state["flash_chunks_rx"] = 0
                quick_link_var.set(
                    f"RF: ACTIVE | Packets: 0 | ACKs: 0")
                reset_ack_banner()

                # Automatically configure Ground Station hardware to +22 dBm HP RFO
                def init_gs_link():
                    time.sleep(0.2)
                    if ser and ser.is_open:
                        try:
                            ser.write(b"PA HP\r\n")
                            ser.flush()
                            time.sleep(0.05)
                            ser.write(b"PWR 22\r\n")
                            ser.flush()
                            time.sleep(0.05)
                            ser.write(b"S\r\n")
                            ser.flush()
                        except Exception:
                            pass
                threading.Thread(target=init_gs_link, daemon=True).start()
            except Exception as e:
                messagebox.showerror("Connection Failed",
                                     f"Could not open serial port {p}:\n{e}")

    btn_connect.config(command=toggle_conn)

    # Watchdog: if the reader thread dies unexpectedly, update UI
    def connection_watchdog():
        if running and not reconnecting and (ser is None or not ser.is_open):
            on_serial_disconnect("serial port closed unexpectedly")
        root.after(1000, connection_watchdog)

    # Initial Welcome Banner
    append_log("=" * 76 + "\n", "info")
    append_log(f" {ORG_NE} | {ORG_EN}\n", "info")
    append_log(f" {APP_TITLE} (GMSK Telecommand & Telemetry Segment)\n", "info")
    append_log(f" Downlink RX : {RX_FREQ_STR}\n", "info")
    append_log(f" Uplink TX   : {TX_FREQ_STR}\n", "info")
    append_log(" STM32WL55JC2 Ground Station Radio Interface Ready (RFO_HP +22 dBm).\n", "info")
    append_log(" Auto-detected port: Connect to /dev/ttyUSB0 to receive live satellite downlink.\n", "info")
    append_log("=" * 76 + "\n", "info")

    def on_closing():
        nonlocal running
        running = False
        try:
            if ser and ser.is_open:
                ser.close()
        except Exception:
            pass
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_closing)
    root.after(40, pump)
    root.after(1000, connection_watchdog)
    root.mainloop()


# ============================================================================
# CLI MODE FALLBACK
# ============================================================================

def run_cli(port=None, baudrate=115200):
    if not port:
        all_p = [p.device for p in serial.tools.list_ports.comports()]
        usb_ports = [p for p in all_p if "ttyUSB" in p or "ttyACM" in p]
        if not usb_ports:
            usb_ports = glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")
        port = usb_ports[0] if usb_ports else "/dev/ttyUSB0"

    print("=" * 72)
    print(f"{ORG_NE}  |  {ORG_EN}")
    print(f"{APP_TITLE}")
    print(f"Connecting to {port} @ {baudrate} 8N1 (Press Ctrl+C to exit)...")
    print("=" * 72)

    try:
        ser = serial.Serial(port, baudrate, timeout=0.1)
    except Exception as e:
        print(f"[ERROR] Could not open {port}: {e}")
        return

    running = True

    try:
        time.sleep(0.15)
        ser.write(b"PA HP\r\n")
        ser.flush()
        time.sleep(0.05)
        ser.write(b"PWR 22\r\n")
        ser.flush()
        time.sleep(0.05)
        ser.write(b"S\r\n")
        ser.flush()
    except Exception:
        pass

    usb_serial = next((x.serial_number for x in serial.tools.list_ports.comports()
                       if x.device == port), None)

    def reader():
        nonlocal ser
        while running:
            try:
                raw = ser.readline()
            except Exception as e:
                print(f"\n!!! [SERIAL] Link to {ser.port} lost ({e}) - waiting for the USB-UART to come back...")
                try:
                    ser.close()
                except Exception:
                    pass
                while running:
                    time.sleep(0.5)
                    p = next((x.device for x in serial.tools.list_ports.comports()
                              if usb_serial and x.serial_number == usb_serial), None)
                    if not p and os.path.exists(ser.port):
                        p = ser.port
                    if not p:
                        continue
                    try:
                        ser = serial.Serial(p, baudrate, timeout=0.1)
                        print(f"--- [SERIAL] Reconnected on {p} ---")
                        break
                    except Exception:
                        continue
                continue
            if not raw:
                continue
            line = raw.decode("utf-8", errors="replace")
            print(line, end="")

    threading.Thread(target=reader, daemon=True).start()
    try:
        while True:
            cmd = input()
            ser.write((cmd + "\r\n").encode("utf-8"))
            ser.flush()
    except (KeyboardInterrupt, EOFError):
        print("\nExiting Ground Station...")
    finally:
        running = False
        try:
            ser.close()
        except Exception:
            pass


# ============================================================================
# ENTRY POINT
# ============================================================================

if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser(
        description="STM32WL55 Ground Station - GMSK Command & Downlink Console")
    ap.add_argument("-p", "--port", default=None,
                    help="Serial port (e.g. /dev/ttyACM0)")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("--cli", action="store_true",
                    help="Force command-line interface mode")
    a = ap.parse_args()

    if a.cli or not (os.environ.get("DISPLAY") or sys.platform in ("win32", "darwin")):
        run_cli(a.port, a.baud)
    else:
        run_gui(a.port, a.baud)