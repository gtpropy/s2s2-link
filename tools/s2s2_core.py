#!/usr/bin/env python3
"""
s2s2_core.py -- build / flash / memory-statistics logic shared by the Textual TUI
(s2s2_tui.py) and the batch modes. No UI code here; standard library only.

Configuration is stored in <repo>/.s2s2.config (JSON).
"""
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from collections import deque

REPO = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
CONFIG_PATH = os.path.join(REPO, ".s2s2.config")

# CPU2 memory map (scripts/wl55jc_cm0p.ld)
CPU2_FLASH_BASE, CPU2_FLASH_SIZE = 0x08032000, 56 * 1024
CPU2_RAM_BASE, CPU2_RAM_SIZE = 0x20009000, 28 * 1024
CPU1_FLASH_SIZE = 0x32000                       # 0x08000000 .. 0x08032000
HEAP_STACK_RESERVE = 0x200 + 0x400              # _Min_Heap_Size + _Min_Stack_Size

TARGETS = {
    "sat": {"name": "Satellite", "dir": os.path.join(REPO, "Satellite_M0+", "Com_sat"),
            "bin": "satellite.bin", "elf": "satellite.elf"},
    "gs": {"name": "Ground station", "dir": os.path.join(REPO, "Ground_Station"),
           "bin": "gs_m0plus.bin", "elf": "gs_m0plus.elf"},
}
NUTTX_BIN = os.path.join(REPO, "nuttx", "nuttx.bin")

DEFAULT_CONFIG = {
    "build_sat": True, "build_gs": True,
    "auth": True, "debug": False, "seal_test": False,
    "sat_serial": "", "gs_serial": "",
}


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


def make_args(cfg):
    return ["AUTH=%s" % ("TRUE" if cfg["auth"] else "FALSE"),
            "DEBUG=%s" % ("T" if cfg["debug"] else "F"),
            "SEAL_TEST=%s" % ("TRUE" if cfg["seal_test"] else "FALSE")]


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


def scan_probes():
    try:
        out = subprocess.run(["st-info", "--probe"], capture_output=True, text=True, timeout=15)
        return parse_probes(out.stdout + out.stderr)
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
    binp, elfp = os.path.join(t["dir"], t["bin"]), os.path.join(t["dir"], t["elf"])
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
def selected_targets(cfg):
    return [k for k in ("sat", "gs") if cfg["build_" + k]]


def build_steps(cfg):
    return [("Build " + TARGETS[k]["name"], ["make"] + make_args(cfg), TARGETS[k]["dir"])
            for k in selected_targets(cfg)]


def flash_steps(cfg, with_cpu1=False):
    steps, problems = [], []
    seen = set()
    for k in selected_targets(cfg):
        serial = cfg[k + "_serial"]
        if not serial:
            problems.append("%s: no ST-Link selected" % TARGETS[k]["name"])
            continue
        if serial in seen:
            problems.append("%s: same probe as the other target" % TARGETS[k]["name"])
            continue
        seen.add(serial)
        d = TARGETS[k]["dir"]
        if with_cpu1:
            steps.append((TARGETS[k]["name"] + ": flash CPU1 (NuttX)", ["make", "flash-m4", "SERIAL=" + serial], d))
            steps.append((TARGETS[k]["name"] + ": set CPU2 boot option bytes", ["make", "c2boot", "SERIAL=" + serial], d))
        steps.append((TARGETS[k]["name"] + ": flash CPU2", ["make", "flash", "SERIAL=" + serial] + make_args(cfg), d))
    return steps, problems


def clean_steps(cfg):
    return [("Clean " + TARGETS[k]["name"], ["make", "clean"], TARGETS[k]["dir"]) for k in ("sat", "gs")]


def batch(action):
    cfg = load_config()
    if action == "build":
        steps = build_steps(cfg)
    elif action in ("flash", "flash-all"):
        steps, problems = flash_steps(cfg, with_cpu1=(action == "flash-all"))
        if problems:
            print("; ".join(problems) + "\nRun 'make tui' to pick the ST-Links.")
            return 2
    elif action == "clean":
        steps = clean_steps(cfg)
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
    for k in selected_targets(cfg):
        print("\n".join(format_stats(target_stats(k))))
    return r.rc or 0
