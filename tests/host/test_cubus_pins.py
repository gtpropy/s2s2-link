#!/usr/bin/env python3
"""
CUBUS pin ownership check (no hardware).

On the RSMS CUBUS V.3 board the satellite CPU2 image may touch only the pins that belong to the radio (docs/CUBUS.md):
PA0 (EN_5V_DC/DC&OCP, owned by CPU2 as in the reference firmware), PC2 / PC3 (PA supply selects), PC4 / PC5 (RF switch), PA9 / PA10 (debug UART). Everything else on the chip
is wired to CPU1's buses, the kill switch, the burner, the watchdog and the power enables, and the Nucleo-only code (LEDs on PB15 / PB9 / PB11,
USART1 TX on PB6) must not be in a CUBUS image.

Two checks:
  1. source: the satellite sources are read with the SAT_CUBUS conditionals resolved; every GPIO access that is left must be on the allow list
  2. image: a CUBUS build must contain the CUBUS pieces and none of the Nucleo / BSP pin code (needs the ARM toolchain, skipped without it)
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
SAT = os.path.join(ROOT, "cpu2", "satellite")
COMMON = os.path.join(ROOT, "cpu2", "common")

# symbols whose value is known when SAT_CUBUS is on; any other condition keeps both branches (conservative)
KNOWN = {"SAT_CUBUS": True, "SAT_NUCLEO_FRONTEND": False}

OK_PIN_MACROS = {"DCDC_5V_EN_PIN", "AMP_3V3_EN_PIN", "AMP_5V_EN_PIN", "FE_CTRL1_PIN", "FE_CTRL2_PIN"}
OK_PORT_MACROS = {"GPIOC", "DCDC_5V_EN_PORT", "AMP_3V3_EN_PORT", "AMP_5V_EN_PORT", "FE_CTRL1_PORT", "FE_CTRL2_PORT"}
# files that may access GPIO registers by name: uart_debug.c only GPIOA (PA9 / PA10 are checked by the bit numbers below)
OK_REG_FILES = {"uart/uart_debug.c": ("GPIOA_",)}

CALL = re.compile(r"\b(HAL_GPIO_WritePin|HAL_GPIO_ReadPin|HAL_GPIO_Init|HAL_GPIO_DeInit|LL_GPIO_OutputInit)\s*\(")
REG = re.compile(r"\bGPIO[ABCDEH]_(MODER|OTYPER|OSPEEDR|PUPDR|AFRL|AFRH|ODR|IDR|BSRR)\b|\bGPIO[ABCDEH]\s*->|\b\w+_PORT\s*->")


def resolve(text):
    """Drop the lines that are not compiled when SAT_CUBUS is defined (handles #ifdef / #ifndef / #if defined / #else / #endif)."""
    out, stack = [], []          # stack of (parent_live, this_branch_live, taken_known)
    live = True
    for raw in text.split("\n"):
        s = raw.strip()
        m = re.match(r"#\s*(ifdef|ifndef|if|elif|else|endif)\b(.*)", s)
        if not m:
            if live:
                out.append(raw)
            continue
        kw, rest = m.group(1), m.group(2).strip()

        def cond(rest, kw):
            names = re.findall(r"[A-Za-z_]\w*", rest.replace("defined", ""))
            if kw in ("ifdef", "ifndef") and len(names) == 1 and names[0] in KNOWN:
                v = KNOWN[names[0]]
                return v if kw == "ifdef" else (not v)
            if kw in ("if", "elif") and len(names) == 1 and names[0] in KNOWN and re.fullmatch(r"(!\s*)?(defined\s*\(?\s*\w+\s*\)?)", rest.strip()):
                v = KNOWN[names[0]]
                return (not v) if rest.strip().startswith("!") else v
            return None          # unknown: keep both branches
        if kw in ("ifdef", "ifndef", "if"):
            c = cond(rest, kw)
            stack.append((live, c, c is not None))
            live = live and (True if c is None else c)
        elif kw in ("elif", "else"):
            parent, c, known = stack.pop()
            if kw == "else":
                nc = None if c is None else (not c)
            else:
                nc = cond(rest, "elif")
                if c is True and nc is None:
                    nc = False
            stack.append((parent, nc, c is not None))
            live = parent and (True if nc is None else nc)
        else:                    # endif
            parent, _, _ = stack.pop()
            live = parent
    return "\n".join(out)


def balanced_args(text, start):
    depth, i = 1, start
    while i < len(text) and depth:
        depth += {"(": 1, ")": -1}.get(text[i], 0)
        i += 1
    return text[start:i - 1]


def check_source():
    problems, calls = [], 0
    files = []
    for base in (SAT, COMMON):
        for d, _, fs in os.walk(base):
            if "/vendor" in d or "/build" in d:
                continue
            for f in fs:
                if f.endswith((".c", ".h")):
                    files.append(os.path.join(d, f))
    for path in sorted(files):
        rel = os.path.relpath(path, SAT) if path.startswith(SAT) else os.path.relpath(path, COMMON)
        if rel in ("board/stm32wlxx_nucleo_radio.c", "board/stm32wlxx_nucleo_radio.h", "board/stm32wlxx_nucleo.h"):
            continue                                   # ST BSP, never called: checked on the image below
        text = resolve(open(path, errors="replace").read())
        # defines are not accesses
        text = "\n".join(l for l in text.split("\n") if not re.match(r"\s*#\s*define\b", l))
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        text = re.sub(r"//[^\n]*", "", text)
        for m in CALL.finditer(text):
            calls += 1
            args = balanced_args(text, m.end())
            for tok in re.findall(r"\b[A-Za-z_]\w*\b", args):
                if tok.startswith(("GPIO_PIN_SET", "GPIO_PIN_RESET")) or tok in ("GPIO_PinState", "enable", "state", "port", "pins", "GPIOx", "GPIO_Pin",
                                                                              "PinState", "s_pa_enabled", "SAT_PA3V3_IDLE", "uint16_t", "GPIO_TypeDef"):
                    continue
                if tok.startswith("GPIO_PIN_") or (tok.startswith("GPIO") and tok not in OK_PORT_MACROS and not tok.startswith("GPIO_")):
                    problems.append("%s: GPIO call uses %s: %s" % (rel, tok, args.strip().replace("\n", " ")[:100]))
                elif tok in OK_PIN_MACROS or tok in OK_PORT_MACROS:
                    continue
        for m in REG.finditer(text):
            allowed = OK_REG_FILES.get(rel)
            if not allowed or not any(m.group(0).startswith(a.rstrip("-> ")) or m.group(0).replace(" ", "").startswith(a) for a in allowed):
                problems.append("%s: direct GPIO register access %r" % (rel, m.group(0)))
        if rel == "uart/uart_debug.c":
            # the debug UART may only configure PA9 / PA10: GPIOA_* lines must shift by 9 / 10 (or 18 / 20 bit pairs, 4 / 8 AFRH nibbles)
            for ln in text.split("\n"):
                if re.search(r"\bGPIOB_(MODER|OTYPER|OSPEEDR|PUPDR|AFRL|AFRH|ODR|BSRR)\b", ln):
                    problems.append("uart/uart_debug.c: GPIOB access in a CUBUS image (PB6 = KILL_SWITCH1_-): " + ln.strip())
    return problems, calls


def check_image():
    tc = shutil.which("arm-none-eabi-gcc")
    if not tc:
        cand = sorted(os.listdir("/Applications/ArmGNUToolchain")) if os.path.isdir("/Applications/ArmGNUToolchain") else []
        if cand:
            tc = os.path.join("/Applications/ArmGNUToolchain", cand[-1], "arm-none-eabi", "bin", "arm-none-eabi-gcc")
    if not tc or not os.path.exists(tc):
        print("cubus image check skipped: no ARM toolchain")
        return []
    nm = os.path.join(os.path.dirname(tc), "arm-none-eabi-nm")
    objdump = os.path.join(os.path.dirname(tc), "arm-none-eabi-objdump")
    out = tempfile.mkdtemp(prefix="cubus_")
    problems = []
    try:
        r = subprocess.run(["make", "-C", SAT, "CUBUS=T", "BUILD=" + out], capture_output=True, text=True,
                           env={k: v for k, v in os.environ.items() if k not in ("CPATH", "C_INCLUDE_PATH")})
        elf = os.path.join(out, "satellite.elf")
        if r.returncode or not os.path.exists(elf):
            return ["CUBUS build failed:\n" + r.stdout[-1500:] + r.stderr[-1500:]]
        syms = subprocess.run([nm, elf], capture_output=True, text=True).stdout
        names = {ln.split()[-1] for ln in syms.splitlines() if ln.strip()}
        for need in ("g_boardId", "Obc_Led"):
            if need not in names:
                problems.append("CUBUS image misses " + need)
        for bad in ("BSP_RADIO_Init", "BSP_RADIO_ConfigRFSwitch", "led_init"):
            if bad in names:
                problems.append("CUBUS image contains " + bad)
        dis = subprocess.run([objdump, "-d", "--disassemble=Obc_Led", elf], capture_output=True, text=True).stdout
        if "bx\tlr" not in dis or "str" in dis.split("<Obc_Led>:")[-1].split("\n\n")[0]:
            problems.append("Obc_Led is not an empty function in the CUBUS image")
        # the GPIOB alternate-function register (PB6 USART1_TX) must not be written: its address is not in any literal pool
        lit = subprocess.run([objdump, "-s", "-j", ".text", elf], capture_output=True, text=True).stdout
        flat = "".join(re.findall(r"^ [0-9a-f]{8} ((?:[0-9a-f]{8} ?){1,4})", lit, flags=re.M)).replace(" ", "")
        if "20040048" in flat:                         # 0x48000420 little endian
            problems.append("GPIOB_AFRL address (0x48000420) found in the CUBUS image")
    finally:
        shutil.rmtree(out, ignore_errors=True)
    return problems


def selftest():
    t = "a\n#ifndef SAT_CUBUS\nB\n#else\nC\n#endif\n#ifdef SAT_NUCLEO_FRONTEND\nD\n#endif\n#if FOO\nE\n#else\nF\n#endif\nG"
    got = resolve(t).split("\n")
    assert got == ["a", "C", "E", "F", "G"], got


if __name__ == "__main__":
    selftest()
    probs, calls = check_source()
    probs += check_image()
    if probs:
        print("CUBUS pin ownership FAILED:")
        for p in probs:
            print("  " + p)
        sys.exit(1)
    print("cubus pins OK (%d GPIO calls on the allow list, image checked)" % calls)
