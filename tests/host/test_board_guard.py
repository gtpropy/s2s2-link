#!/usr/bin/env python3
"""
Board guard (tools/flash/board_guard.py): a Nucleo image must never be flashed onto the CUBUS flight board.

No hardware is used. The decision logic is tested directly; the flow (read board -> refuse / allow, exit codes, message) runs with a
stub `openocd` on PATH that only prints the three values the guard asks for; a real `make flash` of the Nucleo satellite and of the
ground station is run against such a stub CUBUS board and must stop with the ERROR text before any flash tool is reached.
The build images are checked for the CUBUS id word, which is what the guard looks for in the flash of a board.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "flash"))
import board_guard as bg  # noqa: E402

fails = 0


def check(name, ok, extra=""):
    global fails
    print("%s  %s %s" % ("ok  " if ok else "FAIL", name, extra))
    if not ok:
        fails += 1


def info(cubus_words=0, uid="00112233445566778899AABB"):
    return {"devid": 0x497, "uid": uid, "cubus_words": cubus_words}


# ---- decision logic
cub, why = bg.classify(info(1), set())
check("flash holds CUBUS id -> CUBUS", cub and len(why) == 1)
cub, why = bg.classify(info(0, "AAAAAAAAAAAAAAAAAAAAAAAA"), {"AAAAAAAAAAAAAAAAAAAAAAAA"})
check("registered uid -> CUBUS", cub and "registered" in why[0])
cub, why = bg.classify(info(0), {"AAAAAAAAAAAAAAAAAAAAAAAA"})
check("other board -> not CUBUS", not cub and why == [])
for img in ("nucleo", "gs", "nuttx"):
    check("CUBUS board refuses %s flash" % img, not bg.decide(img, "flash", True))
check("CUBUS board refuses the NuttX CPU1 flash even for a CUBUS image", not bg.decide("cubus", "flash-m4", True))
check("CUBUS board accepts the CUBUS CPU2 image", bg.decide("cubus", "flash", True))
for img in ("nucleo", "gs", "cubus"):
    check("other board accepts %s" % img, bg.decide(img, "flash", False))

# ---- the script is read only
check("generated OpenOCD script has no write / reset / halt / flash command", bg.script_is_read_only(bg.TCL) == [])
check("the scanner does catch a write", bg.script_is_read_only("mww 0x20000000 1") != [] and bg.script_is_read_only("reset halt") != [])

# ---- registry file
with tempfile.TemporaryDirectory() as tmp:
    p = os.path.join(tmp, "reg.txt")
    open(p, "w").write("# comment\n\n0123456789abcdef01234567   # lower case ok\nnot an id\n")
    check("registry parse", bg.load_registry(p) == {"0123456789ABCDEF01234567"})
    check("missing registry file is empty", bg.load_registry(os.path.join(tmp, "none")) == set())

# ---- the images
BUILDS = {}
tmpb = tempfile.mkdtemp(prefix="guardbuild.")
try:
    for key, d, extra in (("cubus", "satellite", ["CUBUS=T"]), ("nucleo", "satellite", ["CUBUS=F"]), ("gs", "ground_station", [])):
        out = os.path.join(tmpb, key)
        r = subprocess.run(["make", "-C", os.path.join(ROOT, "cpu2", d), "-j4", "BUILD=" + out] + extra, capture_output=True, text=True)
        binf = os.path.join(out, d + ".bin")
        if r.returncode != 0 or not os.path.exists(binf):
            print(r.stdout[-2000:], r.stderr[-2000:])
            check("build %s" % key, False)
            continue
        data = open(binf, "rb").read()
        words = [w for (w,) in struct.iter_unpack("<I", data[:len(data) // 4 * 4])]
        BUILDS[key] = words.count(bg.CUBUS_ID)
    check("CUBUS image contains the CUBUS id word", BUILDS.get("cubus", 0) >= 1, "(%s)" % BUILDS.get("cubus"))
    check("Nucleo satellite image does not", BUILDS.get("nucleo", 1) == 0)
    check("ground-station image does not", BUILDS.get("gs", 1) == 0)

    # ---- flow with a stub openocd (prints what the guard script asks for; no hardware)
    def stub(path, uid, words):
        open(os.path.join(path, "openocd"), "w").write(
            "#!/bin/sh\necho GUARD_ID=20000497\necho GUARD_UID=%s\necho GUARD_CUBUS_WORDS=%d\necho GUARD_DONE=1\n" % (uid, words))
        os.chmod(os.path.join(path, "openocd"), 0o755)

    sb = os.path.join(tmpb, "stub")
    os.makedirs(sb)
    env = dict(os.environ, PATH=sb + os.pathsep + os.environ["PATH"])

    def guard(*args):
        return subprocess.run([sys.executable, os.path.join(ROOT, "tools", "flash", "board_guard.py")] + list(args), capture_output=True, text=True, env=env)

    stub(sb, "0123456789ABCDEF01234567", 1)
    r = guard("--image", "nucleo", "--what", "flash")
    check("flow: CUBUS board + Nucleo image -> exit 1", r.returncode == 1)
    check("flow: message says ERROR!! You made a mistake", "ERROR!!" in r.stdout and "You made a mistake, dont flash nucleo in CUBUS" in r.stdout)
    check("flow: CUBUS board + CUBUS image -> exit 0", guard("--image", "cubus").returncode == 0)
    stub(sb, "0123456789ABCDEF01234567", 0)
    check("flow: ordinary board + Nucleo image -> exit 0", guard("--image", "nucleo").returncode == 0)
    open(os.path.join(sb, "openocd"), "w").write("#!/bin/sh\necho 'Error: no device found' >&2\nexit 1\n")
    r = guard("--image", "nucleo")
    check("flow: unreadable board -> LOCKED (exit 1)", r.returncode == 1 and "LOCKED" in r.stdout)

    # ---- make flash really stops (stub CUBUS board; the flash tools are never reached)
    stub(sb, "0123456789ABCDEF01234567", 1)
    for key, d, extra in (("nucleo", "satellite", ["CUBUS=F"]), ("gs", "ground_station", [])):
        r = subprocess.run(["make", "-C", os.path.join(ROOT, "cpu2", d), "BUILD=" + os.path.join(tmpb, key), "flash"] + extra,
                           capture_output=True, text=True, env=env)
        check("make flash (%s image) on a CUBUS board stops with the error" % key,
              r.returncode != 0 and "dont flash nucleo in CUBUS" in r.stdout and "Flashing" not in r.stdout)
    r = subprocess.run(["make", "-C", os.path.join(ROOT, "cpu2", "satellite"), "flash-m4", "NUTTX_BIN=" + os.path.join(tmpb, "stub", "openocd")],
                       capture_output=True, text=True, env=env)
    check("make flash-m4 on a CUBUS board stops with the error", r.returncode != 0 and "dont flash nucleo in CUBUS" in r.stdout)
finally:
    shutil.rmtree(tmpb, ignore_errors=True)

print("board guard test: %s" % ("all ok" if fails == 0 else "%d FAILED" % fails))
sys.exit(1 if fails else 0)
