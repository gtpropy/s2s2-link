"""
tb.cubus -- RSMS CUBUS V.3 support of the testbench and its DEBUG section.

On the CUBUS flight board there is no Nucleo ST-Link virtual COM port and CPU1 runs the OBC firmware, not the s2s2obc bench app. So in
CUBUS mode (`cubus` in .s2s2.config, TUI Build options / `make CUBUS=T`):
  * the "Nucleo" link becomes a text-only link to the satellite's debug UART (PA9 / PA10 on the ACCESS connector, USB serial adapter)
  * tests that need the bench app protocol (image store, flash IO, digipeater inject, rate, CW test, e2e ...) are blocked with a reason
  * the DEBUG section works with what the board really offers: SWD (the ST-Link of the board) and the radio

DEBUG tools (everything runs only when you press a button; nothing is opened at start-up):
  snapshot()    one SWD read of the satellite CPU2: board id, state machine, receive-stage counters (g_satRx), RF switch / PA / 5 V pins,
                the OBC command ring and block; read twice 0.5 s apart to see whether the core runs
  diagnose()    pure function: turns snapshots (+ UART stage counts) into findings, "why is no command received?" answered stage by stage
  uart_stats()  the same stages counted in the satellite's debug UART text
  test cubusrx  sends a command from the ground station for one satellite cycle and compares the satellite's counters before / after
  pin check     runs tests/host/test_cubus_pins.py (pin ownership of the image)
Everything pure (decoders, diagnose, uart_stats) is covered by tests/host/test_testbench_cubus.py.
"""
import os
import re
import struct
import subprocess
import sys
import threading
import time

from tb.bus import EVENTS, log
from tb.links import GS, OBC
from tb.profiling import PROF
from tb.state import set_result
from tb.tests_extra import Swd
from tb.util import ROOT, SAT_BUILD, now

BOARD_IDS = {0xB0A2D001: "Nucleo-WL55JC", 0xB0A2D002: "custom front end (reference board)", 0xB0A2D003: "RSMS CUBUS V.3"}
CUBUS_ID = 0xB0A2D003
APP_STATES = {0: "CW beacon (deaf)", 1: "listening for commands", 2: "command downlink (deaf)"}
RX_MAGIC = 0x52585242          # "RXRB": the command ring is initialised by the M4
TX_MAGIC = 0x54585242          # "TXRB": the telemetry ring is initialised by the M4
FLASH_MAGIC = 0x46445442       # "FDTB"
OBC_MAGIC = 0x4F424331         # "OBC1"
RING_RX_DEPTH = 4
IPC_RING_TX, IPC_RING_RX, IPC_FLASH, IPC_OBC = 0x20008300, 0x20008500, 0x20008840, 0x20008F00
GPIOA, GPIOC = 0x48000000, 0x48000800
BEACON_PERIOD_MS = 90000

# CUBUS builds of the satellite image can not be used with these tests (they talk to the Nucleo bench app on CPU1 over the ST-Link VCP)
CUBUS_BLOCKED = {"image": "stores the picture in CPU1's LittleFS through the s2s2obc bench app",
                 "imageloop": "stores the picture in CPU1's LittleFS through the s2s2obc bench app",
                 "sdrimage": "stores the picture in CPU1's LittleFS through the s2s2obc bench app",
                 "digi": "injects frames through the s2s2obc bench app",
                 "rate": "is started through the s2s2obc bench app",
                 "cwtest": "is started through the s2s2obc bench app",
                 "vcpfuzz": "tests the s2s2obc VCP parser",
                 "flashio": "writes CPU1's LittleFS through the s2s2obc bench app",
                 "lfsproof": "looks for the picture in CPU1's LittleFS",
                 "e2e": "needs the bench app on CPU1 (image path, replay reset)"}

RX_FIELDS = ["magic", "rx_done", "rx_timeout", "rx_error", "bad_len", "decode_fail", "decode_ok", "digi", "validate_fail", "own_echo", "empty",
             "auth_fail", "not_tc", "incomplete", "accepted", "ring_ok", "ring_fail", "last_tick"]
RX_SIZE = 4 * len(RX_FIELDS) + 16


# ----------------------------------------------------------------------------------------------------------------- pure decoders
def board_name(bid):
    return BOARD_IDS.get(bid, "unknown id 0x%08X" % bid) if bid is not None else None


def parse_rxstats(raw):
    """g_satRx (cpu2/satellite/app/satellite_main.c struct sat_rx_stats_s) -> dict."""
    if raw is None or len(raw) < RX_SIZE:
        return None
    vals = struct.unpack("<%dI" % len(RX_FIELDS), raw[:4 * len(RX_FIELDS)])
    d = dict(zip(RX_FIELDS, vals))
    d["last_cmd"] = raw[4 * len(RX_FIELDS):RX_SIZE].hex()[:26]
    d["valid"] = d["magic"] == 0x52583031
    return d


def decode_gpio(a_moder, a_odr, c_moder, c_odr):
    """Pins the radio owns on CUBUS: PA0 (5 V rail), PC2 / PC3 (3.3 V / 5 V PA supply select), PC4 / PC5 (RF switch FE_CTRL1 / FE_CTRL2)."""
    def is_out(m, pin):
        return ((m >> (2 * pin)) & 3) == 1

    def bit(o, pin):
        return (o >> pin) & 1
    c4, c5 = bit(c_odr, 4), bit(c_odr, 5)
    rf = {(1, 0): "RX", (0, 1): "TX", (0, 0): "off", (1, 1): "both high (not a valid state)"}[(c4, c5)]
    return {"rail_5v": {"pin": "PA0", "output": is_out(a_moder, 0), "high": bool(bit(a_odr, 0))},
            "pa_3v3": {"pin": "PC2", "output": is_out(c_moder, 2), "high": bool(bit(c_odr, 2))},
            "pa_5v": {"pin": "PC3", "output": is_out(c_moder, 3), "high": bool(bit(c_odr, 3))},
            "fe_ctrl1": {"pin": "PC4", "output": is_out(c_moder, 4), "high": bool(c4)},
            "fe_ctrl2": {"pin": "PC5", "output": is_out(c_moder, 5), "high": bool(c5)},
            "rf_switch": rf, "pc_outputs": all(is_out(c_moder, p) for p in (2, 3, 4, 5))}


def parse_ring(raw, depth=RING_RX_DEPTH):
    """magic, write_index, read_index of a shared ring (+ pending count)."""
    if raw is None or len(raw) < 12:
        return None
    magic, w, r = struct.unpack("<III", raw[:12])
    return {"magic": magic, "write": w, "read": r, "pending": (w - r) % depth if depth else 0}


def parse_obc(raw):
    if raw is None or len(raw) < 32:
        return None
    f = struct.unpack("<8I", raw[:32])
    return dict(zip(["magic", "cpu2_beat", "session", "tx_req_seq", "tx_done_seq", "tx_status", "reset_req", "reset_ack"], f))


UART_PATTERNS = [("raw", r"STEP 1: READ RAW"), ("decode_fail", r"DECODE FAILED"), ("decode_ok", r"DECODE SUCCESS"), ("validate_fail", r"VALIDATE FAILED"),
                 ("validate_ok", r"VALIDATE SUCCESS"), ("accepted", r"VALID COMMAND ACCEPTED"), ("rejected", r"COMMAND RECEIVED: NO"),
                 ("incomplete", r"COMMAND RECEIVED: INCOMPLETE"), ("ring_fail", r"command NOT written|NOT written to"), ("digi", r"\[DIGI\]"),
                 ("tx_timeout", r"TX TIMEOUT|TX timeout"), ("rx_error", r"RX ERROR")]


def uart_stats(lines):
    """Count the receive stages in the satellite's debug UART text (list of (t, text) or str) and keep the last interesting lines."""
    counts = {k: 0 for k, _ in UART_PATTERNS}
    last = []
    rx = [(k, re.compile(p)) for k, p in UART_PATTERNS]
    for ln in lines:
        txt = ln[1] if isinstance(ln, (tuple, list)) else ln
        hit = False
        for k, p in rx:
            if p.search(txt):
                counts[k] += 1
                hit = True
        m = re.search(r"RSSI=(-?\d+) dBm, SNR=(-?\d+) dB", txt)
        if m:
            counts["last_rssi"], counts["last_snr"] = int(m.group(1)), int(m.group(2))
        if hit:
            last.append(txt.strip()[:160])
    return {"counts": counts, "last": last[-14:]}


def finding(level, title, text, stage=""):
    return {"level": level, "title": title, "text": text, "stage": stage}


def _delta(a, b, k):
    if a is None or b is None or a.get(k) is None or b.get(k) is None:
        return None
    return (b[k] - a[k]) & 0xFFFFFFFF


def diagnose(s, prev=None, uart=None, expect_id=None):
    """Findings for one snapshot (and the counters' change since `prev`). Pure: no hardware. Level: ok / info / warn / bad."""
    out = []
    add = lambda *a: out.append(finding(*a))                       # noqa: E731

    # ---- image / board
    bid = s.get("board_id")
    if bid is None:
        add("warn", "Board id not readable", "g_boardId is not in the ELF: build the satellite image again (this repository's current source) and flash it, "
            "otherwise the SWD readout below can not be matched to the firmware on the board.", "image")
    elif expect_id and bid != expect_id:
        add("bad", "The flashed image is for another board", f"The board runs an image built for {board_name(bid)}, the profile in .s2s2.config expects {board_name(expect_id)}. "
            "Rebuild with `make flash sat CUBUS=T` (or CUBUS=F for a Nucleo) and flash again. A Nucleo image on the CUBUS board drives pins that belong to CPU1.", "image")
    else:
        add("ok", "Image matches the profile", f"g_boardId = 0x{bid:08X} ({board_name(bid)}).", "image")

    # ---- is CPU2 running
    if s.get("alive") is False:
        add("bad", "CPU2 is not running", "The CPU2 tick and heartbeat did not advance in 0.5 s. Check the CPU2 boot setup: option bytes C2OPT = 1 and SBRV = 0xC800, CPU1 must set "
            "PWR_CR4.C2BOOT (test 'bootcfg' checks all three). Nothing below this line means anything until CPU2 runs.", "boot")
        return out
    if s.get("alive"):
        add("ok", "CPU2 is running", f"uptime {s['state'].get('uptime_s', '?')} s.", "boot")

    st, rx, gp, ring = s.get("state") or {}, s.get("rx"), s.get("gpio"), s.get("ring_rx")
    name = st.get("name")
    if name:
        extra = ""
        if st.get("app_state") == 0:
            extra = " It transmits the CW beacon for about 30 to 37 s of every 90 s and hears nothing meanwhile: a command sent now is lost. Try again after the beacon."
        elif st.get("app_state") == 2:
            extra = " It is answering a command and hears nothing meanwhile."
        elif st.get("cmd_service_active"):
            extra = " A command session is active: new commands are ignored until it ends."
        add("info", "State machine", f"{name}; {st.get('since_beacon_s', '?')} s since the last beacon started (period {BEACON_PERIOD_MS // 1000} s).{extra}", "timing")

    # ---- RF switch and PA rails
    if gp:
        listening = st.get("app_state") == 1 and not st.get("cmd_service_active") and not st.get("dcdc_hold")
        if listening:
            if gp["rf_switch"] != "RX":
                add("bad", "RF switch is not in RX while listening", f"PC4 = {int(gp['fe_ctrl1']['high'])}, PC5 = {int(gp['fe_ctrl2']['high'])} ({gp['rf_switch']}); receive needs "
                    "PC4 = 1, PC5 = 0. The satellite can not hear anything with the switch in this state. Check the switch table against the CUBUS switch part (docs/CUBUS.md) "
                    "and that nothing else drives PC4 / PC5.", "rf")
            else:
                add("ok", "RF switch is in RX", "PC4 = 1, PC5 = 0 while listening.", "rf")
            if gp["pa_5v"]["high"] or gp["pa_3v3"]["high"]:
                add("warn", "A PA supply is on while listening", f"PC2 (3.3 V) = {int(gp['pa_3v3']['high'])}, PC3 (5 V) = {int(gp['pa_5v']['high'])}. On CUBUS both should be off in RX.", "rf")
        if not gp["pc_outputs"]:
            add("warn", "PC2..PC5 are not all outputs", "The RF switch / PA select pins are not configured as outputs: RBI_Init did not run, or the GPIO clock of CPU2 is off.", "rf")

    # ---- the receive path, stage by stage
    if rx is None or not rx.get("valid"):
        add("warn", "No receive counters", "g_satRx is missing or has no magic word: the image on the board is older than the DEBUG counters. Flash a current build.", "rx")
    else:
        d = (lambda k: _delta(prev["rx"], rx, k)) if prev and prev.get("rx") and prev["rx"].get("valid") else (lambda k: rx[k])
        span = "since the first snapshot" if prev else "since boot"
        n_done = d("rx_done")
        if n_done == 0:
            if rx["rx_error"] or rx["rx_timeout"]:
                add("warn", "Radio errors", f"{rx['rx_error']} RX error and {rx['rx_timeout']} RX timeout flag(s) so far: the radio sees something it can not take as a packet.", "rx")
            add("bad" if (s.get("state") or {}).get("uptime_s", 0) > 100 else "info", "Nothing was delivered by the radio",
                f"0 packets {span}. Either nothing was sent in a listen window, or the signal does not reach the radio. In order: (1) was a command sent while the satellite listens "
                "(not during the CW beacon)? (2) uplink frequency 437.375 MHz on both ends, (3) RF switch RX state (above), (4) antenna / RF path and RX network (RFI_P / RFI_N), "
                "(5) clock (TCXO) stable, (6) ground station really transmits (its 'UPLINK COMPLETE' line, power).", "rx")
        else:
            add("ok", "The radio delivers packets", f"{n_done} packet(s) {span}; last RSSI {(s.get('radio') or {}).get('rssi')} dBm, SNR {(s.get('radio') or {}).get('snr')} dB.", "rx")
            n_ok, n_fail = d("decode_ok"), d("decode_fail")
            if n_ok == 0:
                add("bad", "Packets arrive but none decodes", f"{n_fail} packet(s) failed G3RUH / HDLC decoding. Typical causes: frequency offset between the two TCXOs, wrong bit rate or "
                    "deviation, signal too weak (RSSI above), the packet cut by the beacon or a transmit, the ground station sending a different framing (check its AUTH / flags setting).", "decode")
            else:
                add("ok", "Frames decode", f"{n_ok} frame(s) decoded, {n_fail} failed.", "decode")
                rej = {k: d(k) for k in ("validate_fail", "own_echo", "empty", "auth_fail", "not_tc")}
                acc = d("accepted")
                if acc == 0:
                    why = ", ".join(f"{v} {k.replace('_', ' ')}" for k, v in rej.items() if v) or "digipeater traffic only"
                    hint = ""
                    if rej["auth_fail"]:
                        hint = " AUTH is on and the trailer is missing or wrong: both boards need the same AUTH setting and key, and after a ground station reboot the replay floor must be reset."
                    elif rej["validate_fail"]:
                        hint = " FCS or structure failed: bit errors, or not an AX.25 UI frame with control 0x03 / PID 0xF0."
                    elif rej["not_tc"]:
                        hint = " The payload does not start with the SAT-ID 0x53."
                    add("bad", "Frames decode but no command is accepted", f"Rejected: {why}.{hint}", "validate")
                else:
                    add("ok", "Commands accepted", f"{acc} command(s) accepted; last one {rx.get('last_cmd')}.", "validate")
                    r_ok, r_fail = d("ring_ok"), d("ring_fail")
                    if r_fail:
                        add("bad", "Commands accepted but not handed to the OBC", f"{r_fail} command(s) could not be written to the shared command ring: the M4 has not initialised it, or it is full "
                            "(the OBC does not read it). The ground station still got its ACKs, the OBC never saw the command.", "handoff")
                    elif r_ok:
                        add("ok", "Handed to the OBC", f"{r_ok} command(s) written to the command ring and the IPCC doorbell rung.", "handoff")
            inc = d("incomplete")
            if inc:
                add("warn", "Incomplete commands", f"{inc} frame(s) started with 0x53 but were not 13 bytes: NACK 0x04 sent.", "validate")

    # ---- the OBC side of the hand-off
    if ring is not None:
        if ring["magic"] != RX_MAGIC:
            add("bad", "The OBC has not initialised the command ring", f"RX ring magic 0x{ring['magic']:08X} (expected 0x{RX_MAGIC:08X}): the M4 never ran rb_ipc_init(), so no command can be "
                "handed over. Is the OBC firmware running on CPU1?", "handoff")
        elif ring["pending"]:
            add("warn", "The OBC is not reading the command ring", f"{ring['pending']} command(s) wait in the ring (write {ring['write']}, read {ring['read']}); it holds 3.", "handoff")
        else:
            add("ok", "Command ring is empty", "everything handed over was taken by the OBC.", "handoff")

    if uart:
        c = uart["counts"]
        add("info", "Debug UART stage counts", ", ".join(f"{k} {v}" for k, v in c.items() if v and k not in ("last_rssi", "last_snr")) or "nothing seen on the UART (is it connected?)", "uart")
    return out


# ----------------------------------------------------------------------------------------------------------------- profile
def read_opts():
    try:
        return open(os.path.join(SAT_BUILD, ".opts")).read().strip()
    except OSError:
        return ""


def profile():
    from tb.compat import CORE
    cfg = CORE.load_config()
    opts = read_opts()
    on = bool(cfg.get("cubus"))
    exp = CUBUS_ID if on else (0xB0A2D001 if cfg.get("frontend") == "NUCLEO" else 0xB0A2D002)
    return {"cubus": on, "frontend": cfg.get("frontend"), "expect_id": exp, "expect_name": board_name(exp), "build_opts": opts,
            "build_is_cubus": "-CUBUS-" in opts, "elf": os.path.exists(os.path.join(SAT_BUILD, "satellite.elf")),
            "sat_serial": cfg.get("sat_serial") or ""}


# ----------------------------------------------------------------------------------------------------------------- SWD
class DebugSwd:
    """Keeps one OpenOCD to the board's ST-Link open between snapshots (opening takes seconds); closed on request, when the profiler or a flash needs the probe, or after idling."""
    IDLE_S = 120

    def __init__(self):
        self.sw = None
        self.lock = threading.Lock()
        self.t_last = 0.0

    def get(self):
        with self.lock:
            if self.sw is None:
                self.sw = Swd("sat").__enter__()
            self.t_last = now()
            return self.sw

    def close(self):
        with self.lock:
            sw, self.sw = self.sw, None
        if sw is not None:
            try:
                sw.__exit__(None, None, None)
            except Exception:
                pass

    def reap(self):
        if self.sw is not None and now() - self.t_last > self.IDLE_S:
            self.close()

    @property
    def open(self):
        return self.sw is not None


SWD = DebugSwd()


def _rd(sw, addr, n):
    if addr is None:
        return None
    try:
        return sw.dump(addr, n)
    except RuntimeError:
        return None


def read_snapshot(sw, gap_s=0.5):
    """One look at the satellite over SWD (reads only). Addresses come from the CPU2 ELF in build/satellite."""
    elf = PROF._elf(os.path.join(SAT_BUILD, "satellite.elf"))
    sym = elf.syms.get

    def u(name, n=4, signed=False):
        b = _rd(sw, sym(name), n)
        return None if b is None else int.from_bytes(b, "little", signed=signed)
    snap = {"t": now(), "elf": elf.ok}
    snap["board_id"] = u("g_boardId")
    tick0, beat0 = u("uwTick"), (parse_obc(_rd(sw, IPC_OBC, 32)) or {}).get("cpu2_beat")
    time.sleep(gap_s)
    tick1 = u("uwTick")
    obc = parse_obc(_rd(sw, IPC_OBC, 32))
    snap["alive"] = None if tick0 is None or tick1 is None else (tick1 != tick0 or (beat0 is not None and obc and obc["cpu2_beat"] != beat0))
    app = u("app_state")
    since = None
    bs = u("s_beacon_start_tick")
    if tick1 is not None and bs is not None:
        since = round(((tick1 - bs) & 0xFFFFFFFF) / 1000.0, 1)
    snap["state"] = {"app_state": app, "name": APP_STATES.get(app, "?" if app is None else "state %d" % app), "tick": tick1, "uptime_s": None if tick1 is None else round(tick1 / 1000.0, 1),
                     "since_beacon_s": since, "cmd_service_active": bool(u("s_cmd_service_active", 1) or 0), "dcdc_hold": bool(u("s_dcdc_hold", 1) or 0),
                     "pending_cmd_len": u("s_pending_cmd_len", 2), "cw_step": u("s_cw_step", 1)}
    snap["radio"] = {"rx_done_flag": u("rx_done_flag", 1), "rx_frame_size": u("rx_frame_size", 2), "rssi": u("last_rx_rssi", 2, True), "snr": u("last_rx_snr", 1, True),
                     "tx_busy": u("tx_busy", 1)}
    snap["rx"] = parse_rxstats(_rd(sw, sym("g_satRx"), RX_SIZE))
    ga, gc = _rd(sw, GPIOA, 0x18), _rd(sw, GPIOC, 0x18)
    if ga and gc:
        snap["gpio"] = decode_gpio(struct.unpack_from("<I", ga, 0)[0], struct.unpack_from("<I", ga, 0x14)[0], struct.unpack_from("<I", gc, 0)[0], struct.unpack_from("<I", gc, 0x14)[0])
    snap["ring_rx"] = parse_ring(_rd(sw, IPC_RING_RX, 12))
    snap["ring_tx"] = parse_ring(_rd(sw, IPC_RING_TX, 12))
    snap["ring_flash"] = parse_ring(_rd(sw, IPC_FLASH, 12), 12)
    snap["obc"] = obc
    snap["selftests"] = {"seal": u("g_sealSelfTest"), "digi": u("g_digiSelfTest")}
    return snap


_last = {"snap": None}


def snapshot_report(prev_ok=True):
    """What the page shows: a fresh snapshot, its diagnosis (counters compared with the previous snapshot when there is one), the UART stage counts."""
    prof = profile()
    sw = SWD.get()
    snap = read_snapshot(sw)
    prev = _last["snap"] if prev_ok else None
    _last["snap"] = snap
    uart = uart_stats(list(OBC.lines)[-1500:]) if OBC.up() else None
    return {"profile": prof, "snapshot": snap, "previous_t": prev["t"] if prev else None,
            "findings": diagnose(snap, prev, uart, prof["expect_id"]), "uart": uart}


def uart_report():
    uart = uart_stats(list(OBC.lines)[-3000:])
    return {"uart": uart, "connected": OBC.up(), "port": OBC.port}


# ----------------------------------------------------------------------------------------------------------------- pin ownership check
def run_pin_check():
    """tests/host/test_cubus_pins.py: source scan + a CUBUS image build in a temporary directory (about a minute)."""
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "host", "test_cubus_pins.py")], capture_output=True, text=True, timeout=600,
                       env={k: v for k, v in os.environ.items() if k not in ("CPATH", "C_INCLUDE_PATH")})
    return {"ok": r.returncode == 0, "output": (r.stdout + r.stderr).strip()[-4000:]}


# ----------------------------------------------------------------------------------------------------------------- guided test
def _row(name, ok, detail, expected=""):
    return {"name": name, "state": "PASS" if ok is True else "FAIL" if ok is False else "INFO", "detail": detail, "expected": expected}


def test_cubusrx(cfg, abort):
    """Uplink path of the satellite, stage by stage. The ground station sends one command every few seconds for `seconds`; the satellite's counters
    are read over SWD before and after, so the result says WHERE a command was lost (radio, decode, validation, hand-off to the OBC), not just that no
    ACK came. Needs the ground station (UART) and the satellite's ST-Link."""
    if not GS.connected():
        raise RuntimeError("connect the ground station's serial port first")
    cmd = str(cfg.get("command") or "HK1").strip()
    if not re.fullmatch(r"[A-Za-z0-9 ]{1,40}", cmd):
        raise RuntimeError("command must be letters / digits / spaces (a ground-station console command such as HK1, PING, SNAP)")
    seconds = max(30, min(int(cfg.get("seconds") or 100), 600))
    every = max(4, min(int(cfg.get("every_s") or 8), 30))
    prof = profile()
    sw = SWD.get()
    base = read_snapshot(sw, 0.3)
    t0, sent = now(), 0
    log(f"CUBUS RX test: sending '{cmd}' every {every} s for {seconds} s (a full satellite cycle is 90 s)", "ok")
    while now() - t0 < seconds and not abort.is_set():
        GS.send(cmd)
        sent += 1
        abort.wait(every)
    abort.wait(4)
    end = read_snapshot(sw, 0.3)
    acks = [e for e in EVENTS.recent(limit=1500) if e["t"] >= t0 and e["kind"] in ("ack", "nack")]
    findings = diagnose(end, base, uart_stats(list(OBC.lines)[-1500:]) if OBC.up() else None, prof["expect_id"])
    rx0, rx1 = base.get("rx") or {}, end.get("rx") or {}

    def d(k):
        return _delta(rx0, rx1, k)
    rows = [_row("commands sent by the ground station", sent > 0, f"{sent} x '{cmd}' in {now() - t0:.0f} s", ">= 1"),
            _row("radio delivered packets", (d("rx_done") or 0) > 0, f"{d('rx_done')} packet(s); RSSI {(end.get('radio') or {}).get('rssi')} dBm", "> 0"),
            _row("packets decoded", (d("decode_ok") or 0) > 0, f"{d('decode_ok')} decoded, {d('decode_fail')} failed", "> 0"),
            _row("commands accepted", (d("accepted") or 0) > 0, f"{d('accepted')} accepted (rejected: validate {d('validate_fail')}, AUTH {d('auth_fail')}, not 0x53 {d('not_tc')})", "> 0"),
            _row("handed to the OBC ring", (d("ring_fail") or 0) == 0 and (d("ring_ok") or 0) > 0, f"{d('ring_ok')} written, {d('ring_fail')} failed", "all written"),
            _row("ACK / NACK heard at the ground station", len(acks) > 0, f"{len(acks)} ACK / NACK event(s)", "> 0")]
    for f in findings:
        if f["level"] in ("bad", "warn"):
            rows.append(_row("finding: " + f["title"], False if f["level"] == "bad" else None, f["text"]))
    verdict = "FAIL" if any(r["state"] == "FAIL" for r in rows) else "PASS"
    res = {"test": "cubusrx", "when": now(), "verdict": verdict, "rows": rows, "command": cmd, "sent": sent, "findings": findings,
           "before": rx0, "after": rx1}
    set_result("cubusrx", res)
    log(f"CUBUSRX {verdict}: " + "; ".join(f"{r['name']} {r['state']}" for r in rows[:6]), "ok" if verdict == "PASS" else "err")


TESTS = {"cubusrx": test_cubusrx}
NEEDS = {"cubusrx": "gs"}
