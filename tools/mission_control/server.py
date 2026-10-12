#!/usr/bin/env python3
"""
S2S2 Mission Control -- the operator's software for the flight satellite.

Talks to the satellite ONLY through the ground-station board's UART (the GS decodes the radio; this program never touches an SDR, an SWD
probe or the satellite's own serial port). It shows what the satellite reports (CW beacons, housekeeping, attitude from the magnetometer
and gyro, link quality, camera images), sends the mission commands of the Packet Format sheet with the full ACK / NACK life cycle,
keeps a permanent history in SQLite and an audit trail of who sent what.

    python3 tools/mission_control/server.py            # http://127.0.0.1:8770, login required
    python3 tools/mission_control/server.py --host 0.0.0.0 --port 8770 --gs /dev/cu.usbserial-XXXX     (--gs '' starts disconnected)

First start creates the account "admin" with a random password that is printed once. Debug functions (raw UART, SDR, memory profiler,
tests) are NOT here: they live in tools/testbench. Code: package mc/ (core = state + command executor, web = routes).
"""
import argparse
import os
from http.server import ThreadingHTTPServer

from mc import core
from mc.web import H


def main():
    ap = argparse.ArgumentParser(description="S2S2 Mission Control")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8770)
    ap.add_argument("--gs", default=None, help="ground-station serial port (default: first /dev/cu.usbserial-*; pass '' to start disconnected)")
    ap.add_argument("--no-browser", action="store_true")
    a = ap.parse_args()
    core.init(a.gs)
    srv = ThreadingHTTPServer((a.host, a.port), H)
    url = f"http://{a.host}:{a.port}/"
    print("S2S2 Mission Control:", url)
    if not a.no_browser:
        os.system(f"open {url}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
