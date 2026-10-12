"""tb.profiling"""

import os
from tb.bus import BUS, log
from tb import profiler as prof_mod
from tb.compat import CORE
from tb.devices import DwCapture, SDR
from tb.links import OBC
from tb.state import RUN
from tb.util import DEFAULTS, GS_BUILD, GS_DIR, ROOT, SAT_BUILD, SAT_DIR, now

# --------------------------------------------------------------------------------------
# radio monitor: RTL-SDR + Direwolf running all the time, feeding the frame log and the image store
# --------------------------------------------------------------------------------------
def prof_targets():
    cfg = CORE.load_config()
    # CUBUS: CPU1 runs the flight OBC firmware, not this repository's NuttX bench image: no ELF to resolve its program counter against
    cpu1 = os.path.join(ROOT, "nuttx", "nuttx" if not cfg.get("cubus") else "no_cpu1_elf_on_cubus")
    return {
        "sat": {"serial": cfg.get("sat_serial") or DEFAULTS["sat_probe"], "cpu2_elf": os.path.join(SAT_BUILD, "satellite.elf"),
                "cpu2_ld": os.path.join(SAT_DIR, "boot", "wl55jc_cm0p.ld"), "cpu1_elf": cpu1},
        "gs": {"serial": cfg.get("gs_serial") or DEFAULTS["gs_probe"], "cpu2_elf": os.path.join(GS_BUILD, "ground_station.elf"),
               "cpu2_ld": os.path.join(GS_DIR, "boot", "wl55jc_cm0p.ld"), "cpu1_elf": os.path.join(ROOT, "nuttx", "nuttx")},
    }


PROF = prof_mod.Profiler(lambda kind, **kw: BUS.publish(kind, **kw), ROOT, {})


def prof_start(name):
    if name == "sat":
        from tb import cubus
        cubus.SWD.close()                              # the DEBUG page's OpenOCD holds the ST-Link
    PROF.targets = prof_targets()
    if name == "sat" and OBC.connected():
        pass                                           # the VCP and the SWD probe are separate USB interfaces: both can be used together
    PROF.start(name)
    log(f"profiler attached to the {name} board over SWD (no reset, cores keep running)", "ok")


MON = {"cap": None, "freq": 434.9936, "gain": 40, "since": None}


def monitor_start(freq=None, gain=None):
    if MON["cap"]:
        return
    if RUN["name"] in ("sdr", "sdrimage"):
        raise RuntimeError(f"test '{RUN['name']}' uses the SDR")
    ok_sdr, why = SDR.check_tuner()
    if not ok_sdr:
        raise RuntimeError("SDR not usable: " + " | ".join(why[:3]) + " (unplug and replug the dongle)")
    MON["freq"] = float(freq or MON["freq"])
    MON["gain"] = float(gain or MON["gain"])
    MON["cap"] = DwCapture(MON["freq"], MON["gain"])
    MON["since"] = now()
    log(f"radio monitor ON: RTL-SDR at {MON['freq']} MHz, Direwolf decoding 4800 bit/s G3RUH; frames and camera fragments are stored", "ok")
    BUS.publish("monitor", on=True, freq=MON["freq"], gain=MON["gain"])


def monitor_stop():
    cap, MON["cap"] = MON["cap"], None
    if cap:
        cap.close()
        log("radio monitor OFF", "ok")
        BUS.publish("monitor", on=False)
