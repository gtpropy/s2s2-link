#!/usr/bin/env python3
"""
board_guard.py -- refuse to flash a Nucleo image onto the CUBUS flight board.

Called by `make flash` and `make flash-m4` (cpu2/common/cpu2.mk) before anything is written. It reads the attached STM32WL55 over SWD
and decides whether the board is a CUBUS (RSMS CUBUS V.3) board:

  1. the CPU2 flash holds the CUBUS board id word 0xB0A2D003 (`g_boardId` of a CUBUS=T image), or
  2. the chip's 96-bit unique id is listed in tools/flash/cubus_boards.txt (boards that still run other firmware, e.g. the reference
     firmware; add one with `python3 tools/flash/board_guard.py --register-cubus` while only that board is connected).

If the board is a CUBUS and the image being flashed is NOT a CUBUS image (Nucleo satellite, ground station, NuttX CPU1 app) the
guard prints a big ERROR and exits 1: the make target stops, nothing is written. There is no override switch on purpose. If the board
cannot be read (no probe, readout protection, no answer) the guard also refuses: it does not guess.

  board_guard.py --image cubus|nucleo|gs|nuttx [--serial S] [--what flash|flash-m4]    check, then exit 0 (allowed) or 1 (refused)
  board_guard.py --status [--serial S]                                                   only print what is detected
  board_guard.py --register-cubus [--serial S]                                           add the attached board to the registry
  board_guard.py --print                                                                 print the OpenOCD script, touch nothing

Access is the same read-only method as tools/flash/swd_rx_check.sh: a bare MEM-AP target (NOT target/stm32wlx.cfg, which writes
DBGMCU registers on attach), no reset, no halt, no write of any kind, 500 kHz, reads of the chip id, the unique id and the CPU2 flash
only. The script is scanned for write / reset / halt / flash commands before OpenOCD starts.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REGISTRY = os.path.join(HERE, "cubus_boards.txt")

CUBUS_ID = 0xB0A2D003                   # BOARD_ID of the CUBUS profile (cpu2/satellite/board/radio_board_if.h)
IDCODE_ADDR = 0xE0042000                # DBGMCU_IDCODE; DEV_ID 0x497 = STM32WL55/54
DEV_ID_WL = 0x497
UID_ADDR = 0x1FFF7590                   # 96-bit unique device id (3 words)
C2_FLASH = 0x08032000                   # CPU2 image
C2_WORDS = 56 * 1024 // 4
CHUNK = 1024                            # words per read_memory

BANNER = """
############################################################################
##                                                                        ##
##   ERROR!!  You made a mistake, dont flash nucleo in CUBUS              ##
##                                                                        ##
############################################################################"""

TCL = """\
set ID_ADDR 0x%08X
set UID_ADDR 0x%08X
set FLASH_ADDR 0x%08X
set FLASH_WORDS %d
set CHUNK %d
proc w {a n} { return [read_memory $a 32 $n] }
set id [expr {[lindex [w $ID_ADDR 1] 0] & 0xFFFFFFFF}]
puts [format "GUARD_ID=%%08X" $id]
set uid {}
foreach x [w $UID_ADDR 3] { lappend uid [format "%%08X" [expr {$x & 0xFFFFFFFF}]] }
puts "GUARD_UID=[join $uid {}]"
set found 0
set a $FLASH_ADDR
set left $FLASH_WORDS
while {$left > 0} {
    set n [expr {$left < $CHUNK ? $left : $CHUNK}]
    foreach x [w $a $n] {
        if {[expr {$x & 0xFFFFFFFF}] == 0xB0A2D003} { incr found }
    }
    incr a [expr {$n * 4}]
    incr left -$n
}
puts "GUARD_CUBUS_WORDS=$found"
puts "GUARD_DONE=1"
""" % (IDCODE_ADDR, UID_ADDR, C2_FLASH, C2_WORDS, CHUNK)

CFG = """\
source [find target/swj-dp.tcl]
swj_newdap chip cpu -irlen 4 -ircapture 0x1 -irmask 0xf -expected-id 0x6ba02477
dap create chip.dap -chain-position chip.cpu
target create chip.ahb mem_ap -dap chip.dap -ap-num 0
"""

FORBIDDEN = re.compile(r"\b(mww|mwh|mwb|mwd|mmw|write_memory|write_image|load_image|restore_image|reset|halt|resume|step|flash|program|"
                       r"verify|bp|wp|rbp|rwp|apreg|dpreg|dap|irscan|drscan|pathmove|jtag|swd|cortex_m|exec|open|socket|shutdown|init|"
                       r"adapter|transport)\b")


def script_is_read_only(tcl):
    """The generated script must contain no command that could write, reset, halt or program the target."""
    return [ln for ln in tcl.splitlines() if FORBIDDEN.search(ln)]


def adapter_select(serial):
    if serial.startswith("loc:"):
        return "adapter usb location " + serial[4:]
    if serial and not serial.startswith("any:"):
        return "adapter serial " + serial
    return "echo using_the_only_attached_ST-Link"


def read_board(serial, timeout=60):
    """Read-only look at the attached chip. Returns {'devid', 'uid', 'cubus_words'} or raises RuntimeError."""
    if script_is_read_only(TCL):
        raise RuntimeError("internal error: the guard script contains a forbidden command")
    with tempfile.TemporaryDirectory(prefix="boardguard.") as tmp:
        cfg, tcl = os.path.join(tmp, "passive.cfg"), os.path.join(tmp, "guard.tcl")
        open(cfg, "w").write(CFG)
        open(tcl, "w").write(TCL)
        cmd = ["openocd", "-f", "interface/stlink-dap.cfg", "-c", "transport select dapdirect_swd", "-c", adapter_select(serial),
               "-c", "adapter speed 500", "-c", "reset_config none", "-f", cfg, "-c", "init", "-f", tcl, "-c", "shutdown"]
        try:
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        except FileNotFoundError:
            raise RuntimeError("openocd not found (needed for the board check)")
        except subprocess.TimeoutExpired:
            raise RuntimeError("the board did not answer in %d s" % timeout)
    kv = dict(re.findall(r"^(GUARD_[A-Z_]+)=(\S+)", r.stdout, re.M))
    if r.returncode != 0 or kv.get("GUARD_DONE") != "1":
        tail = [ln for ln in r.stderr.splitlines() if not ln.startswith("Info :")][-3:]
        raise RuntimeError("could not read the board over SWD (%s)" % ("; ".join(tail) or "no output"))
    return {"devid": int(kv["GUARD_ID"], 16), "uid": kv["GUARD_UID"].upper(), "cubus_words": int(kv["GUARD_CUBUS_WORDS"])}


def load_registry(path=REGISTRY):
    """Unique ids (24 hex digits) of the CUBUS boards that are known by registry, one per line, '#' starts a comment."""
    ids = set()
    if os.path.exists(path):
        for ln in open(path):
            m = re.match(r"\s*([0-9A-Fa-f]{24})\b", ln)
            if m:
                ids.add(m.group(1).upper())
    return ids


def classify(info, registry):
    """(is_cubus, reasons) for a board read by read_board(). Pure function (unit tested)."""
    reasons = []
    if info["cubus_words"] > 0:
        reasons.append("CPU2 flash holds the CUBUS board id 0x%08X" % CUBUS_ID)
    if info["uid"].upper() in registry:
        reasons.append("unique id %s is registered as CUBUS (tools/flash/cubus_boards.txt)" % info["uid"])
    return bool(reasons), reasons


def decide(image, what, is_cubus):
    """Allowed? image: cubus | nucleo | gs | nuttx. A CUBUS board accepts only the CUBUS CPU2 image; the NuttX CPU1 image is never flashed from here."""
    if not is_cubus:
        return True
    return image == "cubus" and what == "flash"


def refuse(info, reasons, image, what):
    print(BANNER)
    print("  Attached board : CUBUS flight board (RSMS CUBUS V.3), unique id %s" % info["uid"])
    for r in reasons:
        print("                   - " + r)
    print("  You asked for  : %s (%s)" % ("flash CPU1 NuttX app" if what == "flash-m4" else "flash CPU2", {
        "cubus": "CUBUS image", "nucleo": "NUCLEO image", "gs": "ground-station image", "nuttx": "NuttX image"}.get(image, image)))
    if what == "flash-m4":
        print("  The CPU1 firmware of the flight board is the OBC firmware. The Nucleo app drives its LED pins = SPI / chip selects / kill switch.")
    else:
        print("  A Nucleo build drives PB6 (= KILL_SWITCH1_-), PB15 / PB9 / PB11 (= SPI lines) and the wrong RF front end on this board.")
    print("  NOTHING WAS WRITTEN. The flash is LOCKED for this board. Use:  make flash sat CUBUS=T")
    print("  Wrong guess? This is not the CUBUS board: remove its unique id from tools/flash/cubus_boards.txt (and do not flash a CUBUS image on it).")
    sys.stdout.flush()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--image", choices=["cubus", "nucleo", "gs", "nuttx"])
    ap.add_argument("--what", choices=["flash", "flash-m4"], default="flash")
    ap.add_argument("--serial", default="")
    ap.add_argument("--status", action="store_true")
    ap.add_argument("--register-cubus", action="store_true")
    ap.add_argument("--print", dest="print_only", action="store_true")
    a = ap.parse_args(argv)

    if a.print_only:
        print("# ---- passive.cfg\n" + CFG + "# ---- guard.tcl\n" + TCL)
        bad = script_is_read_only(TCL)
        print("# forbidden commands in script: %s" % (bad or "none"))
        return 1 if bad else 0
    if not (a.image or a.status or a.register_cubus):
        ap.error("give --image, --status or --register-cubus")

    try:
        info = read_board(a.serial)
    except RuntimeError as e:
        if a.status or a.register_cubus:
            print("board check failed: %s" % e)
            return 1
        print(BANNER)
        print("  FLASH LOCKED: the guard could not tell what board is attached, so it does not allow a write.")
        print("  Reason: %s" % e)
        print("  Check the probe / cable / SERIAL=, close other programs that hold the ST-Link (profiler, OpenOCD), then try again.")
        return 1
    if info["devid"] & 0xFFF != DEV_ID_WL:
        print("board check: DEV_ID 0x%03X is not an STM32WL55 (0x%03X)" % (info["devid"] & 0xFFF, DEV_ID_WL))
        if not a.status:
            return 1
    registry = load_registry()
    is_cubus, reasons = classify(info, registry)

    if a.status:
        print("unique id %s: %s" % (info["uid"], "CUBUS board (%s)" % "; ".join(reasons) if is_cubus else "not recognised as CUBUS (no CUBUS image, not registered)"))
        return 0
    if a.register_cubus:
        if info["uid"] in registry:
            print("already registered: %s" % info["uid"])
            return 0
        with open(REGISTRY, "a") as f:
            f.write("%s   # CUBUS flight board, registered %s\n" % (info["uid"], __import__("time").strftime("%Y-%m-%d")))
        print("registered %s in %s" % (info["uid"], REGISTRY))
        return 0

    if decide(a.image, a.what, is_cubus):
        print("board guard: %s" % ("CUBUS board, CUBUS image: allowed" if is_cubus else "not a CUBUS board (uid %s): allowed" % info["uid"]))
        return 0
    refuse(info, reasons, a.image, a.what)
    return 1


if __name__ == "__main__":
    sys.exit(main())
