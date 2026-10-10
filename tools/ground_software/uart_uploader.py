#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
==============================================================================
STM32WL55 Ground Station UART Telecommand & Telemetry Console / Uploader
Antarikchya Pratisthan Nepal  |  अन्तरिक्ष प्रतिष्ठान नेपाल
Location: /home/prem/Desktop/nuttxspace/Ground_Station/uart_uploader.py

Direct serial interface to STM32WL55 Ground Station firmware (gs_main.c)
via ST-LINK Virtual COM Port (PA2/PA3 LPUART1 @ 115200 8N1).

Transmits commands over RF to Satellite M0+ (437.375 MHz Uplink) in AX.25
G3RUH scrambled packets, and receives Satellite Downlink (435.000 MHz).
==============================================================================
"""

import sys
import os
import time
import glob
import json
import threading
import argparse

try:
    import serial
    import serial.tools.list_ports
except ImportError:
    print("[ERROR] pyserial is required. Install with: pip3 install pyserial")
    sys.exit(1)

# Protocol Constants
SAT_CALLSIGN = "9N2SJ"
GS_CALLSIGN  = "GROUND"
TX_FREQ_STR  = "437.375 MHz (Uplink to Satellite)"
RX_FREQ_STR  = "435.000 MHz (Downlink from Satellite)"

PRESET_COMMANDS = {
    "ping": {
        "hex": "53 01 1D D2 F5 00 00 00 00 00 00 00 01",
        "desc": "Ping Satellite OBC (Expects ACK: PING OK)",
        "label": "PING"
    },
    "hk1": {
        "hex": "53 01 1D D2 F5 00 01 00 00 00 00 00 01",
        "desc": "Request Combined Live HK Telemetry (80 Bytes)",
        "label": "HK1"
    },
    "hk2": {
        "hex": "53 01 1D D2 F5 00 02 00 00 00 00 00 01",
        "desc": "Request OBC Telemetry Beacon Broadcast",
        "label": "HK2"
    },
    "cam": {
        "hex": "53 04 CC 5E BD 00 00 00 00 00 00 00 01",
        "desc": "Trigger Camera Snapshot (Expects Camera ACK)",
        "label": "CAM"
    },
    "snap": {
        "hex": "53 04 CC 5E BD 00 FF 00 00 00 00 00 01",
        "desc": "Camera Direct Snapshot Request",
        "label": "SNAP"
    }
}

def auto_detect_port():
    """Detect available ST-Link or USB serial ports."""
    ports = [p.device for p in serial.tools.list_ports.comports()]
    if not ports:
        ports = glob.glob("/dev/ttyACM*") + glob.glob("/dev/ttyUSB*")
    return ports[0] if ports else "/dev/ttyACM0"

def build_flash_cmd(start_addr, num_pkts):
    """Build 13-Byte Telecommand: 53 01 1D D1 F2 00 00 [ADDR:4B] [PKTS:2B]"""
    a3 = (start_addr >> 24) & 0xFF
    a2 = (start_addr >> 16) & 0xFF
    a1 = (start_addr >> 8)  & 0xFF
    a0 = start_addr & 0xFF
    c1 = (num_pkts >> 8) & 0xFF
    c0 = num_pkts & 0xFF
    return f"53 01 1D D1 F2 00 00 {a3:02X} {a2:02X} {a1:02X} {a0:02X} {c1:02X} {c0:02X}"

class GSUploader:
    def __init__(self, port, baudrate=115200, timeout=0.1):
        self.port_name = port
        self.baudrate = baudrate
        self.ser = None
        self.running = False
        self.lock = threading.Lock()

    def connect(self):
        try:
            self.ser = serial.Serial(self.port_name, self.baudrate, timeout=0.1)
            self.running = True
            print(f"[OK] Connected to Ground Station on {self.port_name} @ {self.baudrate} 8N1")
            return True
        except Exception as e:
            print(f"[ERROR] Failed to open {self.port_name}: {e}")
            return False

    def close(self):
        self.running = False
        if self.ser and self.ser.is_open:
            try:
                self.ser.close()
            except Exception:
                pass
        print(f"[INFO] Disconnected from {self.port_name}")

    def send_line(self, line):
        """Send formatted command line to Ground Station firmware."""
        if not self.ser or not self.ser.is_open:
            print("[ERROR] Serial port not connected!")
            return False
        with self.lock:
            payload = (line.strip() + "\r\n").encode("utf-8")
            self.ser.write(payload)
            self.ser.flush()
        print(f">>> [GS UART TX] {line.strip()}")
        return True

    def send_hex_command(self, hex_str):
        """Format as 'HEX <bytes>' for Ground Station AX.25 G3RUH packet creation."""
        clean = hex_str.strip().replace(",", " ")
        return self.send_line(f"HEX {clean}")

    def start_reader_thread(self, on_line_cb=None):
        def reader():
            while self.running and self.ser and self.ser.is_open:
                try:
                    raw = self.ser.readline()
                except Exception:
                    break
                if not raw:
                    continue
                line = raw.decode("utf-8", errors="replace")
                if on_line_cb:
                    on_line_cb(line)
                else:
                    self.default_log_handler(line)

        t = threading.Thread(target=reader, daemon=True)
        t.start()
        return t

    def default_log_handler(self, line):
        s = line.strip()
        if not s:
            return

        # High-visibility alerts for incoming telemetry / ACKs
        if "[SATELLITE-ACK]" in s:
            print(f"\033[1;32m[ACK RECEIVED]\033[0m {s}")
        elif "[RX-DOWNLINK]" in s:
            print(f"\033[1;36m[DOWNLINK RF]\033[0m {s}")
        elif "[HK-COMBINED]" in s or "{\"type\":\"HK\"" in s:
            print(f"\033[1;35m[LIVE TELEMETRY]\033[0m {s}")
        elif "[FLASH-CHUNK]" in s:
            print(f"\033[1;33m[FLASH CHUNK]\033[0m {s}")
        elif "ERR" in s or "TIMEOUT" in s or "FAIL" in s:
            print(f"\033[1;31m[GS ERROR]\033[0m {s}")
        elif "[UPLINK-TX]" in s or "[CMD-TX]" in s:
            print(f"\033[1;34m[RF TX]\033[0m {s}")
        else:
            print(f"    {s}")

def print_banner():
    print("=" * 76)
    print("  ANTARIKCHYA PRATISTHAN NEPAL  |  अन्तरिक्ष प्रतिष्ठान नेपाल")
    print("  STM32WL55 Ground Station - AX.25 G3RUH Command & Downlink Uploader")
    print(f"  Downlink RX : {RX_FREQ_STR}")
    print(f"  Uplink TX   : {TX_FREQ_STR}")
    print(f"  Target Sat  : {SAT_CALLSIGN}  |  Modulation: 4800 bps GMSK")
    print("=" * 76)

def interactive_session(uploader):
    print("\n[INTERACTIVE MODE] Type command number, shortcut, or HEX payload:")
    print("  1      -> Request Combined Live HK Telemetry (80B)")
    print("  2      -> Request Secondary HK Telemetry")
    print("  3      -> Trigger Camera Subsystem")
    print("  P      -> Send PING to Satellite OBC")
    print("  S      -> Query Ground Station Link Status & Counters")
    print("  FLASH  -> Read NOR Flash [addr] [pkts] (e.g. FLASH 0 5)")
    print("  HEX    -> Send raw hex bytes (e.g. HEX 53 01 1D D2 F5 00 00 ...)")
    print("  ?      -> Help list from Ground Station firmware")
    print("  Q      -> Exit")
    print("-" * 76)

    try:
        while True:
            cmd = input("\033[1mGS > \033[0m").strip()
            if not cmd:
                continue
            if cmd.lower() in ("q", "quit", "exit"):
                break
            elif cmd.upper() == "P":
                uploader.send_hex_command(PRESET_COMMANDS["ping"]["hex"])
            elif cmd in ("1", "HK", "hk", "HK1"):
                uploader.send_hex_command(PRESET_COMMANDS["hk1"]["hex"])
            elif cmd in ("2", "HK2"):
                uploader.send_hex_command(PRESET_COMMANDS["hk2"]["hex"])
            elif cmd in ("3", "CAM", "cam"):
                uploader.send_hex_command(PRESET_COMMANDS["cam"]["hex"])
            elif cmd.upper().startswith("FLASH"):
                parts = cmd.split()
                addr = int(parts[1], 16 if parts[1].startswith("0x") else 10) if len(parts) > 1 else 0
                pkts = int(parts[2]) if len(parts) > 2 else 5
                hex_cmd = build_flash_cmd(addr, pkts)
                print(f"[FLASH REQUEST] Address 0x{addr:08X}, {pkts} packets")
                uploader.send_hex_command(hex_cmd)
            elif cmd.upper().startswith("HEX "):
                uploader.send_line(cmd)
            else:
                uploader.send_line(cmd)
            time.sleep(0.05)
    except (KeyboardInterrupt, EOFError):
        print("\n[INFO] Exiting interactive session...")

def main():
    parser = argparse.ArgumentParser(
        description="STM32WL55 Ground Station - AX.25 G3RUH Command & Downlink Uploader"
    )
    parser.add_argument("-p", "--port", default=None, help="Serial port (e.g. /dev/ttyACM0)")
    parser.add_argument("-b", "--baud", type=int, default=115200, help="Baud rate (default: 115200)")
    parser.add_argument("--ping", action="store_true", help="Send PING command to Satellite")
    parser.add_argument("--hk", action="store_true", help="Request Combined Live HK Telemetry (80 Bytes)")
    parser.add_argument("--cam", action="store_true", help="Trigger Camera Snapshot")
    parser.add_argument("--status", action="store_true", help="Query Ground Station link status")
    parser.add_argument("--carrier", type=int, nargs="?", const=5, default=None, help="Transmit continuous test carrier @ 437.375 MHz for N seconds (default: 5)")
    parser.add_argument("--hex", type=str, default=None, help="Send arbitrary 13-Byte or raw HEX command")
    parser.add_argument("--listen", type=int, default=0, help="Listen for N seconds after sending (default: 10)")
    parser.add_argument("--interactive", "-i", action="store_true", help="Launch interactive command prompt")

    args = parser.parse_args()

    port = args.port or auto_detect_port()
    print_banner()

    uploader = GSUploader(port, args.baud)
    if not uploader.connect():
        sys.exit(1)

    uploader.start_reader_thread()
    time.sleep(0.5)

    has_specific_cmd = (args.ping or args.hk or args.cam or args.status or args.hex or (args.carrier is not None))

    if args.carrier is not None:
        print(f"[TEST] Transmitting Continuous Carrier on 437.375 MHz for {args.carrier} seconds...")
        uploader.send_line(f"CARRIER {args.carrier}")
        time.sleep(args.carrier + 1)
    elif args.ping:
        uploader.send_hex_command(PRESET_COMMANDS["ping"]["hex"])
    elif args.hk:
        uploader.send_hex_command(PRESET_COMMANDS["hk1"]["hex"])
    elif args.cam:
        uploader.send_hex_command(PRESET_COMMANDS["cam"]["hex"])
    elif args.status:
        uploader.send_line("S")
    elif args.hex:
        uploader.send_hex_command(args.hex)

    if args.interactive or not has_specific_cmd:
        interactive_session(uploader)
    elif args.listen > 0 or has_specific_cmd:
        listen_time = args.listen if args.listen > 0 else 10
        print(f"[INFO] Listening for Satellite Downlink responses ({listen_time}s)... Press Ctrl+C to stop.")
        try:
            time.sleep(listen_time)
        except KeyboardInterrupt:
            pass

    uploader.close()

if __name__ == "__main__":
    main()
