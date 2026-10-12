"""tb.status"""

import glob
import math
from tb.build import git_info, status_build
from tb.bus import EVENTS, IMAGES
from tb.commands import JOBS
from tb.devices import SDR
from tb.imaging import IMG, kind_of
from tb.links import GS, OBC
from tb.compat import cubus_on
from tb.profiling import MON
from tb.registry import TESTS
from tb.state import PROG, RUN, STATE
from tb.usb import probes
from tb.util import CHUNK, FLASH_CHUNKS, now, sha256

def live_state():
    """Everything the dashboard shows at a glance, in one call."""
    latest = {k: v for k, v in EVENTS.latest.items()}
    return {"t": now(), "latest": latest, "monitor": {"on": bool(MON["cap"]), "freq": MON["freq"], "gain": MON["gain"], "since": MON["since"]},
            "images": IMAGES.list()[:12], "running": RUN["name"], "jobs": list(JOBS.values())[-12:],
            "counts": {k: sum(1 for e in EVENTS.recent(limit=2000) if e["kind"] == k) for k in ("ack", "nack", "beacon", "hk", "frame", "command")}}


_sdr_cache = {"t": 0, "v": None}


def get_status(fresh=False):
    ports = sorted(glob.glob("/dev/cu.usbserial-*"))
    if fresh and not RUN["name"]:      # rtl_test takes seconds: only on an explicit "Detect"
        _sdr_cache["v"] = SDR.present()
        _sdr_cache["t"] = now()
    sdr = _sdr_cache["v"] or (False, [])
    return {"gs": {"connected": GS.connected(), "port": GS.port, "ports": ports},
            "probes": probes() if fresh else [],
            "sdr": {"present": sdr[0], "info": sdr[1]},
            "sat_build": status_build("sat"), "gs_build": status_build("gs"),
            "git": git_info(), "running": RUN["name"],
            "cubus": cubus_on(),
            "obc": {"connected": OBC.up(), "protocol": OBC.connected(), "text_only": OBC.text_only, "port": OBC.port,
                    "ports": OBC.uart_ports() if cubus_on() else OBC.ports()},
            "progress": {"total": PROG["total"], "acked": sorted(PROG["acked"]), "got": sorted(PROG["got"]),
                         "stored": sorted(PROG["stored"]), "read": sorted(PROG["read"]), "phase": PROG["phase"], "mode": PROG["mode"]},
            "image": {"name": IMG["name"], "bytes": len(IMG["bytes"]) if IMG["bytes"] else 0,
                      "chunks": math.ceil(len(IMG["bytes"]) / CHUNK) if IMG["bytes"] else 0,
                      "info": IMG["info"], "sha256": sha256(IMG["bytes"]) if IMG["bytes"] else None,
                      "fit": IMG["fit"], "type": kind_of(IMG["bytes"])[0] if IMG["bytes"] else None,
                      "fits_flash": (math.ceil(len(IMG["bytes"]) / CHUNK) <= FLASH_CHUNKS) if IMG["bytes"] else None},
            "results": {k: ({kk: vv for kk, vv in v.items() if kk != "raw_frames"} if v else None) for k, v in STATE.items()
                        if k in TESTS}}
