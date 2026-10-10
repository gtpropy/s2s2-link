#!/usr/bin/env python3
"""Proof that the image really sits in CPU1's internal flash (LittleFS pages 72..99).

  tools/.venv/bin/python tools/testbench/flash_proof.py <docs/reports/dir | source-file> [--probe SERIAL]

Reads the raw flash 0x08024000..0x08031FFF straight over SWD with OpenOCD (no firmware involved),
then looks for the source file's bytes in it. LittleFS interleaves block-pointers into file blocks,
so a chunk that straddles a block boundary will not appear contiguously; the check therefore looks up
a 32-byte window from the middle of every 128-byte chunk and reports how many were found.

Run it while the Nucleo is idle (the bench must not hold the ST-Link: close or stop the test first).
"""
import argparse
import glob
import hashlib
import os
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
BASE, PAGES, PAGE = 0x08024000, 28, 2048


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("target", nargs="?")
    ap.add_argument("--probe", default="002E00354741500120383733")
    a = ap.parse_args()
    t = a.target or sorted(glob.glob(os.path.join(ROOT, "docs", "reports", "*/")))[-1]
    if os.path.isdir(t):
        src = next(iter(glob.glob(os.path.join(t, "source.*"))))
    else:
        src = t
    data = open(src, "rb").read()
    print(f"source file: {src}  {len(data)} B  sha256 {hashlib.sha256(data).hexdigest()}")

    out = os.path.join(tempfile.mkdtemp(), "flash.bin")
    cmd = ["openocd", "-f", "interface/stlink.cfg", "-c", "transport select hla_swd",
           "-c", f"hla_serial {a.probe}", "-f", "target/stm32wlx.cfg", "-c", "reset_config none",
           "-c", "init", "-c", f"dump_image {out} 0x{BASE:08X} 0x{PAGES * PAGE:X}", "-c", "exit"]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    if not os.path.exists(out):
        print("could not read the flash over SWD:", r.stderr[-300:])
        return 1
    flash = open(out, "rb").read()
    used = sum(1 for p in range(PAGES) if flash[p * PAGE:(p + 1) * PAGE] != b"\xff" * PAGE)
    print(f"raw flash 0x{BASE:08X}..0x{BASE + PAGES * PAGE - 1:08X} dumped over SWD: {used}/{PAGES} pages programmed")

    chunks = (len(data) + 127) // 128
    found = 0
    where = []
    for i in range(chunks):
        c = data[i * 128:(i + 1) * 128]
        w = c[48:80] if len(c) >= 80 else c[:min(len(c), 32)]
        if len(w) >= 16:
            pos = flash.find(w)
            if pos >= 0:
                found += 1
                where.append(pos)
    print(f"{found}/{chunks} chunks of the file found verbatim in the raw flash"
          f"  (the rest straddle LittleFS block-pointer headers)")
    if where:
        print(f"first match at 0x{BASE + min(where):08X}, last at 0x{BASE + max(where):08X}")
    print("JPEG/PNG magic at start of a flash block:",
          [hex(BASE + p * PAGE) for p in range(PAGES) if flash[p * PAGE + 4:p * PAGE + 8] in (data[:4],)][:3] or "n/a (LittleFS keeps pointers first)")
    ok = chunks and found / chunks > 0.8
    print("VERDICT:", "the file is physically stored in CPU1's internal flash" if ok else "NOT FOUND in flash")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
