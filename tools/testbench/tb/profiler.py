"""
profiler.py -- live "task manager" for the two STM32WL55 boards, read over SWD without stopping the cores.

One OpenOCD per board (ST-Link DAP, both cores). While it runs, from the PC side only:
  * PC sampling   DWT_PCSR (0xE000101C) of the Cortex-M4 (CPU1, NuttX) and the Cortex-M0+ (CPU2, radio firmware) read hundreds of times
                  per second; every sample is resolved to a function through the ELF symbol table, so you see where each core spends its
                  time (a sleeping core reads 0xFFFFFFFF = idle).
  * memory map    the 64 KB of SRAM dumped about once a second, shown as a heat map (what changes) against the linker layout
                  (.data, .bss, heap, stack, IPC mailboxes).
  * watches       any RAM symbol from the ELF read about four times a second with a history.
  * peek          read-only memory dump of any address.
  * stack         optional, on request: halts one core for a few milliseconds to read SP and to paint the unused stack with a
                  pattern; later the deepest overwritten word is the stack high-water mark.
Nothing is written to the target except the stack paint pattern into unused stack RAM.
"""
import bisect
import collections
import os
import re
import socket
import subprocess
import tempfile
import threading
import time

PCSR = 0xE000101C
IDLE_FUNCS = {"up_idle", "arm_idle", "z_arm_cpu_idle", "__WFI", "WFI_Loop", "Default_Handler"}      # a core parked in WFI still reports the PC of the WFI
SRAM_BASE, SRAM_SIZE = 0x20000000, 0x10000
BLOCK = 256


class OpenOcd:
    def __init__(self, serial, tcl_port):
        self.serial = serial
        self.port = tcl_port
        self.proc = None
        self.sk = None
        self.lock = threading.Lock()

    def start(self):
        cmd = ["openocd", "-f", "interface/stlink-dap.cfg", "-c", "transport select dapdirect_swd", "-c", f"adapter serial {self.serial}",
               "-c", "set DUAL_CORE 1", "-f", "target/stm32wlx.cfg", "-c", "reset_config none", "-c", "adapter speed 4000",
               "-c", "gdb_port disabled", "-c", "telnet_port disabled", "-c", f"tcl_port {self.port}", "-c", "init"]
        self.log = os.path.join(tempfile.gettempdir(), f"s2s2_openocd_{self.port}.log")
        self.proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=open(self.log, "w"), start_new_session=True)   # a file: an unread pipe fills up and blocks openocd
        for _ in range(80):
            if self.proc.poll() is not None:
                err = open(self.log).read()
                raise RuntimeError("openocd exited: " + (err.strip().splitlines() or ["?"])[-1] + "  (another tool may hold the ST-Link)")
            try:
                self.sk = socket.create_connection(("127.0.0.1", self.port), timeout=2)
                break
            except OSError:
                time.sleep(0.15)
        else:
            raise RuntimeError("openocd tcl port did not come up")
        self.sk.settimeout(10)

    def cmd(self, c):
        with self.lock:
            try:
                self.sk.sendall(c.encode() + b"\x1a")
                d = b""
                while not d.endswith(b"\x1a"):
                    chunk = self.sk.recv(65536)
                    if not chunk:
                        raise RuntimeError("openocd closed the connection")
                    d += chunk
            except socket.timeout:
                self.sk.close()                          # a late answer would answer the next command: start a fresh connection
                self.sk = socket.create_connection(("127.0.0.1", self.port), timeout=2)
                self.sk.settimeout(10)
                raise RuntimeError("openocd did not answer in 10 s")
            return d[:-1].decode(errors="replace").strip()

    def stop(self):
        try:
            os.remove(self.log)
        except (OSError, AttributeError):
            pass
        try:
            if self.sk:
                self.sk.close()
        except OSError:
            pass
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(3)
            except subprocess.TimeoutExpired:
                self.proc.kill()


class Elf:
    """Symbols and layout of one firmware image (arm-none-eabi-nm / size / the linker script)."""

    def __init__(self, path, ld=None):
        self.path = path
        self.ok = os.path.exists(path)
        self.funcs = []          # (addr, size, name) sorted
        self.addrs = []
        self.objs = []           # (addr, size, name, kind) RAM objects
        self.sections = []       # (name, size, addr)
        self.regions = {}        # FLASH / RAM -> (origin, length)
        self.syms = {}
        if not self.ok:
            return
        out = subprocess.run(["arm-none-eabi-nm", "-S", "-n", "--defined-only", path], capture_output=True, text=True).stdout
        for ln in out.splitlines():
            p = ln.split()
            if len(p) == 4:
                a, sz, k, n = int(p[0], 16), int(p[1], 16), p[2], p[3]
            elif len(p) == 3:
                a, sz, k, n = int(p[0], 16), 0, p[1], p[2]
            else:
                continue
            self.syms[n] = a
            if k in "tTwW" and (a & 0xF0000000) in (0x08000000, 0x00000000, 0x10000000):
                self.funcs.append((a & ~1, max(sz, 2), n))
            elif k in "bBdD" and 0x20000000 <= a < 0x20010000:
                self.objs.append((a, sz, n, k))
        self.funcs.sort()
        self.addrs = [f[0] for f in self.funcs]
        sz = subprocess.run(["arm-none-eabi-size", "-A", path], capture_output=True, text=True).stdout
        for ln in sz.splitlines():
            p = ln.split()
            if len(p) == 3 and p[0].startswith(".") and not p[0].startswith(".debug") and p[0] not in (".comment", ".ARM.attributes"):
                self.sections.append((p[0], int(p[1]), int(p[2])))
        if ld and os.path.exists(ld):
            for m in re.finditer(r"(\w+)\s*\([rwx]+\)\s*:\s*ORIGIN\s*=\s*(0x[0-9A-Fa-f]+),\s*LENGTH\s*=\s*(\d+)K", open(ld).read()):
                self.regions[m.group(1)] = (int(m.group(2), 16), int(m.group(3)) * 1024)

    def lookup(self, pc):
        pc &= ~1
        i = bisect.bisect_right(self.addrs, pc) - 1
        if i >= 0:
            a, sz, n = self.funcs[i]
            if pc < a + max(sz, 2) + 0x40:
                return n
        return "0x%08X" % pc

    def find_obj(self, name):
        for a, sz, n, k in self.objs:
            if n == name:
                return a, sz
        return None


class Core:
    def __init__(self, label, tcl_name, elf):
        self.label, self.tcl, self.elf = label, tcl_name, elf
        self.total = collections.Counter()
        self.window = collections.deque(maxlen=20)          # last ~10 s of per-0.5s counters
        self.cur = collections.Counter()
        self.sleep = 0
        self.samples = 0


class Session:
    def __init__(self, name, serial, cores, publish, tmp):
        self.name, self.serial, self.publish, self.tmp = name, serial, publish, tmp
        self.oo = None
        self.cores = cores
        self.stop_ev = threading.Event()
        self.threads = []
        self.heat = [0] * (SRAM_SIZE // BLOCK)
        self.used = [0] * (SRAM_SIZE // BLOCK)
        self.prev = None
        self.watch = {}                                    # name -> dict(core, addr, size, hist)
        self.rate = 0.0
        self.error = ""
        self.t0 = time.time()
        self.stackmark = {}                                # core label -> dict
        self.mem_ms = 0

    def start(self):
        s = socket.socket()
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
        s.close()
        self.oo = OpenOcd(self.serial, port)
        self.oo.start()
        for fn in (self._pc_loop, self._mem_loop, self._watch_loop, self._pub_loop):
            t = threading.Thread(target=fn, daemon=True)
            t.start()
            self.threads.append(t)

    def stop(self):
        self.stop_ev.set()
        for t in self.threads:
            t.join(2)
        if self.oo:
            self.oo.stop()

    # ---- loops
    def _pc_loop(self):
        N = 64
        loops = {c.label: ("set r {}; for {set i 0} {$i<%d} {incr i} {lappend r [%s read_memory 0x%X 32 1]}; set r" % (N, c.tcl, PCSR)) for c in self.cores}
        t_rate, n_rate = time.time(), 0
        while not self.stop_ev.is_set():
            for c in self.cores:
                try:
                    out = self.oo.cmd(loops[c.label])
                except Exception as e:
                    self.error = str(e)
                    time.sleep(0.5)
                    continue
                for tok in out.split():
                    try:
                        pc = int(tok, 0)
                    except ValueError:
                        continue
                    c.samples += 1
                    n_rate += 1
                    if pc == 0xFFFFFFFF:
                        c.sleep += 1
                        c.cur["(sleeping)"] += 1
                        c.total["(sleeping)"] += 1
                    else:
                        n = c.elf.lookup(pc)
                        c.cur[n] += 1
                        c.total[n] += 1
            now = time.time()
            if now - t_rate >= 1.0:
                self.rate = n_rate / (now - t_rate)
                t_rate, n_rate = now, 0

    def _mem_loop(self):
        path = os.path.join(self.tmp, f"sram_{self.name}.bin")
        while not self.stop_ev.is_set():
            t = time.time()
            try:
                self.oo.cmd(f"dump_image {{{path}}} 0x{SRAM_BASE:X} {SRAM_SIZE}")
                data = open(path, "rb").read()
            except Exception as e:
                self.error = str(e)
                time.sleep(1)
                continue
            if len(data) == SRAM_SIZE:
                for b in range(SRAM_SIZE // BLOCK):
                    blk = data[b * BLOCK:(b + 1) * BLOCK]
                    self.used[b] = sum(1 for x in blk if x) * 255 // BLOCK
                    if self.prev is not None:
                        diff = sum(1 for x, y in zip(blk, self.prev[b * BLOCK:(b + 1) * BLOCK]) if x != y)
                        self.heat[b] = min(255, int(self.heat[b] * 0.7 + diff * 255 / BLOCK * 1.2))
                self.prev = data
                self.mem_ms = int((time.time() - t) * 1000)
            self.stop_ev.wait(max(0.2, 1.0 - (time.time() - t)))

    def _watch_loop(self):
        while not self.stop_ev.is_set():
            for name, w in list(self.watch.items()):
                try:
                    sz = w["size"]
                    width, count = (32, min(sz // 4, 16)) if sz >= 4 else ((16, 1) if sz == 2 else (8, sz))
                    out = self.oo.cmd(f"stm32wlx.cpu0 read_memory 0x{w['addr']:X} {width} {count}")
                    vals = [int(x, 0) for x in out.split()]
                    w["value"] = vals
                    if len(vals) == 1:
                        w["hist"].append(vals[0])
                        if len(w["hist"]) > 240:
                            w["hist"].pop(0)
                except Exception as e:
                    w["error"] = str(e)
            self.stop_ev.wait(0.25)

    def _pub_loop(self):
        while not self.stop_ev.is_set():
            self.stop_ev.wait(0.5)
            self.publish("prof", target=self.name, **self.state())

    def state(self):
        cores = []
        for c in self.cores:
            c.window.append(c.cur)
            c.cur = collections.Counter()
            win = collections.Counter()
            for w in c.window:
                win.update(w)
            tot = sum(win.values()) or 1
            slp = sum(v for n, v in win.items() if n == "(sleeping)" or n in IDLE_FUNCS)
            top = [{"fn": n, "pct": round(100.0 * v / tot, 1)} for n, v in win.most_common(14)]
            alltot = sum(c.total.values()) or 1
            life = [{"fn": n, "pct": round(100.0 * v / alltot, 2)} for n, v in c.total.most_common(25)]
            cores.append({"core": c.label, "samples": c.samples, "busy_pct": round(100.0 * (tot - slp) / tot, 1), "sleep_pct": round(100.0 * slp / tot, 1),
                          "top": top, "lifetime": life, "elf": c.elf.ok, "stack": self.stackmark.get(c.label)})
        return {"rate": round(self.rate), "cores": cores, "heat": self.heat, "used": self.used, "mem_ms": self.mem_ms, "error": self.error,
                "uptime": round(time.time() - self.t0, 1),
                "watch": {n: {"addr": w["addr"], "size": w["size"], "value": w.get("value"), "hist": w["hist"][-120:], "core": w["core"], "error": w.get("error")}
                          for n, w in self.watch.items()}}

    # ---- actions
    def add_watch(self, name, core_label):
        c = next(c for c in self.cores if c.label == core_label)
        o = c.elf.find_obj(name)
        if not o:
            raise RuntimeError(f"{name} is not a RAM symbol of the {core_label} image")
        self.watch[name] = {"addr": o[0], "size": max(1, min(o[1], 64)), "core": core_label, "hist": []}

    def peek(self, addr, length):
        length = max(4, min(length, 1024)) & ~3
        out = self.oo.cmd(f"stm32wlx.cpu0 read_memory 0x{addr:X} 8 {length}")
        return bytes(int(x, 0) for x in out.split())

    def _regs(self, tcl):
        txt = self.oo.cmd(f'targets {tcl}; halt; set r [capture "reg sp"]; append r "\\n" [capture "reg pc"]; append r "\\n" [capture "reg lr"]; resume; targets stm32wlx.cpu0; set r')
        d = {}
        for m in re.finditer(r"^(\w+) \([^)]*\): (0x[0-9a-fA-F]+)", txt, re.M):
            d[m.group(1)] = int(m.group(2), 16)
        return d

    def stack_arm(self, core_label):
        c = next(c for c in self.cores if c.label == core_label)
        if "_estack" not in c.elf.syms:
            raise RuntimeError("no _estack symbol")
        top = c.elf.syms["_estack"]
        ebss = c.elf.syms.get("_ebss", c.elf.syms.get("_end", 0))
        r = self._regs(c.tcl)
        sp = r.get("sp")
        if not sp or not (ebss < sp <= top):
            raise RuntimeError(f"unexpected SP {sp and hex(sp)} (stack should be below {hex(top)})")
        lo = (ebss + 0x200 + 3) & ~3                       # keep clear of the heap reserve
        hi = (sp - 64) & ~3
        if hi <= lo:
            raise RuntimeError("no free stack area to paint")
        words = (hi - lo) // 4
        for off in range(0, words, 256):
            n = min(256, words - off)
            self.oo.cmd("stm32wlx.cpu0 write_memory 0x%X 32 {%s}" % (lo + off * 4, " ".join(["0xDEADBEEF"] * n)))
        self.stackmark[c.label] = {"top": top, "lo": lo, "hi": hi, "sp": sp, "armed": time.time(), "used": None}
        return self.stackmark[c.label]

    def stack_read(self, core_label):
        m = self.stackmark.get(core_label)
        if not m:
            raise RuntimeError("arm the stack watermark first")
        words = (m["hi"] - m["lo"]) // 4
        deepest = m["hi"]
        path = os.path.join(self.tmp, f"stack_{self.name}.bin")
        r = self.oo.cmd("targets stm32wlx.cpu0; dump_image {%s} 0x%X %d" % (path, m["lo"], words * 4))
        if not os.path.exists(path):
            raise RuntimeError("stack dump failed: " + r[:120])
        raw = open(path, "rb").read()
        for i in range(0, len(raw) - 3, 4):
            if raw[i:i + 4] != b"\xef\xbe\xad\xde":
                deepest = m["lo"] + i
                break
        m["deepest"] = deepest
        m["used"] = m["top"] - deepest
        m["free"] = deepest - m["lo"]
        return m


class Profiler:
    """Holds up to one session per board; targets are configured by the server (serial numbers and ELF paths)."""

    def __init__(self, publish, root, targets):
        self.publish = publish
        self.root = root
        self.targets = targets                             # name -> dict(serial, cpu2_elf, cpu2_ld, cpu1_elf)
        self.sessions = {}
        self.tmp = os.path.abspath(os.path.join(root, "data", "testbench", "prof"))
        os.makedirs(self.tmp, exist_ok=True)
        self.elfs = {}

    def _elf(self, path, ld=None):
        k = (path, ld)
        mt = os.path.getmtime(path) if os.path.exists(path) else 0           # a missing ELF (e.g. CPU1 of the CUBUS board) is allowed: addresses are shown raw
        if k not in self.elfs or mt != self.elfs[k][0]:
            self.elfs[k] = (mt, Elf(path, ld))
        return self.elfs[k][1]

    def start(self, name):
        if name in self.sessions:
            return
        t = self.targets[name]
        if not t["serial"]:
            raise RuntimeError(f"no ST-Link serial configured for {name}")
        cores = [Core("CPU2 (M0+ radio)", "stm32wlx.cpu1", self._elf(t["cpu2_elf"], t.get("cpu2_ld"))),
                 Core("CPU1 (M4 NuttX)", "stm32wlx.cpu0", self._elf(t["cpu1_elf"]))]
        s = Session(name, t["serial"], cores, self.publish, self.tmp)
        s.start()
        self.sessions[name] = s

    def stop(self, name=None):
        for n in ([name] if name else list(self.sessions)):
            s = self.sessions.pop(n, None)
            if s:
                s.stop()

    def layout(self, name):
        t = self.targets[name]
        out = []
        for label, path, ld in (("CPU2 (M0+ radio)", t["cpu2_elf"], t.get("cpu2_ld")), ("CPU1 (M4 NuttX)", t["cpu1_elf"], None)):
            e = self._elf(path, ld)
            regions = dict(e.regions)
            if not regions and label.startswith("CPU1"):        # NuttX: nuttx.bin must stay below page 72 (0x08024000); SRAM1 is 32 KB
                regions = {"FLASH": (0x08000000, 0x24000), "RAM": (0x20000000, 0x8000)}
            secs = [{"name": n, "size": s, "addr": a} for n, s, a in e.sections]
            flash = [x for x in secs if 0x08000000 <= x["addr"] < 0x08040000]
            ram = [x for x in secs if 0x20000000 <= x["addr"] < 0x20010000]
            top = sorted(e.objs, key=lambda o: -o[1])[:25]
            out.append({"core": label, "elf": e.ok, "path": os.path.relpath(path, self.root), "mtime": os.path.getmtime(path) if e.ok else None,
                        "regions": regions, "sections": secs, "flash_used": sum(x["size"] for x in flash), "ram_used": sum(x["size"] for x in ram),
                        "estack": e.syms.get("_estack"), "ebss": e.syms.get("_ebss"), "sdata": e.syms.get("_sdata"),
                        "top_ram": [{"name": n, "addr": a, "size": sz} for a, sz, n, k in top]})
        return out

    def symbols(self, name, core_label, q):
        t = self.targets[name]
        e = self._elf(t["cpu2_elf"], t.get("cpu2_ld")) if core_label.startswith("CPU2") else self._elf(t["cpu1_elf"])
        q = q.lower()
        return [{"name": n, "addr": a, "size": sz} for a, sz, n, k in e.objs if q in n.lower() and sz > 0][:60]

    def ramsyms(self, name):
        t = self.targets[name]
        out = {}
        for label, path, ld in (("CPU2 (M0+ radio)", t["cpu2_elf"], t.get("cpu2_ld")), ("CPU1 (M4 NuttX)", t["cpu1_elf"], None)):
            e = self._elf(path, ld)
            out[label] = sorted([[a, sz, n] for a, sz, n, k in e.objs if sz > 0])[:1500]
        return out
