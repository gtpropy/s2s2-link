#!/usr/bin/env python3
"""
file_xfer.py -- host-side file transfer over the S2S2 digipeater path.

Format: Packet Format Sheet 3 (camera fragment), with one change:
  Sheet 3 says 128 data bytes. The radio packet is 200 bytes and a 128-byte
  fragment does not fit with AX.25 framing, so FRAG_DATA = 112.

Fragment (Sheet 3, big-endian multi-byte fields):
  [0]      packet type      0x08
  [1]      image ID         file id (0-255)
  [2..3]   fragment index   uint16
  [4..5]   total fragments  uint16
  [6]      image type       0x00 RGB (file data)
  [7]      AI score         0
  [8..119] data             112 bytes (last fragment zero-padded)
  [120..121] CRC-16/X.25 over bytes [0..119], low byte first

AX.25 UI frame (plain, no FEC wrap) addressed to the far ground station, with
the satellite as the next digipeater hop, so the satellite repeats it.
"""
import struct

FRAG_TYPE = 0x08
FRAG_DATA = 112
FRAG_HDR = 8
FRAG_CRC = 2
FRAG_LEN = FRAG_HDR + FRAG_DATA + FRAG_CRC  # 122


def crc16_x25(data: bytes) -> int:
    """CRC-16/X.25 (reflected 0x1021, init 0xFFFF, xorout 0xFFFF), same as ax25.c."""
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0x8408 if crc & 1 else crc >> 1
    return (~crc) & 0xFFFF


def make_fragments(data: bytes, image_id: int):
    """Split a file into Sheet 3 fragments. Returns a list of 122-byte bytes."""
    if len(data) == 0:
        raise ValueError("empty file")
    total = (len(data) + FRAG_DATA - 1) // FRAG_DATA
    if total > 0xFFFF:
        raise ValueError("file too large for uint16 fragment count")
    frags = []
    for i in range(total):
        chunk = data[i * FRAG_DATA:(i + 1) * FRAG_DATA]
        chunk = chunk + b"\x00" * (FRAG_DATA - len(chunk))
        body = struct.pack(">BBHHBB", FRAG_TYPE, image_id & 0xFF, i, total, 0x00, 0x00) + chunk
        crc = crc16_x25(body)
        frags.append(body + bytes([crc & 0xFF, crc >> 8]))
    return frags


def parse_fragment(frag: bytes):
    """Check CRC and header. Returns (image_id, index, total, data) or None."""
    if len(frag) != FRAG_LEN or frag[0] != FRAG_TYPE:
        return None
    crc = frag[-2] | (frag[-1] << 8)
    if crc16_x25(frag[:-2]) != crc:
        return None
    _, image_id, index, total, _, _ = struct.unpack(">BBHHBB", frag[:FRAG_HDR])
    if index >= total:
        return None
    return image_id, index, total, frag[FRAG_HDR:FRAG_HDR + FRAG_DATA]


def ax25_address(call: str, ssid: int, last: bool) -> bytes:
    call = call.upper().ljust(6)[:6]
    out = bytes((ord(c) << 1) for c in call)
    ssid_byte = 0x60 | ((ssid & 0x0F) << 1) | (1 if last else 0)
    return out + bytes([ssid_byte])


def build_ui_frame(info: bytes, dest: str, dest_ssid: int, src: str, src_ssid: int,
                   digi: str, digi_ssid: int) -> bytes:
    """Clean AX.25 frame with flags, one digipeater hop (H-bit clear)."""
    body = bytearray()
    body += ax25_address(dest, dest_ssid, False)
    body += ax25_address(src, src_ssid, False)
    body += ax25_address(digi, digi_ssid, True)
    body += bytes([0x03, 0xF0]) + info
    crc = crc16_x25(bytes(body))   # CRC-16/X.25 == AX.25 FCS (already complemented)
    return bytes([0x7E]) + bytes(body) + bytes([crc & 0xFF, crc >> 8, 0x7E])


def parse_ui_frame(frame: bytes):
    """Parse a clean AX.25 frame (flags optional). Returns dict or None if FCS bad."""
    if frame[:1] == b"\x7e":
        frame = frame[1:]
    if frame[-1:] == b"\x7e":
        frame = frame[:-1]
    if len(frame) < 17:
        return None
    if crc16_x25(frame[:-2]) != (frame[-2] | (frame[-1] << 8)):
        return None
    def call(a):
        return "".join(chr(b >> 1) for b in a[:6]).rstrip(), (a[6] >> 1) & 0x0F
    dest, dssid = call(frame[0:7])
    src, sssid = call(frame[7:14])
    digis = []
    off = 14
    last = (frame[13] & 1) == 1
    while not last:
        c, s = call(frame[off:off + 7])
        digis.append((c, s, bool(frame[off + 6] & 0x80)))
        last = (frame[off + 6] & 1) == 1
        off += 7
    info = frame[off + 2:-2]
    return {"dest": (dest, dssid), "src": (src, sssid), "digis": digis, "info": info}


class Reassembler:
    def __init__(self):
        self.image_id = None
        self.total = None
        self.parts = {}

    def add(self, frag: bytes):
        p = parse_fragment(frag)
        if p is None:
            return False
        image_id, index, total, data = p
        if self.image_id is None:
            self.image_id, self.total = image_id, total
        elif (image_id, total) != (self.image_id, self.total):
            return False
        self.parts[index] = data
        return True

    def missing(self):
        if self.total is None:
            return []
        return [i for i in range(self.total) if i not in self.parts]

    def file_bytes(self, size: int):
        if self.missing():
            raise ValueError("transfer incomplete: %d fragments missing" % len(self.missing()))
        out = b"".join(self.parts[i] for i in range(self.total))
        return out[:size]


# ---- KISS (Direwolf AGW/KISS port 8001) ------------------------------------
KISS_FEND, KISS_FESC, KISS_TFEND, KISS_TFESC = 0xC0, 0xDB, 0xDC, 0xDD


def kiss_decode(stream: bytes):
    """Yield AX.25 frames (no flags, FCS kept) from a KISS byte stream."""
    buf = bytearray()
    esc = False
    for b in stream:
        if b == KISS_FEND:
            if len(buf) > 1:
                yield bytes(buf[1:])          # buf[0] is the port/command byte
            buf.clear()
            esc = False
        elif b == KISS_FESC:
            esc = True
        elif esc:
            buf.append(KISS_FEND if b == KISS_TFEND else KISS_FESC if b == KISS_TFESC else b)
            esc = False
        else:
            buf.append(b)


def kiss_encode(frame: bytes) -> bytes:
    """Wrap an AX.25 frame (no flags) for a KISS TNC port."""
    out = bytearray([KISS_FEND, 0x00])
    for b in frame:
        if b == KISS_FEND:
            out += bytes([KISS_FESC, KISS_TFEND])
        elif b == KISS_FESC:
            out += bytes([KISS_FESC, KISS_TFESC])
        else:
            out.append(b)
    out.append(KISS_FEND)
    return bytes(out)
