"""tb.util"""

import hashlib
import os
import struct
import time

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))           # tools/testbench
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SAT_DIR = os.path.join(ROOT, "cpu2", "satellite")
GS_DIR = os.path.join(ROOT, "cpu2", "ground_station")
# build outputs of the two CPU2 images (objects, .elf, .bin, .map, .opts): <repo>/build/<image>/
SAT_BUILD = os.path.join(ROOT, "build", "satellite")
GS_BUILD = os.path.join(ROOT, "build", "ground_station")
REPORT_DIR = os.path.join(ROOT, "docs", "reports")
TOOLCHAIN = "/Applications/ArmGNUToolchain/15.3.rel1/arm-none-eabi/bin"

CHUNK = 128
GS_BUF_MAX = 10240                     # GS reassembly buffer (gs_cam_buf) -- only matters for "fit to GS"
OBC_SYNC = b"\xC5" * 8 + b"\xA7"
OBC_CHUNK, OBC_STATUS, OBC_RESET, OBC_ERASE, OBC_STORE, OBC_SENDF, OBC_READF, OBC_SETNAME, OBC_INFO = 1, 2, 3, 4, 5, 6, 7, 8, 9
OBC_RATE = 10        # raw air-rate test: seq = gap ms between packets, total = packet count
OBC_CWTEST = 11      # continuous CW "HELLO" for a spectrum analyzer: seq = seconds (0 = stop)
OBC_SAF = 12         # store-and-forward journal: seq 0 = status line, n = record n, 0xFFFF = empty the journal
OBC_INJECT = 13      # hand the satellite's digipeater a frame body as if it had been received over the air
FLASH_BYTES = 44 * 1024                # LittleFS on CPU1 flash pages 72..99 = 56 KB; leave room for copy-on-write
FLASH_CHUNKS = (FLASH_BYTES + 127) // 128

DEFAULTS = {
    "gs_port": "",                     # empty = first /dev/cu.usbserial-*
    "gs_baud": 115200,
    "sat_probe": "002D001B4741500920383733",   # only used for build/flash
    "gs_probe": "303030303030303030303031",
    "freq_mhz": 435.000,
    "sdr_gain": 40,
}


def now():
    return time.time()


def sha256(b):
    return hashlib.sha256(b).hexdigest()


# --------------------------------------------------------------------------------------
# Nucleo ST-Link VCP: OBC link frames + the satellite's own log
# --------------------------------------------------------------------------------------
def crc16_ccitt(d, c=0xFFFF):
    for b in d:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def obc_frame(cmd, seq=0, total=0, data=b""):
    """PC -> CPU1 frame: C5 x8, A7, cmd, seq(2 LE), total(2 LE), len, data, crc16(2 LE) over cmd..data."""
    body = bytes([cmd]) + struct.pack("<HH", seq, total) + bytes([len(data)]) + data
    return OBC_SYNC + body + struct.pack("<H", crc16_ccitt(body))
