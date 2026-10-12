"""tb.bus"""

import collections
import os
import queue
import threading
from tb.compat import events_mod, imagestore
from tb.util import ROOT, now

# --------------------------------------------------------------------------------------
# event bus (SSE) + shared state
# --------------------------------------------------------------------------------------
class Bus:
    def __init__(self):
        self.lock = threading.Lock()
        self.subs = []
        self.hist = collections.deque(maxlen=3000)

    def publish(self, kind, **kw):
        ev = {"t": round(now(), 3), "kind": kind}
        ev.update(kw)
        with self.lock:
            if kind not in ("chunk", "prof"):
                self.hist.append(ev)
            subs = list(self.subs)
        for q in subs:
            try:
                q.put_nowait(ev)
            except queue.Full:
                pass

    def subscribe(self):
        q = queue.Queue(maxsize=5000)
        with self.lock:
            for ev in list(self.hist)[-300:]:
                q.put_nowait(ev)
            self.subs.append(q)
        return q

    def unsubscribe(self, q):
        with self.lock:
            if q in self.subs:
                self.subs.remove(q)


BUS = Bus()

DATA = os.path.join(ROOT, "data", "testbench")
EVENTS = events_mod.EventLog(os.path.join(DATA, "events.db"))
IMAGES = imagestore.ImageStore(os.path.join(DATA, "images"), lambda kind, **kw: BUS.publish(kind, **kw))


def add_event(ev):
    """One structured event (ack, nack, cw, beacon, frame, command, ...): history, live state and the event stream."""
    ev = EVENTS.add(ev)
    BUS.publish("evt", ev=ev)
    return ev


def log(msg, level="info"):
    BUS.publish("log", level=level, msg=msg)
