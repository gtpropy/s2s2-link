#!/usr/bin/env python3
"""
s2s2_tui.py -- menuconfig-style TUI for the S2S2 CPU2 firmware (satellite + ground station).

  python3 tools/s2s2_tui.py            interactive menu            (make / make tui)
  python3 tools/s2s2_tui.py --memstat  print static memory stats   (make memstat)
  python3 tools/s2s2_tui.py --probes   list ST-Link probes         (make probes)
  python3 tools/s2s2_tui.py --batch build|flash|flash-all|clean    (make build ...)
  python3 tools/s2s2_tui.py --dump     render the main screen as text (testing)

Configuration is stored in <repo>/.s2s2.config (JSON). Only the Python
standard library is used.
"""
import curses
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


# --------------------------------------------------------------------------
# TUI
# --------------------------------------------------------------------------
class App:
    def __init__(self, stdscr=None):
        self.scr = stdscr
        self.cfg = load_config()
        self.probes = scan_probes()
        self.runner = Runner()
        self.sel = 0
        self.stats = {k: target_stats(k) for k in TARGETS}
        self.msg = "Up/Down move, Enter/Space select, b build, f flash, a flash all, q quit"
        self.view = "main"          # main | report | log

    # ---- menu model ----
    def probe_label(self, serial):
        if not serial:
            return "< none selected >"
        for p in self.probes:
            if p["serial"] == serial:
                return "%s  %s" % (serial, p["version"])
        return "%s  (not connected)" % serial

    def items(self):
        c = self.cfg
        chk = lambda v: "[*]" if v else "[ ]"
        return [
            ("hdr", "Targets", None),
            ("tog", "%s Satellite        (Satellite_M0+/Com_sat)" % chk(c["build_sat"]), "build_sat"),
            ("tog", "%s Ground station   (Ground_Station)" % chk(c["build_gs"]), "build_gs"),
            ("hdr", "Options", None),
            ("tog", "%s AUTH       integrity tag + replay counter" % chk(c["auth"]), "auth"),
            ("tog", "%s DEBUG      -Og -g3 (satellite may not fit 56 KB)" % chk(c["debug"]), "debug"),
            ("tog", "%s SEAL_TEST  extended boot self-test (+~600 B)" % chk(c["seal_test"]), "seal_test"),
            ("hdr", "ST-Link probes (%d connected)  [p rescan]" % len(self.probes), None),
            ("probe", "Satellite probe:  " + self.probe_label(c["sat_serial"]), "sat_serial"),
            ("probe", "Ground probe:     " + self.probe_label(c["gs_serial"]), "gs_serial"),
            ("hdr", "Actions", None),
            ("act", "Build selected targets", "build"),
            ("act", "Flash CPU2 (selected targets)", "flash"),
            ("act", "Flash CPU1 NuttX + CPU2 boot option + CPU2", "flash_all"),
            ("act", "Memory report (full screen)", "report"),
            ("act", "Clean build outputs", "clean"),
            ("act", "Save configuration", "save"),
            ("act", "Quit", "quit"),
        ]

    def selectable(self):
        return [i for i, it in enumerate(self.items()) if it[0] != "hdr"]

    def move(self, d):
        idx = self.selectable()
        pos = idx.index(self.sel) if self.sel in idx else 0
        self.sel = idx[(pos + d) % len(idx)]

    # ---- actions ----
    def refresh_stats(self):
        self.stats = {k: target_stats(k) for k in TARGETS}

    def start(self, title, steps):
        if not steps:
            self.msg = "Nothing to do (no target selected)"
            return
        if not self.runner.run_steps(title, steps, on_done=self.refresh_stats):
            self.msg = "A command is already running"
        else:
            self.msg = "Running: " + title

    def do_action(self, name):
        c = self.cfg
        if name == "build":
            self.start("Build", build_steps(c))
        elif name in ("flash", "flash_all"):
            steps, problems = flash_steps(c, with_cpu1=(name == "flash_all"))
            if problems:
                self.msg = "; ".join(problems)
                return
            if name == "flash_all" and not os.path.exists(NUTTX_BIN):
                self.msg = "nuttx/nuttx.bin missing (build NuttX first)"
                return
            self.start("Flash", steps)
        elif name == "report":
            self.view = "report"
        elif name == "clean":
            self.start("Clean", clean_steps(c))
        elif name == "save":
            save_config(c)
            self.msg = "Saved " + CONFIG_PATH
        elif name == "quit":
            return "quit"

    def choose_probe(self, key):
        self.probes = scan_probes()
        opts = [("(none)", "")] + [("%s  %s  %s" % (p["serial"], p["version"], p["chip"]), p["serial"])
                                   for p in self.probes]
        cur = 0
        for i, (_, s) in enumerate(opts):
            if s == self.cfg[key]:
                cur = i
        h, w = self.scr.getmaxyx()
        bw = min(w - 4, 74)
        while True:
            win = curses.newwin(len(opts) + 4, bw, max(1, (h - len(opts) - 4) // 2), (w - bw) // 2)
            win.box()
            win.addstr(0, 2, " Select ST-Link for %s " % ("satellite" if key == "sat_serial" else "ground station"))
            for i, (label, _) in enumerate(opts):
                win.addstr(i + 2, 2, label[:bw - 5], curses.A_REVERSE if i == cur else curses.A_NORMAL)
            win.refresh()
            k = self.scr.getch()
            if k in (curses.KEY_UP, ord("k")):
                cur = (cur - 1) % len(opts)
            elif k in (curses.KEY_DOWN, ord("j")):
                cur = (cur + 1) % len(opts)
            elif k in (10, 13, curses.KEY_ENTER, ord(" ")):
                self.cfg[key] = opts[cur][1]
                self.msg = "Selected probe: " + (opts[cur][1] or "none")
                return
            elif k in (27, ord("q")):
                return

    def activate(self):
        kind, _, arg = self.items()[self.sel]
        if kind == "tog":
            self.cfg[arg] = not self.cfg[arg]
        elif kind == "probe":
            self.choose_probe(arg)
        elif kind == "act":
            return self.do_action(arg)

    # ---- drawing ----
    def put(self, y, x, text, attr=0):
        h, w = self.scr.getmaxyx()
        if 0 <= y < h and x < w:
            try:
                self.scr.addstr(y, x, text[: max(0, w - x - 1)], attr)
            except curses.error:
                pass

    def draw_main(self):
        s = self.scr
        s.erase()
        h, w = s.getmaxyx()
        self.put(0, 1, " S2S2 CPU2 firmware configuration ", curses.A_REVERSE)
        self.put(0, w - 28, "AUTH=%s DEBUG=%s" % ("on" if self.cfg["auth"] else "off", "on" if self.cfg["debug"] else "off"))
        left_w = 56 if w >= 112 else w - 2
        y = 2
        for i, (kind, text, _) in enumerate(self.items()):
            if kind == "hdr":
                y += 0 if i == 0 else 1
                self.put(y, 1, "-- %s --" % text, curses.A_BOLD)
            else:
                attr = curses.A_REVERSE if i == self.sel else 0
                self.put(y, 3, ("> " if kind == "act" else "  ") + text, attr)
            y += 1
        # right panel: memory stats
        if w >= 112:
            x = left_w + 3
            yy = 2
            self.put(yy, x, "-- Static memory --", curses.A_BOLD)
            yy += 1
            for k in selected_targets(self.cfg) or list(TARGETS):
                for ln in format_stats(self.stats[k])[:7]:
                    self.put(yy, x, ln)
                    yy += 1
                yy += 1
            self.put(yy, x, nuttx_line())
        # log pane
        top = max(y + 1, h - 12)
        self.put(top - 1, 1, "-- Output: %s %s --" % (self.runner.title or "-", "(running)" if self.runner.busy else ""), curses.A_BOLD)
        for i, ln in enumerate(self.runner.tail(h - top - 2)):
            self.put(top + i, 2, ln)
        self.put(h - 1, 1, self.msg, curses.A_DIM)
        s.refresh()

    def draw_report(self):
        s = self.scr
        s.erase()
        self.put(0, 1, " Memory report  (any key = back, r = refresh) ", curses.A_REVERSE)
        y = 2
        for k in TARGETS:
            for ln in format_stats(self.stats[k]):
                self.put(y, 2, ln)
                y += 1
            y += 1
        self.put(y, 2, nuttx_line())
        s.refresh()

    def run(self):
        curses.curs_set(0)
        curses.use_default_colors()
        self.scr.timeout(150)
        self.sel = self.selectable()[0]
        while True:
            if self.view == "report":
                self.draw_report()
                k = self.scr.getch()
                if k == ord("r"):
                    self.refresh_stats()
                elif k != -1:
                    self.view = "main"
                continue
            self.draw_main()
            k = self.scr.getch()
            if k == -1:
                continue
            if k in (curses.KEY_UP, ord("k")):
                self.move(-1)
            elif k in (curses.KEY_DOWN, ord("j")):
                self.move(1)
            elif k in (10, 13, curses.KEY_ENTER, ord(" ")):
                if self.activate() == "quit":
                    return
            elif k == ord("b"):
                self.do_action("build")
            elif k == ord("f"):
                self.do_action("flash")
            elif k == ord("a"):
                self.do_action("flash_all")
            elif k == ord("p"):
                self.probes = scan_probes()
                self.msg = "%d ST-Link probe(s) found" % len(self.probes)
            elif k == ord("s"):
                self.do_action("save")
            elif k == ord("m"):
                self.view = "report"
            elif k == ord("q"):
                save_config(self.cfg)
                return


def dump_main():
    app = App(None)
    out = []
    for i, (kind, text, _) in enumerate(app.items()):
        out.append(("-- %s --" % text) if kind == "hdr" else ("   " + text))
    out.append("")
    for k in TARGETS:
        out.extend(format_stats(app.stats[k]))
        out.append("")
    out.append(nuttx_line())
    print("\n".join(out))


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


def main(argv):
    os.environ.setdefault("ESCDELAY", "25")
    if "--dump" in argv:
        dump_main()
        return 0
    if "--memstat" in argv:
        for k in TARGETS:
            print("\n".join(format_stats(target_stats(k))) + "\n")
        print(nuttx_line())
        return 0
    if "--probes" in argv:
        ps = scan_probes()
        if not ps:
            print("no ST-Link probes found")
        for p in ps:
            print("%s  %s  %s" % (p["serial"], p["version"], p["chip"]))
        return 0
    if "--batch" in argv:
        return batch(argv[argv.index("--batch") + 1])
    if not sys.stdin.isatty() or not sys.stdout.isatty():
        print("not a terminal -- use --batch build|flash|flash-all|clean, --memstat or --probes")
        return 2
    curses.wrapper(lambda scr: App(scr).run())
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
