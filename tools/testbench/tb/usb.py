"""tb.usb"""

import re
import subprocess

def _usb_devices():
    """(product name, serial number) of every USB device, read from macOS's USB registry. This does NOT talk to the
    devices. (It used to run `st-info --probe` and `rtl_test -t`: both open the device, and probing an ST-Link resets the
    board it is attached to -- a status refresh was resetting the Nucleo in the middle of tests.)"""
    out = []
    try:
        txt = subprocess.run(["ioreg", "-p", "IOUSB", "-l", "-w0"], capture_output=True, text=True, timeout=15).stdout
        name = None
        for ln in txt.splitlines():
            m = re.search(r'"USB Product Name" = "([^"]*)"', ln)
            if m:
                name = m.group(1)
                out.append([name, ""])
                continue
            m = re.search(r'"USB Serial Number" = "([^"]*)"', ln)
            if m and out:
                out[-1][1] = m.group(1)
    except Exception:
        pass
    return [tuple(x) for x in out]


def probes():
    return [sn for name, sn in _usb_devices() if "stlink" in name.lower().replace("-", "").replace("_", "").replace(" ", "") and sn]
