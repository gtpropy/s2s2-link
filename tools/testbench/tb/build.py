"""tb.build"""

import os
import re
import subprocess
from tb.bus import log
from tb.profiling import PROF
from tb.util import DEFAULTS, GS_BUILD, GS_DIR, ROOT, SAT_BUILD, SAT_DIR, TOOLCHAIN

# --------------------------------------------------------------------------------------
# build / flash / status / report
# --------------------------------------------------------------------------------------
def run_cmd(args, cwd, label):
    env = dict(os.environ, PATH=TOOLCHAIN + ":/opt/homebrew/bin:" + os.environ.get("PATH", ""))
    for v in ("CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH"):
        env.pop(v, None)
    log(f"$ {' '.join(args)}   ({label})")
    p = subprocess.Popen(args, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    tail = []
    for l in p.stdout:
        l = l.rstrip()
        if re.search(r"error|Error|BIN :|verified|Programming|undefined", l):
            log("  " + l[:200], "warn" if "rror" in l else "info")
        tail.append(l)
    p.wait()
    log(f"{label}: exit {p.returncode}", "ok" if p.returncode == 0 else "err")
    return p.returncode


def _cubus_arg(target, cubus):
    """make argument for the CUBUS profile (satellite only). None = use the saved option of .s2s2.config."""
    return ["CUBUS=" + ("TRUE" if cubus else "FALSE")] if (target == "sat" and cubus is not None) else []


def do_build(target, cubus=None):
    d = SAT_DIR if target == "sat" else GS_DIR
    run_cmd(["make", "-j4"] + _cubus_arg(target, cubus), d, f"build {target}" + (" (CUBUS)" if target == "sat" and cubus else ""))
    status_build(target)


def do_flash(target, cfg):
    d = SAT_DIR if target == "sat" else GS_DIR
    probe = cfg.get("sat_probe" if target == "sat" else "gs_probe") or DEFAULTS["sat_probe" if target == "sat" else "gs_probe"]
    PROF.stop(target)                                  # the profiler's OpenOCD holds the ST-Link
    if target == "sat":
        from tb import cubus as _cubus
        _cubus.SWD.close()                             # so does the DEBUG page's
    cb = cfg.get("cubus")
    run_cmd(["make", "flash", f"SERIAL={probe}"] + _cubus_arg(target, cb), d, f"flash {target}" + (" (CUBUS)" if target == "sat" and cb else ""))


def status_build(target):
    d, name = (SAT_BUILD, "satellite") if target == "sat" else (GS_BUILD, "ground_station")
    b = os.path.join(d, name + ".bin")
    if os.path.exists(b):
        return {"bin": os.path.getsize(b), "mtime": os.path.getmtime(b), "limit": 57344}
    return None


def git_info():
    try:
        h = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
        return h
    except Exception:
        return "?"
