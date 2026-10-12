#!/usr/bin/env python3
"""Talk to the store-and-forward digipeater through CPU1 (the Nucleo's ST-Link VCP, OBC link frames).

  tools/.venv/bin/python tools/console/saf_tool.py status
  tools/.venv/bin/python tools/console/saf_tool.py list
  tools/.venv/bin/python tools/console/saf_tool.py clear      # empty the journal, then reset CPU2
  tools/.venv/bin/python tools/console/saf_tool.py inject "K1ABC>APRS,ARISS:hello"     # as if heard on 437.375 MHz
Options: --port /dev/cu.usbmodemXXXX (default: the first one)

inject builds a UI frame from the TNC2 text, hands it to CPU2 through the shared-RAM mailbox and prints what the digipeater
decided (repeated and transmitted, or the reason it was dropped). The repeat goes out over the air on the downlink frequency.
Frames: PC->CPU1 C5 x8, A7, cmd, seq(2), total(2), len, data, crc16 (CRC-16/CCITT-FALSE); CPU1->PC A5 5A cmd seq(2) status 0D 0A,
data frames A5 5C seq(2) len data crc16 0D 0A in front of the answer.
"""
import argparse, glob, os, sys, time
import serial

REASONS = ["repeated", "disabled", "malformed", "not a UI frame", "no unused path entry", "next hop is not us / not an alias / not WIDE2-N",
           "our own source", "loop (our call already used)", "denied station", "duplicate", "hop limit", "too long for the radio packet",
           "global rate limit", "same station too soon"]


def crc16(data, crc=0xFFFF):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(cmd, seq=0, total=0, data=b""):
    body = bytes([cmd]) + seq.to_bytes(2, "little") + total.to_bytes(2, "little") + bytes([len(data)]) + data
    return b"\xC5" * 8 + b"\xA7" + body + crc16(body).to_bytes(2, "little")


def transact(ser, cmd, seq=0, total=0, data=b"", timeout=12.0):
    ser.reset_input_buffer()
    ser.write(frame(cmd, seq, total, data))
    buf, t0, datas = b"", time.time(), []
    while time.time() - t0 < timeout:
        buf += ser.read(256)
        while True:
            i = buf.find(b"\xA5\x5C")
            if i >= 0 and len(buf) >= i + 5 and len(buf) >= i + 5 + buf[i + 4] + 4:
                n = buf[i + 4]
                datas.append(buf[i + 5:i + 5 + n]); buf = buf[:i] + buf[i + 5 + n + 4:]
                continue
            break
        j = buf.find(b"\xA5\x5A")
        if j >= 0 and len(buf) >= j + 8:
            return buf[j + 5], datas
    return None, datas


def tnc2_to_body(text):
    """SRC>DEST,VIA1,VIA2*:info -> AX.25 body (no flags, no FCS)."""
    head, info = text.split(":", 1)
    src, rest = head.split(">", 1)
    parts = rest.split(",")
    star = max([i for i, p in enumerate(parts[1:]) if p.endswith("*")], default=-1)
    addrs = [parts[0]] + [p.rstrip("*") for p in parts[1:]]

    def enc(a, last, h=False):
        call, _, ss = a.partition("-")
        call = call.upper().ljust(6)
        return bytes(ord(c) << 1 for c in call) + bytes([0x60 | (int(ss or 0) << 1) | (0x80 if h else 0) | (1 if last else 0)])

    out = enc(addrs[0], False) + enc(src, not addrs[1:])
    for i, a in enumerate(addrs[1:]):
        out += enc(a, i == len(addrs) - 2, h=(i <= star))
    return out + b"\x03\xF0" + info.encode()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["status", "list", "inject", "clear"])
    ap.add_argument("text", nargs="?")
    ap.add_argument("--port")
    a = ap.parse_args()
    port = a.port or (sorted(glob.glob("/dev/cu.usbmodem*")) or [None])[0]
    if not port:
        sys.exit("no Nucleo VCP found (/dev/cu.usbmodem*)")
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 115200, 0.2
    ser.dtr = ser.rts = False
    ser.open()
    ser.dtr = ser.rts = False
    if a.cmd == "status":
        st, d = transact(ser, 0x0C, 0)
        print(d[0].decode(errors="replace") if d else "no answer", "| status", st)
    elif a.cmd == "clear":
        st, _ = transact(ser, 0x0C, 0xFFFF)
        print("journal emptied (reset CPU2 so it forgets its RAM copy)" if st == 0 else "failed, status 0x%02X" % (st or 0))
    elif a.cmd == "list":
        n = 1
        while True:
            st, d = transact(ser, 0x0C, n)
            if not d:
                break
            r = d[0]
            print("#%d id=%d len=%d hops_left=%d forwarded=%d ttl_left=%ds" % (n, r[0] | r[1] << 8, r[2], r[3], r[4], r[5] | r[6] << 8))
            print("   ", repr(bytes(r[7:]))[:150])
            n += 1
        print("%d stored message(s)" % (n - 1))
    else:
        body = tnc2_to_body(a.text)
        st, _ = transact(ser, 0x0D, 0, 0, body, timeout=60)
        if st is None:
            print("no answer from CPU1")
        elif st == 0:
            print("REPEATED:", a.text)
            time.sleep(3)                          # CPU2 reports the new record to CPU1's journal within a second or two
            st2, d2 = transact(ser, 0x0C, 0)
            print("JOURNAL:", d2[0].decode(errors="replace") if d2 else "no answer")
            n, total = 1, 0
            while True:
                st3, d3 = transact(ser, 0x0C, n)
                if not d3:
                    break
                r = d3[0]
                total += r[2]
                print("MEMORY OBJECTS: #%d id=%d len=%d hops_left=%d forwarded=%d ttl_left=%ds" % (n, r[0] | r[1] << 8, r[2], r[3], r[4], r[5] | r[6] << 8))
                print("    ", repr(bytes(r[7:])))
                n += 1
            print("STORED %d msg, %d B" % (n - 1, total))
        elif st >= 0x20 and st - 0x20 < len(REASONS):
            print("not repeated: %s" % REASONS[st - 0x20])
        else:
            print("status 0x%02X (0x07 = CPU2 did not answer, 0x02 = bad frame)" % st)


if __name__ == "__main__":
    main()
