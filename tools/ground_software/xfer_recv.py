#!/usr/bin/env python3
"""
xfer_recv.py -- receive a file sent with xfer_send.py, from Direwolf's KISS port.

Direwolf (tools/testbench/direwolf_s2s2_4800.conf) decodes the 4800 bps G3RUH downlink
from the SDR and exposes frames on KISSPORT 8001. This script reads them,
keeps the transfer fragments, writes the file once every fragment is in.

Usage:
  python3 tools/ground_software/xfer_recv.py --out received.bin --size <bytes> [--kiss 127.0.0.1:8001]
  python3 tools/ground_software/xfer_recv.py --out received.bin --size <bytes> --kiss-file capture.kiss
"""
import argparse
import hashlib
import os
import socket
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))
import file_xfer as fx  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--size", type=int, required=True, help="original file size in bytes")
    ap.add_argument("--kiss", default="127.0.0.1:8001")
    ap.add_argument("--kiss-file", help="read KISS bytes from a file instead of the socket")
    ap.add_argument("--seconds", type=float, default=3600.0)
    args = ap.parse_args()

    rx = fx.Reassembler()
    seen = 0
    bad = 0

    def feed(stream_bytes):
        nonlocal seen, bad
        for frame in fx.kiss_decode(stream_bytes):
            parsed = fx.parse_ui_frame(frame)
            if parsed is None:
                bad += 1
                continue
            if rx.add(parsed["info"]):
                seen += 1

    if args.kiss_file:
        feed(open(args.kiss_file, "rb").read())
    else:
        host, port = args.kiss.split(":")
        s = socket.create_connection((host, int(port)), timeout=1.0)
        end = time.time() + args.seconds
        last = time.time()
        while time.time() < end:
            try:
                chunk = s.recv(4096)
            except socket.timeout:
                chunk = b""
            if chunk:
                feed(chunk)
            if rx.total is not None and not rx.missing():
                break
            if time.time() - last > 30:
                last = time.time()
                print(f"fragments {len(rx.parts)}/{rx.total or '?'}, bad frames {bad}")

    if rx.total is None:
        print("no transfer fragments received")
        return 1
    missing = rx.missing()
    print(f"received {len(rx.parts)}/{rx.total} fragments, bad frames {bad}")
    if missing:
        print("missing fragments:", missing[:50], "..." if len(missing) > 50 else "")
        return 2
    data = rx.file_bytes(args.size)
    open(args.out, "wb").write(data)
    print("wrote", args.out, len(data), "bytes, sha256", hashlib.sha256(data).hexdigest())
    return 0


if __name__ == "__main__":
    sys.exit(main())
