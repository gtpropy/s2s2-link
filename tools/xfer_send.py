#!/usr/bin/env python3
"""
xfer_send.py -- stream a file to the GS UART in chunks, one fragment per chunk.

The file stays on this host (stands in for the satellite's external flash).
For each chunk the GS replies "XFER OK i/n" after the RF transmit finishes.

Usage:
  python3 tools/xfer_send.py <file> [--port /dev/cu.usbserial-AG0L702O]
                             [--image 1] [--baud 115200] [--dry-run]
--dry-run prints the first lines and sends nothing.
"""
import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import file_xfer as fx  # noqa: E402

CHUNK = fx.FRAG_DATA  # 112 bytes per chunk, matches the GS fragment size


def chunk_lines(data: bytes, image_id: int):
    """Return (start_line, [chunk_lines]) for the GS UART protocol."""
    total = (len(data) + CHUNK - 1) // CHUNK
    start = f"XFER START {image_id} {total}"
    lines = []
    for i in range(total):
        part = data[i * CHUNK:(i + 1) * CHUNK]
        lines.append(f"<HexFragmentStart>{part.hex().upper()}<HexFragmentEndFile>")
    return start, lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--port", default="/dev/cu.usbserial-AG0L702O")
    ap.add_argument("--image", type=int, default=1)
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--timeout", type=float, default=20.0)
    args = ap.parse_args()

    data = open(args.file, "rb").read()
    start, lines = chunk_lines(data, args.image)
    print(f"{args.file}: {len(data)} bytes -> {len(lines)} chunks ({CHUNK} bytes each)")

    if args.dry_run:
        print(start)
        for ln in lines[:2]:
            print(ln[:80] + "...")
        return 0

    import serial
    ser = serial.Serial(args.port, args.baud, timeout=0.2)

    def wait_for(prefixes, timeout):
        end = time.time() + timeout
        buf = b""
        while time.time() < end:
            buf += ser.read(256)
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode(errors="replace").strip()
                if any(text.startswith(p) for p in prefixes):
                    return text
        return None

    ser.write((start + "\r\n").encode())
    ack = wait_for(["XFER READY", "XFER ERR"], 5)
    print("GS:", ack)
    if not ack or not ack.startswith("XFER READY"):
        return 1

    t0 = time.time()
    for i, ln in enumerate(lines):
        ser.write((ln + "\r\n").encode())
        reply = wait_for(["XFER OK", "XFER ERR", "XFER TIMEOUT"], args.timeout)
        if not reply or not reply.startswith("XFER OK"):
            print(f"chunk {i}: {reply or 'no reply'} -- stopping")
            ser.write(b"XFER END\r\n")
            return 2
        if i % 50 == 0 or i == len(lines) - 1:
            print(f"{reply}  ({time.time() - t0:.0f} s)")
    done = wait_for(["XFER DONE"], 5)
    print("GS:", done)
    return 0


if __name__ == "__main__":
    sys.exit(main())
