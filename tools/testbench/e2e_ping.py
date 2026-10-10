#!/usr/bin/env python3
"""Bench regression: GS -> satellite PING -> ACK/ACK/"PING OK" back on the GS.

Sends PING on the GS UART (115200) every --every seconds until the GS prints the
satellite's ACKs, or --timeout expires. The satellite only listens ~50 s of every
~95 s, so a single PING can land in its CW phase and be lost; hence the retries.

  tools/.venv/bin/python tools/testbench/e2e_ping.py [--port /dev/cu.usbserial-AG0L702O]
Exit code 0 = got ACK Accepted + PING reply (authenticated downlink decoded), 1 otherwise.
"""
import argparse
import re
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/cu.usbserial-AG0L702O")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--timeout", type=float, default=240)
    ap.add_argument("--every", type=float, default=12)
    ap.add_argument("--cmd", default="PING")
    a = ap.parse_args()

    s = serial.Serial(a.port, a.baud, timeout=0.2)
    t0 = time.time()
    last_tx = -1e9
    sent = 0
    buf = ""
    t_ul = t_ack = None        # first "UPLINK COMPLETE" / first ACK after the latest PING
    while time.time() - t0 < a.timeout:
        if time.time() - last_tx >= a.every:
            s.write((a.cmd + "\r\n").encode())
            last_tx = time.time()
            sent += 1
            t_ul = t_ack = None
            buf = ""
        s.timeout = 0.02
        buf += s.read(4096).decode(errors="replace")
        if t_ul is None and "UPLINK COMPLETE" in buf:
            t_ul = time.time()
        if t_ul is not None and t_ack is None and "ACK Accepted" in buf:
            t_ack = time.time()
        acc = "ACK Accepted" in buf
        rep = re.search(r"PING OK|ACK: PING|HK TELEMETRY PACKET DETECTED", buf) is not None   # PING is HEALTH SAT now: ACK, HK packet, ACK
        if acc and rep:
            rssi = re.findall(r"RSSI=(-?\d+) dBm", buf)
            dec = buf.count("AX.25 DECODE OK")
            blk = buf.count("integrity check OK")
            lat = f", uplink-done -> ACK {1000 * (t_ack - t_ul):.0f} ms" if t_ul and t_ack else ""
            print(f"PASS after {time.time() - t0:.0f}s ({sent} PING sent): "
                  f"{dec} frames decoded, {blk} authenticated frames, RSSI {rssi[:3]} dBm{lat}")
            return 0
    print(f"FAIL after {a.timeout:.0f}s ({sent} PING sent): "
          f"ACK Accepted={'ACK Accepted' in buf}, decoded frames={buf.count('AX.25 DECODE OK')}")
    print(buf[-800:].replace("\r\r\n", "\n"))
    return 1


if __name__ == "__main__":
    sys.exit(main())
