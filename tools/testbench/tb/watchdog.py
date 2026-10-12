"""
tb.watchdog -- background monitors and the alert list. Off until you switch it on (it would otherwise touch the boards on its own).

  * test failures    a finished test with verdict FAIL raises an alert (always on)
  * GS silence       the ground-station UART is connected but sent no line for N seconds
  * VCP heartbeat    STATUS every N seconds while no test runs; three misses in a row (longer than a CW beacon) raise an alert
Alerts stay until acknowledged. With "notify" a macOS notification is shown as well.
"""
import json
import os
import subprocess
import threading
import time

from tb.bus import BUS, DATA, log
from tb.links import GS, OBC
from tb.state import RUN
from tb.util import OBC_STATUS, now

PATH = os.path.join(DATA, "settings.json")
DEFAULTS = {"enabled": False, "gs_silence_s": 180, "vcp_poll": True, "vcp_every_s": 20, "notify": False, "alert_on_fail": True}


class Watchdog:
    def __init__(self):
        self.lock = threading.Lock()
        self.alerts = []
        self._id = 0
        self.settings = dict(DEFAULTS)
        try:
            self.settings.update({k: v for k, v in json.load(open(PATH)).get("watch", {}).items() if k in DEFAULTS})
        except (OSError, ValueError):
            pass
        self.vcp_miss = 0
        self.t_vcp = 0.0
        threading.Thread(target=self._loop, daemon=True).start()
        threading.Thread(target=self._results, daemon=True).start()

    def set(self, changes):
        s = dict(self.settings)
        for k, v in changes.items():
            if k not in DEFAULTS:
                raise ValueError(f"unknown setting {k}")
            s[k] = type(DEFAULTS[k])(v) if not isinstance(DEFAULTS[k], bool) else bool(v)
        s["gs_silence_s"] = max(30, min(86400, s["gs_silence_s"]))
        s["vcp_every_s"] = max(5, min(3600, s["vcp_every_s"]))
        self.settings = s
        os.makedirs(os.path.dirname(PATH), exist_ok=True)
        try:
            cur = json.load(open(PATH))
        except (OSError, ValueError):
            cur = {}
        cur["watch"] = s
        json.dump(cur, open(PATH, "w"), indent=1)
        return s

    # ---- alerts
    def raise_alert(self, key, level, text):
        with self.lock:
            if any(a["key"] == key and not a["acked"] for a in self.alerts):
                return None
            self._id += 1
            a = {"id": self._id, "t": now(), "key": key, "level": level, "text": text, "acked": False}
            self.alerts = (self.alerts + [a])[-200:]
        BUS.publish("alert", alert=a)
        log(f"ALERT: {text}", "err" if level == "bad" else "warn")
        if self.settings["notify"]:
            try:
                subprocess.Popen(["osascript", "-e", f'display notification "{text[:120]}" with title "S2S2 testbench"'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except OSError:
                pass
        return a

    def ack(self, aid=None):
        with self.lock:
            for a in self.alerts:
                if aid is None or a["id"] == aid:
                    a["acked"] = True
        BUS.publish("alert", ack=aid)

    def listing(self):
        with self.lock:
            return list(reversed(self.alerts))

    # ---- monitors
    def _results(self):
        q = BUS.subscribe()
        while True:
            e = q.get()
            if e.get("kind") == "result" and self.settings["alert_on_fail"] and (e.get("result") or {}).get("verdict") == "FAIL":
                self.raise_alert("test:" + e["name"], "bad", f"test {e['name']} FAILED")

    def _loop(self):
        while True:
            time.sleep(2)
            s = self.settings
            if not s["enabled"]:
                continue
            try:
                self._check_gs(s)
                self._check_vcp(s)
            except Exception as e:                       # a monitor must never die
                log(f"watchdog: {e}", "warn")

    def _check_gs(self, s):
        if GS.connected():
            with GS.lock:
                last = GS.lines[-1][0] if GS.lines else None
            if last is not None and now() - last > s["gs_silence_s"]:
                self.raise_alert("gs_silence", "warn", f"ground station UART silent for {now() - last:.0f} s")

    def _check_vcp(self, s):
        if not (s["vcp_poll"] and OBC.connected()) or RUN["name"] or now() - self.t_vcp < s["vcp_every_s"]:
            return
        self.t_vcp = now()
        seq = int(now()) & 0xFFFF
        mk = OBC.mark()
        OBC.send(OBC_STATUS, seq)
        st = OBC.wait_ack(OBC_STATUS, seq, mk, 2.0)
        if st == 0:
            self.vcp_miss = 0
        else:
            self.vcp_miss += 1
            if self.vcp_miss >= 3:
                self.raise_alert("vcp", "bad", "Nucleo VCP: STATUS not answered (CPU1 or CPU2 stopped?)" if st is None else f"Nucleo VCP: STATUS answered with 0x{st:02X}")


WATCH = Watchdog()
