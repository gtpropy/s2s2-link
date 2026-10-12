#!/usr/bin/env python3
"""
Receive-window test: the ground station sends a few harmless frames while the satellite's debug UART is logged.

The satellite (reference firmware) prints "RX Listening armed" when its 50 s listen window opens and "[STEP 1: READ RAW]" for every
packet its radio completes. This script waits for the window, makes the GS send COUNT frames at fixed offsets inside it and then
reports how many packets the satellite saw. The default payload is NOT a 0x53 telecommand: the satellite logs it and goes back to
listening, it does not answer, so the 5 V PA / DC-DC are not switched on.

  python3 rx_window_test.py --sat-port /dev/cu.usbserial-XXXX --gs-port /dev/cu.usbserial-YYYY --go [--count 3] [--out DIR]

Safety:
  * both ports are opened with DTR / RTS low (set before open) and the satellite port is only read
  * the only thing written is the GS console line `HEX <bytes>` (one RF frame each); nothing else is sent
  * without --go the plan is printed and nothing is opened
  * check the RF coupling first: the GS sends +22 dBm (RFO_HP); the SX126x input limit is +10 dBm. Over the air with antennas that is
    safe (measured on the bench 2026-10-11: satellite CW +14 dBm arrived at the GS at -95 dBm, so about 110 dB of loss); never connect
    the GS to the satellite by a cable without an attenuator.
"""
import argparse
import os
import sys
import time

DEFAULT_PAYLOAD = "11 22 33 44 55 66 77 88 99 AA BB CC DD"       # 13 bytes, first byte not 0x53: not a telecommand


def open_port(path, baud):
    import serial
    s = serial.Serial()
    s.port = path
    s.baudrate = baud
    s.timeout = 0.05
    s.dtr = False
    s.rts = False                                                  # before open(): do not pulse reset / boot lines
    s.open()
    return s


class Reader:
    def __init__(self, ser, path):
        self.ser, self.buf, self.f = ser, b"", open(path, "w")

    def poll(self):
        d = self.ser.read(1024)
        if d:
            self.buf += d
        out = []
        while b"\n" in self.buf:
            line, self.buf = self.buf.split(b"\n", 1)
            line = line.decode("utf-8", "replace").rstrip()
            self.f.write(time.strftime("%H:%M:%S ") + line + "\n")
            self.f.flush()
            out.append(line)
        return out

    def note(self, text):
        self.f.write(time.strftime("%H:%M:%S ") + "### " + text + "\n")
        self.f.flush()


def summarize(sat_log):
    keys = {"raw": "[STEP 1: READ RAW]", "decode_ok": "[DECODE SUCCESS]", "decode_fail": "[DECODE FAILED]", "validate_fail": "[VALIDATE FAILED]",
            "not_command": "COMMAND RECEIVED: NO", "command_yes": "COMMAND RECEIVED: YES"}
    counts = {k: 0 for k in keys}
    for line in open(sat_log, errors="replace"):
        for k, pat in keys.items():
            if pat in line:
                counts[k] += 1
    return counts


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--sat-port", required=True)
    ap.add_argument("--gs-port", required=True)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--count", type=int, default=3)
    ap.add_argument("--first", type=float, default=4.0, help="seconds after the window opens for the first frame")
    ap.add_argument("--gap", type=float, default=10.0, help="seconds between frames")
    ap.add_argument("--payload", default=DEFAULT_PAYLOAD, help="hex bytes for the GS HEX command")
    ap.add_argument("--timeout", type=float, default=260.0, help="give up waiting for a window after this many seconds")
    ap.add_argument("--out", default=".")
    ap.add_argument("--go", action="store_true", help="actually run (without it only the plan is printed)")
    a = ap.parse_args()

    first = bytes.fromhex(a.payload.replace(" ", ""))[:1]
    if first == b"\x53":
        print("refusing: the payload starts with 0x53 (a telecommand); the satellite would answer and switch the 5 V PA on", file=sys.stderr)
        return 2
    plan = "%d frame(s) 'HEX %s' at +%.0f s, then every %.0f s inside one 50 s listen window" % (a.count, a.payload, a.first, a.gap)
    print("plan:", plan)
    if not a.go:
        print("not running (add --go)")
        return 0
    if a.first + a.gap * (a.count - 1) > 45:
        print("refusing: the frames would not all fit in the 50 s window", file=sys.stderr)
        return 2

    os.makedirs(a.out, exist_ok=True)
    sat = Reader(open_port(a.sat_port, a.baud), os.path.join(a.out, "rx_sat.txt"))
    gs = Reader(open_port(a.gs_port, a.baud), os.path.join(a.out, "rx_gs.txt"))
    t_end = time.time() + a.timeout
    t_arm, sends, sent, done_at = None, [], 0, None
    while time.time() < t_end:
        for line in sat.poll():
            if "RX Listening armed" in line and t_arm is None:
                t_arm = time.time()
                sends = [t_arm + a.first + i * a.gap for i in range(a.count)]
                gs.note("window armed, frames scheduled")
            if "Listen Window Completed" in line and t_arm is not None:
                done_at = time.time()
        gs.poll()
        if sent < len(sends) and time.time() >= sends[sent]:
            gs.ser.write(("HEX %s\r\n" % a.payload).encode())
            sent += 1
            gs.note("sent HEX #%d" % sent)
        if done_at and time.time() > done_at + 3:
            break
    sat.ser.close()
    gs.ser.close()
    if t_arm is None:
        print("no listen window seen on the satellite UART (wrong port / baud, board off, or CUBUS image without UART)")
        return 1
    c = summarize(os.path.join(a.out, "rx_sat.txt"))
    print("sent %d frame(s). satellite saw: %s" % (sent, ", ".join("%s=%d" % kv for kv in c.items())))
    if c["raw"] == 0:
        print("RESULT: the satellite radio completed no packet (no [STEP 1: READ RAW]). Check the GS log for the TX lines first.")
    elif c["decode_ok"] == 0:
        print("RESULT: packets arrived but none decoded (framing / alignment).")
    else:
        print("RESULT: the satellite received and decoded the frames.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
