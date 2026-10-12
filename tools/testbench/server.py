#!/usr/bin/env python3
"""S2S2 Testbench -- debug software for the satellite and the ground-station board (local web GUI, http://127.0.0.1:8765).

    tools/.venv/bin/python tools/testbench/server.py [--port 8765] [--host 127.0.0.1] [--no-browser]

Consoles of both boards (GS FTDI UART, Nucleo ST-Link VCP), an SWD profiler (CPU time per function, SRAM map, watches), command tests,
end-to-end and image tests, digipeater and SDR/Direwolf tests, screenshots and reports (HTML / PDF / ZIP). Nothing is simulated and nothing
is opened until you press a button: the serial ports, the ST-Link and the SDR stay untouched at start-up.

Layout: this file only starts the server. The code is the package tb/ (util, bus, links, state, imaging, devices, digi, tests_*, e2e,
profiler, profiling, commands, consoles, runner, status, build, reports, web). The Mission Control program is separate: tools/mission_control.
"""
import argparse
import subprocess
from http.server import ThreadingHTTPServer

from tb.devices import SDR
from tb.links import GS, OBC
from tb.profiling import PROF, monitor_stop
from tb.reports import PORT
from tb.web import H


def main():
    ap = argparse.ArgumentParser(description="S2S2 Testbench")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--no-browser", action="store_true")
    a = ap.parse_args()
    PORT[0] = a.port
    srv = ThreadingHTTPServer((a.host, a.port), H)
    url = f"http://{a.host}:{a.port}/"
    print("S2S2 testbench:", url)
    if not a.no_browser:
        subprocess.Popen(["open", url])
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        PROF.stop()
        monitor_stop()
        SDR.stop()
        GS.disconnect()
        OBC.disconnect()


if __name__ == "__main__":
    main()
