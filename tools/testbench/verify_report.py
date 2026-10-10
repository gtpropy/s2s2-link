#!/usr/bin/env python3
"""Independent check of a docs/reports/<time>/ folder. Does NOT use the bench code.

  tools/.venv/bin/python tools/testbench/verify_report.py [docs/reports/<time>]   (default: newest)

It re-derives everything from the raw files:
  * SHA-256 of source.* and received.*  (and a byte compare)
  * rebuilds the file from the raw ground-station UART log (gs_uart.log) -- the {"type":"CAMERA",...}
    lines the GS firmware printed for every packet it decoded off the air
  * checks the satellite's own log (sat_vcp.log) for the OBC-flash steps
"""
import argparse
import glob
import hashlib
import json
import os
import re
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def sha(b):
    return hashlib.sha256(b).hexdigest()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir", nargs="?")
    a = ap.parse_args()
    d = a.dir or sorted(glob.glob(os.path.join(ROOT, "docs", "reports", "*/")))[-1]
    d = d.rstrip("/")
    print("report:", d)
    rep = json.load(open(os.path.join(d, "report.json")))
    img = rep["results"].get("image") or {}
    src = next(iter(glob.glob(os.path.join(d, "source.*"))), None)
    rx = next(iter(glob.glob(os.path.join(d, "received.*"))), None)
    ok = True
    if not (src and rx):
        print("FAIL: source/received file missing")
        return 1
    sb, rb = open(src, "rb").read(), open(rx, "rb").read()
    print(f"source   {os.path.basename(src):14s} {len(sb):7d} B  sha256 {sha(sb)}")
    print(f"received {os.path.basename(rx):14s} {len(rb):7d} B  sha256 {sha(rb)}")
    same = sb == rb
    print("  byte-identical:", same)
    ok &= same

    # --- rebuild from the raw GS UART log ---
    want_total = img.get("chunks") or ((len(sb) + 127) // 128)
    chunks, total = {}, want_total
    for line in open(os.path.join(d, "gs_uart.log"), errors="replace"):
        m = re.search(r'\{"type":"CAMERA","pkt":(\d+),"total":(\d+),"offset":(\d+),"len":(\d+),"hex":"([0-9A-Fa-f]*)"', line)
        if m and int(m.group(2)) == want_total:      # the GS log can also hold earlier runs with another size
            chunks[int(m.group(1))] = (int(m.group(3)), bytes.fromhex(m.group(5)))
    buf = bytearray(total * 128)
    for p, (off, data) in chunks.items():
        buf[off:off + len(data)] = data
    rebuilt = bytes(buf[:len(sb)])
    print(f"rebuilt from gs_uart.log: {len(chunks)}/{total} chunks, sha256 {sha(rebuilt)}  ==source: {rebuilt == sb}")
    ok &= rebuilt == sb and len(chunks) == total

    # --- flash evidence ---
    if img.get("flash_sha256"):
        print("flash read-back sha256:", img["flash_sha256"], " == source:", img["flash_sha256"] == sha(sb))
        ok &= img["flash_sha256"] == sha(sb)
    sat = os.path.join(d, "sat_vcp.log")
    if os.path.exists(sat):
        acks = [l for l in open(sat) if "ACK-FRAME" in l]
        kinds = {}
        for l in acks:
            m = re.search(r"cmd=(\d+) seq=\d+ status=(\d+)", l)
            if m:
                kinds.setdefault(int(m.group(1)), []).append(int(m.group(2)))
        names = {1: "CHUNK", 2: "STATUS", 3: "RESET", 4: "NEWFILE", 5: "STORE", 6: "SENDF(flash->RF)", 7: "READF", 8: "SETNAME", 9: "INFO"}
        for c, v in sorted(kinds.items()):
            print(f"  Nucleo answers {str(names.get(c, c)):18s} {len(v):4d}  (status 0 = OK: {v.count(0)})")
    leds = os.path.join(d, "led_trace.json")
    if os.path.exists(leds):
        tr = json.load(open(leds))
        seen = {c for _, c in tr}
        print("  LED GPIO trace (SWD, PB15/PB9/PB11):", len(tr), "transitions; colours seen:", sorted(seen))
    print("VERDICT:", "VERIFIED" if ok else "NOT VERIFIED")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
