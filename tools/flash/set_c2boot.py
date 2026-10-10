#!/usr/bin/env python3
"""
set_c2boot.py -- program the STM32WL55 CPU2 (Cortex-M0+) boot vector through
OpenOCD, so CPU2 starts from the CPU2 image at 0x08032000.

Encoding (ST HAL, stm32wlxx_hal_flash_ex.c FLASH_OB_Program):
    FLASH_SRRVR (0x58004084)
      SBRV[15:0] = (address - FLASH_BASE) >> 2
      C2OPT[31]  = 1  (1 = boot vector in FLASH, 0 = SRAM from 0x20000000)
    0x08032000 -> SBRV = 0xC800, C2OPT = 1

Usage: python3 tools/flash/set_c2boot.py <probe-serial> [address]
"""
import re
import subprocess
import sys
import time

FLASH_BASE = 0x08000000
SRRVR = 0x58004084
KEYR = 0x58004008
OPTKEYR = 0x5800400C
SR = 0x58004010
CR = 0x58004014
KEY1, KEY2 = 0x45670123, 0xCDEF89AB
OPTKEY1, OPTKEY2 = 0x08192A3B, 0x4C5D6E7F
CR_OPTSTRT = 1 << 17
CR_OBL_LAUNCH = 1 << 27
SR_BSY = 1 << 16


def ocd(serial, *cmds):
    args = ["openocd", "-f", "interface/stlink.cfg",
            "-c", "transport select hla_swd", "-c", f"adapter serial {serial}",
            "-f", "target/stm32wlx.cfg", "-c", "init"]
    for c in cmds:
        args += ["-c", c]
    args += ["-c", "exit"]
    out = subprocess.run(args, capture_output=True, text=True)
    return out.returncode, out.stdout + out.stderr


def read32(serial, addr):
    rc, out = ocd(serial, f"mdw 0x{addr:08x} 1")
    m = re.search(rf"0x{addr:08x}:\s+([0-9a-fA-F]{{8}})", out)
    if not m:
        raise SystemExit(f"read 0x{addr:08x} failed:\n{out}")
    return int(m.group(1), 16)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    serial = sys.argv[1]
    address = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x08032000

    sbrv = (address - FLASH_BASE) >> 2
    if sbrv > 0xFFFF:
        raise SystemExit("address outside flash range for SBRV")

    before = read32(serial, SRRVR)
    # keep the secure-SRAM fields, set SBRV, set C2OPT=1 (boot from flash)
    new = (before & 0x7FFF0000) | 0x80000000 | sbrv
    print(f"SRRVR before=0x{before:08X} new=0x{new:08X} (CPU2 boot 0x{address:08X})")

    # Unlock flash control and option registers
    ocd(serial,
        f"mww 0x{KEYR:08x} 0x{KEY1:08x}", f"mww 0x{KEYR:08x} 0x{KEY2:08x}",
        f"mww 0x{OPTKEYR:08x} 0x{OPTKEY1:08x}", f"mww 0x{OPTKEYR:08x} 0x{OPTKEY2:08x}")

    # Write the boot vector, start option programming, wait for busy to clear
    cr = read32(serial, CR)
    ocd(serial,
        f"mww 0x{SRRVR:08x} 0x{new:08x}",
        f"mww 0x{CR:08x} 0x{cr | CR_OPTSTRT:08x}")
    for _ in range(50):
        if not (read32(serial, SR) & SR_BSY):
            break
        time.sleep(0.1)

    # Load the option bytes (resets the chip) and check the result
    cr = read32(serial, CR)
    ocd(serial, f"mww 0x{CR:08x} 0x{cr | CR_OBL_LAUNCH:08x}")
    time.sleep(1.0)

    after = read32(serial, SRRVR)
    ok = (after & 0xFFFF) == sbrv and bool(after & 0x80000000)
    print(f"SRRVR after=0x{after:08X} -> {'OK' if ok else 'MISMATCH'}")
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
