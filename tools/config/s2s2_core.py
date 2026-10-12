#!/usr/bin/env python3
"""
s2s2_core.py -- build / flash / memory-statistics logic shared by the Textual TUI
(s2s2_tui.py) and the batch modes. No UI code here; standard library only.

Configuration is stored in <repo>/.s2s2.config (JSON) and is the single source of truth, like NuttX's .config:
`generate()` turns it into <repo>/generated/{s2s2_config.h, s2s2.mk, secrets.h}, which both firmware Makefiles use
(`make configure`, or save in the TUI). secrets.h holds the random AUTH key shared by satellite and ground station; it is
created once (when missing) and replaced only on request ("new key"). generated/ is git-ignored.
"""
import hashlib
import json
import os
import re
import secrets as _secrets
import shutil
import subprocess
import sys
import threading
import time
from collections import deque

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
CONFIG_PATH = os.path.join(REPO, ".s2s2.config")

# CPU2 memory map (scripts/wl55jc_cm0p.ld)
CPU2_FLASH_BASE, CPU2_FLASH_SIZE = 0x08032000, 56 * 1024
CPU2_RAM_BASE, CPU2_RAM_SIZE = 0x20009000, 28 * 1024
CPU1_FLASH_SIZE = 0x32000                       # 0x08000000 .. 0x08032000
HEAP_STACK_RESERVE = 0x200 + 0x400              # _Min_Heap_Size + _Min_Stack_Size

# "bin" / "elf" are relative to the repository root: every image builds into <repo>/build/<image>/ (see cpu2/common/cpu2.mk)
TARGETS = {
    "sat": {"name": "Satellite", "dir": os.path.join(REPO, "cpu2", "satellite"),
            "bin": os.path.join("build", "satellite", "satellite.bin"), "elf": os.path.join("build", "satellite", "satellite.elf")},
    "gs": {"name": "Ground station", "dir": os.path.join(REPO, "cpu2", "ground_station"),
           "bin": os.path.join("build", "ground_station", "ground_station.bin"), "elf": os.path.join("build", "ground_station", "ground_station.elf")},
}
NUTTX_BIN = os.path.join(REPO, "nuttx", "nuttx.bin")

GEN_DIR = os.path.join(REPO, "generated")

# Every option of the project. Mission values end up as #defines in generated/s2s2_config.h, build options as make
# variables in generated/s2s2.mk. "sat_serial"/"gs_serial" (ST-Link probes) are only used by the flash steps.
DEFAULT_CONFIG = {
    # targets
    "build_sat": True, "build_gs": True,
    # security
    "auth": True,                      # integrity tag + replay counter on short packets (key in generated/secrets.h)
    # build
    "debug": False, "frontend": "NUCLEO", "cubus": False, "log": True, "timer_rtc": False,
    # identity
    "project": "S2S2", "sat_call": "9N2SJ", "sat_ssid": 1, "gs_call": "GROUND", "gs_ssid": 0,
    # radio and timing
    "downlink_hz": 435000000, "uplink_hz": 437375000, "cw_unit_ms": 60, "beacon_period_s": 90,
    # digipeater (satellite): repeats other stations' AX.25 UI frames, see docs/DIGIPEATER.md
    "digi": True, "digi_alias": "ARISS", "digi_wide": True, "digi_max_n": 2, "digi_trace": True,
    "digi_dedupe_s": 30, "digi_rate_per_min": 6, "digi_burst": 2, "digi_src_min_s": 10,
    # store-and-forward: keep repeated frames (journal in CPU1 flash) and send them again later, hop budget from the WIDEn-N path
    "saf": True, "saf_immediate": True, "saf_append_stats": False, "saf_max_fwd": 3, "saf_fwd_interval_s": 900, "saf_ttl_min": 360,
    # probes
    "sat_serial": "", "gs_serial": "",
}

# option metadata for the TUI / validation: key -> (label, help)
FRONTENDS = ("NUCLEO", "CUSTOM")


def validate_config(cfg):
    """Return a list of problems (empty = ok)."""
    errs = []

    def call(key, label):
        v = str(cfg[key])
        if not re.fullmatch(r"[A-Z0-9]{1,6}", v):
            errs.append("%s must be 1-6 characters A-Z / 0-9" % label)

    call("sat_call", "satellite call sign")
    call("gs_call", "ground station call sign")
    if not re.fullmatch(r"[A-Z0-9]{1,6}", str(cfg["project"])):
        errs.append("project name must be 1-6 characters A-Z / 0-9")
    for key, label in (("sat_ssid", "satellite SSID"), ("gs_ssid", "ground station SSID")):
        if not (isinstance(cfg[key], int) and 0 <= cfg[key] <= 15):
            errs.append("%s must be 0-15" % label)
    for key, label in (("downlink_hz", "downlink frequency"), ("uplink_hz", "uplink frequency")):
        if not (isinstance(cfg[key], int) and 400_000_000 <= cfg[key] <= 960_000_000):
            errs.append("%s must be 400000000-960000000 Hz" % label)
    if not (isinstance(cfg["cw_unit_ms"], int) and 20 <= cfg["cw_unit_ms"] <= 200):
        errs.append("CW unit must be 20-200 ms")
    if not (isinstance(cfg["beacon_period_s"], int) and 30 <= cfg["beacon_period_s"] <= 600):
        errs.append("beacon period must be 30-600 s")
    if cfg["digi_alias"] and not re.fullmatch(r"[A-Z0-9]{1,6}", str(cfg["digi_alias"])):
        errs.append("digipeater alias must be empty or 1-6 characters A-Z / 0-9")
    for key, label, lo, hi in (("digi_max_n", "digipeater WIDEn-N limit", 1, 7), ("digi_dedupe_s", "digipeater duplicate window s", 0, 600),
                               ("digi_rate_per_min", "digipeater repeats per minute", 0, 60), ("digi_burst", "digipeater burst", 1, 8),
                               ("digi_src_min_s", "digipeater per-station interval s", 0, 300)):
        if not (isinstance(cfg[key], int) and lo <= cfg[key] <= hi):
            errs.append("%s must be %d-%d" % (label, lo, hi))
    for key, label, lo, hi in (("saf_max_fwd", "store-and-forward sends per message", 1, 8),
                               ("saf_fwd_interval_s", "store-and-forward interval s", 10, 86400),
                               ("saf_ttl_min", "store-and-forward lifetime min", 1, 1440)):
        if not (isinstance(cfg[key], int) and lo <= cfg[key] <= hi):
            errs.append("%s must be %d-%d" % (label, lo, hi))
    if cfg["frontend"] not in FRONTENDS:
        errs.append("front end must be one of %s" % ", ".join(FRONTENDS))
    if not isinstance(cfg["cubus"], bool):
        errs.append("CUBUS must be on or off")
    return errs


# --------------------------------------------------------------------------
# menu tree of the configuration (what the TUI shows, like NuttX's menuconfig)
#   kind: menu (submenu) | bool | int | str | choice | probe | action | info
#   dep : list of config keys that must be true for the item to be shown
# --------------------------------------------------------------------------
def _i(kind, key, label, help="", **kw):
    d = {"kind": kind, "key": key, "label": label, "help": help}
    d.update(kw)
    return d


MENUS = {
    "main": ("S2S2 configuration", [
        _i("menu", "targets", "Targets", "Which firmware images are built and flashed."),
        _i("menu", "security", "Security: AUTH and the shared key", "Integrity check of short packets and the random key both images share."),
        _i("menu", "build", "Build options", "Optimisation, logging, RF front end."),
        _i("menu", "identity", "Identity: project name, call signs, SSIDs", "Call signs used in every AX.25 packet and in the CW beacon."),
        _i("menu", "radio", "Radio frequencies, CW and beacon timing", "Downlink / uplink frequency and the Morse beacon."),
        _i("menu", "digi", "Digipeater and store and forward", "The satellite repeats other stations' UI frames, keeps them and sends them again."),
        _i("menu", "probes", "Programmer: ST-Link probes", "Which probe flashes which board."),
        _i("menu", "actions", "Build, flash, clean, memory", "Run the build and flash steps with the configuration above."),
    ]),
    "targets": ("Targets", [
        _i("bool", "build_sat", "Satellite image (cpu2/satellite)", "Build and flash the satellite's CPU2 firmware."),
        _i("bool", "build_gs", "Ground station image (cpu2/ground_station)", "Build and flash the ground station's CPU2 firmware."),
    ]),
    "security": ("Security", [
        _i("bool", "auth", "AUTH: integrity tag + replay counter on short packets",
           "Every payload up to 107 bytes gets a 12-byte trailer (counter + tag). Both ends must use the same setting and the same key. "
           "Off: plain Packet-Format packets."),
        _i("info", "_key", "AUTH key"),
        _i("action", "newkey", "<New AUTH key>  (rebuild and reflash BOTH boards afterwards)",
           "Replaces the random 16-byte key in generated/secrets.h. The key is created automatically the first time and never committed."),
    ]),
    "build": ("Build options", [
        _i("bool", "debug", "DEBUG: -Og -g3 instead of -Os", "For stepping with a debugger. The satellite does not fit 56 KB with it."),
        _i("bool", "log", "LOG: satellite console log", "Off removes the log (about 15 KB) but is broken: downlink frames get corrupted (open bug)."),
        _i("bool", "timer_rtc", "RTC-backed UTIL_TIMER tick", "Dormant ST timer layer (about 1.5 KB). Leave off."),
        _i("bool", "cubus", "CUBUS: build the satellite for the RSMS CUBUS V.3 flight board",
           "On: the satellite image uses the CUBUS board profile (ADL5324 PA fed from RFO_LP, 3.3 V for CW / 5 V for GMSK, RF switch FE_CTRL1/2, the 5 V bus rail only "
           "borrowed on PA0, no Nucleo LEDs on PB15/PB9/PB11, no PB6 UART) and replaces the front end setting below. The ground station is always a Nucleo. "
           "Same as make CUBUS=T. See docs/CUBUS.md."),
        _i("choice", "frontend", "Satellite RF front end (ignored with CUBUS)", "NUCLEO = stock Nucleo-WL55JC (RFO_HP PA). CUSTOM = the reference board (RFO_LP + external 3.3 V / 5 V PAs).",
           options=FRONTENDS),
    ]),
    "identity": ("Identity", [
        _i("str", "project", "Project name (CW prefix)", "1-6 characters A-Z / 0-9. The CW beacon sends <project> <satellite call sign>.", upper=True),
        _i("str", "sat_call", "Satellite call sign", "1-6 characters A-Z / 0-9.", upper=True),
        _i("int", "sat_ssid", "Satellite SSID", "0-15.", lo=0, hi=15),
        _i("str", "gs_call", "Ground station call sign", "1-6 characters A-Z / 0-9.", upper=True),
        _i("int", "gs_ssid", "Ground station SSID", "0-15.", lo=0, hi=15),
    ]),
    "radio": ("Radio and timing", [
        _i("int", "downlink_hz", "Downlink frequency, Hz (satellite -> ground)", "400000000-960000000. Default 435000000.", lo=400_000_000, hi=960_000_000),
        _i("int", "uplink_hz", "Uplink frequency, Hz (ground -> satellite)", "400000000-960000000. Default 437375000.", lo=400_000_000, hi=960_000_000),
        _i("int", "cw_unit_ms", "CW dit length, ms", "20-200. The ground station decodes with the same value (60 ms = 20 WPM).", lo=20, hi=200),
        _i("int", "beacon_period_s", "CW beacon period, s", "30-600. The beacon starts this often, also during a transfer.", lo=30, hi=600),
    ]),
    "digi": ("Digipeater and store and forward", [
        _i("bool", "digi", "DIGI: repeat other stations' AX.25 UI frames",
           "Frames with a digipeater path and a good FCS are judged by the digipeater instead of the command logic. Costs about 5 KB of flash."),
        _i("str", "digi_alias", "Alias besides our call sign", "ARISS or empty. 1-6 characters A-Z / 0-9.", upper=True, dep=["digi"]),
        _i("bool", "digi_wide", "Answer WIDE2-N paths", "WIDE2 is the wide-area tier a satellite belongs to; WIDE1-1 is for ground fill-in digipeaters.", dep=["digi"]),
        _i("bool", "digi_trace", "Trace: put our call sign into WIDE2-N paths", "New-N trace: NODE*,WIDE2-1.", dep=["digi"]),
        _i("int", "digi_max_n", "Highest WIDE2-N accepted", "1-7. Flooding control: a satellite footprint is huge, so keep it at 2.", lo=1, hi=7, dep=["digi"]),
        _i("int", "digi_dedupe_s", "Duplicate window, s", "0-600. Same source + destination + info is not repeated again inside this time (0 = off).", lo=0, hi=600, dep=["digi"]),
        _i("int", "digi_rate_per_min", "Repeats per minute", "0-60 (0 = unlimited). Power and heat budget of the 5 V PA.", lo=0, hi=60, dep=["digi"]),
        _i("int", "digi_burst", "Burst (token bucket size)", "1-8.", lo=1, hi=8, dep=["digi"]),
        _i("int", "digi_src_min_s", "Minimum seconds between two repeats of one station", "0-300.", lo=0, hi=300, dep=["digi"]),
        _i("bool", "saf", "SAF: keep repeated frames, send them again later",
           "Store and forward. The messages are kept in RAM and journaled in CPU1's flash (they survive a reset). Hop budget = the WIDEn-N of the path.", dep=["digi"]),
        _i("bool", "saf_immediate", "Also repeat right away", "Off: a message is only sent from the store.", dep=["digi", "saf"]),
        _i("int", "saf_max_fwd", "Sends per stored message", "1-8.", lo=1, hi=8, dep=["digi", "saf"]),
        _i("int", "saf_fwd_interval_s", "Seconds between two sends of one message", "10-86400. 900 s is about a quarter of an orbit.", lo=10, hi=86400, dep=["digi", "saf"]),
        _i("int", "saf_ttl_min", "Lifetime of a stored message, min", "1-1440.", lo=1, hi=1440, dep=["digi", "saf"]),
        _i("bool", "saf_append_stats", "TEST: append the store statistics to the repeated text",
           "Adds ' | MEMORY OBJECTS: ... | STORED n msg, N B' to the text of every frame repeated right away. Alters other stations' text: tests only.", dep=["digi", "saf"]),
    ]),
    "probes": ("ST-Link probes", [
        _i("probe", "sat_serial", "Satellite probe", "ST-Link that flashes the satellite board."),
        _i("probe", "gs_serial", "Ground station probe", "ST-Link that flashes the ground station board."),
        _i("action", "rescan", "<Rescan probes>", "Read the connected ST-Links from the USB registry (the probes are not opened)."),
    ]),
    "actions": ("Build, flash and tools", [
        _i("action", "build", "<Build the selected targets>", "make in cpu2/satellite and cpu2/ground_station."),
        _i("action", "flash", "<Flash CPU2 of the selected targets>", "Needs a probe for every selected target. CPU1 is preserved."),
        _i("action", "flash_all", "<Flash CPU1 (NuttX) + boot option bytes + CPU2>", "Needs nuttx/nuttx.bin (make nuttx)."),
        _i("action", "clean", "<Clean>", "Remove the build directories."),
        _i("action", "memory", "<Show the memory report>", "Flash and RAM bars, section sizes, largest symbols."),
        _i("action", "gen", "<Generate generated/ now>", "Rewrite s2s2_config.h, s2s2.mk and (if missing) secrets.h."),
        _i("action", "defconfig", "<Reset all options to the defaults>", "Keeps the ST-Link choice."),
    ]),
}


def item_visible(it, cfg):
    return all(cfg.get(k) for k in it.get("dep", ()))


# --------------------------------------------------------------------------
# config
# --------------------------------------------------------------------------
def load_config():
    cfg = dict(DEFAULT_CONFIG)
    try:
        with open(CONFIG_PATH) as f:
            cfg.update({k: v for k, v in json.load(f).items() if k in DEFAULT_CONFIG})
    except (OSError, ValueError):
        pass
    return cfg


def save_config(cfg):
    with open(CONFIG_PATH, "w") as f:
        json.dump(cfg, f, indent=2, sort_keys=True)
        f.write("\n")


# --------------------------------------------------------------------------
# generated files: secrets.h (AUTH key), s2s2_config.h, s2s2.mk
# --------------------------------------------------------------------------
SECRETS_H = os.path.join(GEN_DIR, "secrets.h")
CONFIG_H = os.path.join(GEN_DIR, "s2s2_config.h")
CONFIG_MK = os.path.join(GEN_DIR, "s2s2.mk")


def _write_if_changed(path, text):
    """Write only when the content differs, so make does not rebuild everything for nothing. True if written."""
    try:
        with open(path) as f:
            if f.read() == text:
                return False
    except OSError:
        pass
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(text)
    return True


def key_fingerprint(key):
    return hashlib.sha256(key).hexdigest()[:8]


def read_key():
    """Return the 16 key bytes of generated/secrets.h, or None."""
    try:
        with open(SECRETS_H) as f:
            m = re.search(r"S2S2_AUTH_KEY_BYTES\s*\{([^}]*)\}", f.read())
        key = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
        return key if len(key) == 16 else None
    except (OSError, AttributeError, ValueError):
        return None


def render_secrets(key):
    body = ", ".join("0x%02X" % b for b in key)
    return """/* GENERATED by tools/s2s2_core.py (make configure / the TUI) -- DO NOT EDIT, DO NOT COMMIT (git-ignored).
 * Shared integrity key (AUTH): the satellite and the ground station MUST be built from the same file.
 * New key: `make newkey` or the TUI button, then rebuild and reflash BOTH boards. */
#ifndef S2S2_SECRETS_H
#define S2S2_SECRETS_H
#define S2S2_AUTH_KEY_BYTES { %s }
#define S2S2_AUTH_KEY_FPR   "%s"      /* first 8 hex digits of SHA-256(key): compare it on both boards' boot message */
#endif
""" % (body, key_fingerprint(key))


def c_str(v):
    return '"' + str(v).replace("\\", "\\\\").replace('"', '\\"') + '"'


def render_config_h(cfg):
    return """/* GENERATED by tools/s2s2_core.py from .s2s2.config (make configure / the TUI) -- DO NOT EDIT (git-ignored).
 * Mission parameters shared by the satellite and the ground station. */
#ifndef S2S2_CONFIG_H
#define S2S2_CONFIG_H

#define S2S2_PROJECT_NAME        %(project)s
#define S2S2_SAT_CALLSIGN        %(sat_call)s
#define S2S2_SAT_SSID            %(sat_ssid)d
#define S2S2_GS_CALLSIGN         %(gs_call)s
#define S2S2_GS_SSID             %(gs_ssid)d
#define S2S2_CW_CALLSIGN         %(cw_call)s     /* sent in Morse in front of the housekeeping bytes */

#define S2S2_DOWNLINK_HZ         %(downlink_hz)dUL   /* satellite -> ground */
#define S2S2_UPLINK_HZ           %(uplink_hz)dUL   /* ground -> satellite */

#define S2S2_CW_UNIT_MS          %(cw_unit_ms)dU         /* Morse dit length; the GS decoder uses the same value */
#define S2S2_BEACON_PERIOD_MS    %(beacon_period_s)d000UL  /* CW beacon starts every this long */

/* satellite digipeater (docs/DIGIPEATER.md) */
#define S2S2_DIGI_ENABLE         %(digi_on)d
#define S2S2_DIGI_ALIAS          %(digi_alias)s     /* generic alias answered besides our call sign, "" = none */
#define S2S2_DIGI_WIDE           %(digi_wide_on)d         /* answer WIDEn-N paths of the tier 2 */
#define S2S2_DIGI_MAX_N          %(digi_max_n)d         /* highest N accepted (flooding control) */
#define S2S2_DIGI_TRACE          %(digi_trace_on)d         /* insert our call sign into WIDEn-N paths */
#define S2S2_DIGI_DEDUPE_MS      %(digi_dedupe_s)d000UL  /* same source + destination + info is not repeated again inside this window */
#define S2S2_DIGI_RATE_PER_MIN   %(digi_rate_per_min)d         /* sustained repeats per minute, 0 = unlimited */
#define S2S2_DIGI_BURST          %(digi_burst)d         /* token bucket size */
#define S2S2_DIGI_SRC_MIN_MS     %(digi_src_min_s)d000UL  /* minimum time between two repeats of the same station */

/* store-and-forward (needs the digipeater): accepted frames are kept and sent again, journal in CPU1 flash */
#define S2S2_SAF_ENABLE          %(saf_on)d
#define S2S2_SAF_IMMEDIATE       %(saf_immediate_on)d         /* also repeat right away (0 = only send from the store) */
#define S2S2_SAF_APPEND_STATS    %(saf_append_on)d         /* TEST: append the store statistics to the text of every digipeated message */
#define S2S2_SAF_MAX_FORWARDS    %(saf_max_fwd)d         /* sends of one stored message */
#define S2S2_SAF_INTERVAL_MS     %(saf_fwd_interval_s)d000UL  /* between two sends of the same message */
#define S2S2_SAF_TTL_MS          (%(saf_ttl_min)d0000UL * 6UL)   /* lifetime: minutes * 60000 */

#endif
""" % dict(cfg, project=c_str(cfg["project"]), sat_call=c_str(cfg["sat_call"]), gs_call=c_str(cfg["gs_call"]),
           cw_call=c_str("%s %s" % (cfg["project"], cfg["sat_call"])),
           digi_on=int(cfg["digi"]), digi_alias=c_str(cfg["digi_alias"]), digi_wide_on=int(cfg["digi_wide"]),
           digi_trace_on=int(cfg["digi_trace"]), saf_on=int(cfg["saf"]), saf_immediate_on=int(cfg["saf_immediate"]), saf_append_on=int(cfg["saf_append_stats"]))


def render_mk(cfg):
    return """# GENERATED by tools/s2s2_core.py from .s2s2.config -- DO NOT EDIT (git-ignored). Command-line values override.
AUTH             ?= %s
DEBUG            ?= %s
FRONTEND         ?= %s
CUBUS            ?= %s
LOG              ?= %s
TIMER_IF_USE_RTC ?= %s
DIGI             ?= %s
""" % ("TRUE" if cfg["auth"] else "FALSE", "T" if cfg["debug"] else "F", cfg["frontend"], "TRUE" if cfg["cubus"] else "FALSE",
       "1" if cfg["log"] else "0", "1" if cfg["timer_rtc"] else "0", "TRUE" if cfg["digi"] else "FALSE")


def generate(cfg=None, new_key=False):
    """Create / refresh the generated files. secrets.h is only created when missing (or when new_key is set).
    Returns the list of files that changed."""
    cfg = cfg or load_config()
    errs = validate_config(cfg)
    if errs:
        raise ValueError("; ".join(errs))
    changed = []
    key = None if new_key else read_key()
    if key is None:
        key = _secrets.token_bytes(16)
    for path, text in ((SECRETS_H, render_secrets(key)), (CONFIG_H, render_config_h(cfg)), (CONFIG_MK, render_mk(cfg))):
        if _write_if_changed(path, text):
            changed.append(os.path.relpath(path, REPO))
    return changed


# --------------------------------------------------------------------------
# toolchain / probes
# --------------------------------------------------------------------------
def toolchain_prefix():
    env = os.environ.get("ARM_TOOLCHAIN_BIN", "")
    if env:
        return env.rstrip("/") + "/"
    import glob
    hits = sorted(glob.glob("/Applications/ArmGNUToolchain/*/arm-none-eabi/bin"))
    return (hits[-1] + "/") if hits else ""


def tool(name):
    p = toolchain_prefix() + "arm-none-eabi-" + name
    return p if os.path.exists(p) else shutil.which("arm-none-eabi-" + name) or p


def parse_probes(text):
    """Parse `st-info --probe` output into [{'serial','version','chip'}]."""
    probes, cur = [], None
    for line in text.splitlines():
        m = re.match(r"\s*\d+\.\s*$", line)
        if m:
            cur = {"serial": "", "version": "", "chip": ""}
            probes.append(cur)
            continue
        if cur is None:
            cur = None
        kv = re.match(r"\s*(\w[\w-]*):\s*(.+?)\s*$", line)
        if kv and probes:
            k, v = kv.group(1), kv.group(2)
            if k == "serial":
                probes[-1]["serial"] = v
            elif k == "version":
                probes[-1]["version"] = v
            elif k == "dev-type":
                probes[-1]["chip"] = v
    # single-probe output has no "1." header
    if not probes:
        s = re.search(r"serial:\s*(\S+)", text)
        if s:
            v = re.search(r"version:\s*(\S+)", text)
            probes.append({"serial": s.group(1), "version": v.group(1) if v else "", "chip": ""})
    return [p for p in probes if p["serial"]]


def parse_ioreg_probes(text):
    """ST-Link probes from `ioreg -p IOUSB -l -w0` output: [{'serial','version','chip'}] (version / chip are not known without
    talking to the probe, they stay empty)."""
    probes, name = [], None
    for line in text.splitlines():
        m = re.search(r'"USB Product Name" = "([^"]*)"', line)
        if m:
            name = m.group(1)
            continue
        m = re.search(r'"USB Serial Number" = "([^"]*)"', line)
        if m and name is not None:
            if "stlink" in name.lower().replace("-", "").replace("_", "").replace(" ", "") and m.group(1):
                probes.append({"serial": m.group(1), "version": "", "chip": ""})
            name = None
    return probes


def scan_probes():
    """List the connected ST-Links by reading the USB registry. It never talks to a probe: `st-info --probe` opens every ST-Link and
    RESETS the board behind it, which is not acceptable with a flight board attached (CUBUS) or in the middle of a test."""
    try:
        out = subprocess.run(["ioreg", "-p", "IOUSB", "-l", "-w0"], capture_output=True, text=True, timeout=15)
        return parse_ioreg_probes(out.stdout)
    except (OSError, subprocess.TimeoutExpired):
        return []


# --------------------------------------------------------------------------
# memory statistics
# --------------------------------------------------------------------------
def parse_size_sections(text):
    """Parse `arm-none-eabi-size -A` output into {section: bytes}."""
    secs = {}
    for line in text.splitlines():
        m = re.match(r"^(\.[\w.]+)\s+(\d+)\s+(0x[0-9a-fA-F]+|\d+)", line)
        if m:
            secs[m.group(1)] = int(m.group(2))
    return secs


def top_symbols(elf, count=8):
    try:
        out = subprocess.run([tool("nm"), "--size-sort", "-S", "-C", elf],
                             capture_output=True, text=True, timeout=20).stdout
    except (OSError, subprocess.TimeoutExpired):
        return []
    syms = []
    for line in out.splitlines():
        p = line.split(None, 3)
        if len(p) == 4 and p[2] in "TtRrDdBb" and p[3] not in ("end", "_end", "_estack"):
            try:
                addr = int(p[0], 16)
            except ValueError:
                continue
            in_flash = 0x08000000 <= addr < 0x08040000
            in_ram = 0x20000000 <= addr < 0x20010000
            if in_flash or in_ram:
                syms.append((int(p[1], 16), p[3]))
    syms.sort(reverse=True)
    return syms[:count]


def target_stats(key):
    t = TARGETS[key]
    binp, elfp = os.path.join(REPO, t["bin"]), os.path.join(REPO, t["elf"])
    st = {"name": t["name"], "built": os.path.exists(binp) and os.path.exists(elfp),
          "bin": t["bin"], "bin_bytes": 0, "sections": {}, "top": [], "age": None}
    if not st["built"]:
        return st
    st["bin_bytes"] = os.path.getsize(binp)
    st["age"] = time.time() - os.path.getmtime(binp)
    try:
        out = subprocess.run([tool("size"), "-A", elfp], capture_output=True, text=True, timeout=20).stdout
        st["sections"] = parse_size_sections(out)
    except (OSError, subprocess.TimeoutExpired):
        pass
    st["top"] = top_symbols(elfp)
    return st


def bar(frac, width=28):
    frac = max(0.0, min(1.0, frac))
    n = int(round(frac * width))
    return "[" + "#" * n + "." * (width - n) + "]"


def format_stats(st):
    """Return the memory report for one target as a list of text lines."""
    L = []
    if not st["built"]:
        return ["%s: not built yet (Build selected targets)" % st["name"]]
    sec = st["sections"]
    data = sec.get(".data", 0)
    bss = sec.get(".bss", 0)
    binb = st["bin_bytes"]
    ram = data + bss
    L.append("%s  %s" % (st["name"], st["bin"]))
    L.append(" FLASH %s  .bin [%s B] of %s B  %.1f%%  free %s B" %
             (bar(binb / CPU2_FLASH_SIZE), format(binb, ","), format(CPU2_FLASH_SIZE, ","),
              100.0 * binb / CPU2_FLASH_SIZE, format(CPU2_FLASH_SIZE - binb, ",")))
    L.append("   .isr_vector %s B   .text(+rodata) %s B   .data(load) %s B" %
             (format(sec.get(".isr_vector", 0), ","), format(sec.get(".text", 0), ","), format(data, ",")))
    L.append(" RAM   %s  static [%s B] of %s B  %.1f%%" %
             (bar((ram + HEAP_STACK_RESERVE) / CPU2_RAM_SIZE), format(ram, ","),
              format(CPU2_RAM_SIZE, ","), 100.0 * (ram + HEAP_STACK_RESERVE) / CPU2_RAM_SIZE))
    L.append("   .data %s B   .bss %s B   heap+stack reserve %s B   free %s B" %
             (format(data, ","), format(bss, ","), format(HEAP_STACK_RESERVE, ","),
              format(CPU2_RAM_SIZE - ram - HEAP_STACK_RESERVE, ",")))
    if st["top"]:
        L.append(" largest symbols:")
        for sz, name in st["top"]:
            L.append("   %6d B  %s" % (sz, name[:48]))
    if st["age"] is not None and st["age"] > 3600:
        L.append(" (binary is %.0f h old)" % (st["age"] / 3600))
    return L


def nuttx_line():
    if not os.path.exists(NUTTX_BIN):
        return "CPU1 NuttX: nuttx/nuttx.bin not built"
    b = os.path.getsize(NUTTX_BIN)
    return "CPU1 NuttX nuttx.bin [%s B] of %s B  %.1f%%  %s" % (
        format(b, ","), format(CPU1_FLASH_SIZE, ","), 100.0 * b / CPU1_FLASH_SIZE, bar(b / CPU1_FLASH_SIZE, 20))


# --------------------------------------------------------------------------
# command runner
# --------------------------------------------------------------------------
class Runner:
    def __init__(self):
        self.lines = deque(maxlen=4000)
        self.proc = None
        self.title = ""
        self.rc = None
        self.running = False
        self.lock = threading.Lock()

    @property
    def busy(self):
        return self.running

    def run_steps(self, title, steps, on_done=None):
        """steps: list of (label, argv, cwd). Stops at the first failure."""
        if self.running:
            return False
        self.running = True
        self.title, self.rc = title, None
        self.lines.clear()

        def work():
            env = dict(os.environ)
            pre = toolchain_prefix()
            if pre:
                env["PATH"] = pre + ":" + env.get("PATH", "")
            for k in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
                env.pop(k, None)
            for label, argv, cwd in steps:
                with self.lock:
                    self.lines.append("=== %s" % label)
                    self.lines.append("$ " + " ".join(argv))
                try:
                    self.proc = subprocess.Popen(argv, cwd=cwd, env=env, stdout=subprocess.PIPE,
                                                 stderr=subprocess.STDOUT, text=True, errors="replace")
                    for ln in self.proc.stdout:
                        ln = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", ln.rstrip("\n"))
                        if ln.strip():
                            with self.lock:
                                self.lines.append(ln)
                    self.proc.wait()
                    rc = self.proc.returncode
                except OSError as e:
                    with self.lock:
                        self.lines.append("ERROR: %s" % e)
                    rc = 127
                if rc != 0:
                    with self.lock:
                        self.lines.append("*** FAILED (exit %d): %s" % (rc, label))
                    self.rc = rc
                    break
            else:
                self.rc = 0
                with self.lock:
                    self.lines.append("*** DONE: %s" % title)
            self.proc = None
            self.running = False
            if on_done:
                on_done()

        threading.Thread(target=work, daemon=True).start()
        return True

    def tail(self, n):
        with self.lock:
            return list(self.lines)[-n:]


# --------------------------------------------------------------------------
# actions (shared by the TUI and --batch)
# --------------------------------------------------------------------------
def selected_targets(cfg, only=None):
    """Targets to build / flash: the ones switched on in the configuration, or exactly `only` (a list of "sat" / "gs")."""
    if only:
        return [k for k in ("sat", "gs") if k in only]
    return [k for k in ("sat", "gs") if cfg["build_" + k]]


def parse_bool(v):
    v = str(v).strip().lower()
    if v in ("t", "true", "1", "y", "yes", "on"):
        return True
    if v in ("f", "false", "0", "n", "no", "off"):
        return False
    raise ValueError("expected T or F, got %r" % v)


def cubus_on(cfg, cubus=None):
    """CUBUS build for the satellite: the command-line override when given (make CUBUS=T), else the configuration."""
    return bool(cfg["cubus"]) if cubus is None else bool(cubus)


def make_args(cfg, k, cubus=None):
    """Extra `make` arguments of one target. CUBUS only exists for the satellite image; it is passed explicitly only when
    overridden on the command line (otherwise the generated s2s2.mk carries the saved setting)."""
    return ["CUBUS=" + ("TRUE" if cubus else "FALSE")] if (k == "sat" and cubus is not None) else []


def target_title(cfg, k, cubus=None):
    return TARGETS[k]["name"] + (" [CUBUS]" if k == "sat" and cubus_on(cfg, cubus) else "")


def build_steps(cfg, only=None, cubus=None):
    return [("Build " + target_title(cfg, k, cubus), ["make"] + make_args(cfg, k, cubus), TARGETS[k]["dir"])
            for k in selected_targets(cfg, only)]


def flash_steps(cfg, with_cpu1=False, only=None, cubus=None):
    steps, problems = [], []
    seen = set()
    for k in selected_targets(cfg, only):
        serial = cfg[k + "_serial"]
        if not serial:
            problems.append("%s: no ST-Link selected" % TARGETS[k]["name"])
            continue
        if serial in seen:
            problems.append("%s: same probe as the other target" % TARGETS[k]["name"])
            continue
        if with_cpu1 and k == "sat" and cubus_on(cfg, cubus):
            problems.append("CUBUS: the CPU1 (OBC) firmware of the flight board is not flashed from here (flash-all would overwrite it "
                            "with the Nucleo image store app); use `flash` for the CPU2 radio image")
            continue
        seen.add(serial)
        d = TARGETS[k]["dir"]
        extra = make_args(cfg, k, cubus)
        title = target_title(cfg, k, cubus)
        if with_cpu1:
            steps.append((title + ": flash CPU1 (NuttX)", ["make", "flash-m4", "SERIAL=" + serial], d))
            steps.append((title + ": set CPU2 boot option bytes", ["make", "c2boot", "SERIAL=" + serial], d))
        steps.append((title + ": flash CPU2", ["make", "flash", "SERIAL=" + serial] + extra, d))
    return steps, problems


def clean_steps(cfg, only=None):
    return [("Clean " + TARGETS[k]["name"], ["make", "clean"], TARGETS[k]["dir"]) for k in ("sat", "gs") if not only or k in only]


def gen_cli(argv):
    """python3 s2s2_core.py gen [--quiet] [--new-key]: write generated/{secrets.h, s2s2_config.h, s2s2.mk}."""
    quiet = "--quiet" in argv
    try:
        changed = generate(new_key="--new-key" in argv)
    except ValueError as e:
        print("invalid configuration: %s" % e)
        return 1
    if not quiet:
        key = read_key()
        print("generated/: %s" % (", ".join(changed) if changed else "up to date"))
        print("AUTH key fingerprint: %s" % (key_fingerprint(key) if key else "?"))
    return 0


def batch(action, only=None, cubus=None):
    """action: build | flash | flash-all | clean.  only: ["sat"] / ["gs"] / both (default: the targets switched on in the
    configuration).  cubus: True / False overrides the saved CUBUS option for this run only (make CUBUS=T)."""
    cfg = load_config()
    if action == "build":
        steps = build_steps(cfg, only, cubus)
    elif action in ("flash", "flash-all"):
        steps, problems = flash_steps(cfg, with_cpu1=(action == "flash-all"), only=only, cubus=cubus)
        if problems:
            print("; ".join(problems) + "\nRun 'make tui' to pick the ST-Links.")
            return 2
    elif action == "clean":
        steps = clean_steps(cfg, only)
    else:
        print("unknown batch action", action)
        return 2
    r = Runner()
    r.run_steps(action, steps)
    shown = 0
    while r.busy or r.rc is None:
        time.sleep(0.1)
        with r.lock:
            new = list(r.lines)[shown:]
            shown += len(new)
        for ln in new:
            print(ln)
        if r.rc is not None and not r.busy:
            break
    with r.lock:
        for ln in list(r.lines)[shown:]:
            print(ln)
    for k in selected_targets(cfg, only):
        print("\n".join(format_stats(target_stats(k))))
    return r.rc or 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "gen":
        sys.exit(gen_cli(sys.argv[2:]))
    print("usage: s2s2_core.py gen [--quiet] [--new-key]")
    sys.exit(2)
