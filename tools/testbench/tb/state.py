"""tb.state"""

import threading
from tb.bus import BUS

# --------------------------------------------------------------------------------------
# tests
# --------------------------------------------------------------------------------------
RUN = {"lock": threading.Lock(), "thread": None, "abort": threading.Event(), "name": None}
STATE = {"link": None, "image": None, "sdr": None, "rate": None, "cwtest": None, "sdrimage": None, "imageloop": None, "digi": None, "e2e": None, "image_rx": None, "gs_log": []}
PROG = {"total": 0, "acked": set(), "got": set(), "stored": set(), "read": set(), "phase": "", "mode": "", "led": None}


def set_result(name, res):
    """Remember the last result of a test for the page and for the report."""
    STATE[name] = res
    BUS.publish("result", name=name, result={k: v for k, v in res.items() if k not in ("rx_bytes", "raw_frames")})


ACK_RX = r"ACK ACCEPTED|ACK Accepted"
CAM_START_RX = r'Starting camera download|"type":"CAMERA"'
