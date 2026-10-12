"""tb.logstore -- every GS / VCP / SDR / testbench log line in SQLite, searchable and exportable (survives restarts, unlike the 2000-line screen buffers)."""
import csv
import io
import os
import re
import sqlite3
import threading
import time

from tb.bus import BUS, DATA

SOURCES = {"gs": "GS", "sat": "VCP", "sdr": "SDR", "log": "BENCH"}
KEEP_S = 14 * 86400


class LogStore:
    def __init__(self, path, bus):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        self.db = sqlite3.connect(path, check_same_thread=False)
        self.db.execute("CREATE TABLE IF NOT EXISTS logs (t REAL, src TEXT, level TEXT, msg TEXT)")
        self.db.execute("CREATE INDEX IF NOT EXISTS logs_t ON logs (t)")
        self.lock = threading.Lock()
        self.bus = bus
        self.q = bus.subscribe()
        threading.Thread(target=self._writer, daemon=True).start()

    def _writer(self):
        last_prune = 0.0
        while True:
            rows = []
            try:
                ev = self.q.get(timeout=1.0)
                batch = [ev]
                while len(batch) < 2000:
                    try:
                        batch.append(self.q.get_nowait())
                    except Exception:
                        break
                for e in batch:
                    src = SOURCES.get(e.get("kind"))
                    if src:
                        rows.append((e["t"], src, e.get("level", "info") if src == "BENCH" else "", str(e.get("msg", ""))[:2000]))
            except Exception:
                pass
            if rows:
                with self.lock:
                    self.db.executemany("INSERT INTO logs VALUES (?,?,?,?)", rows)
                    self.db.commit()
            if time.time() - last_prune > 3600:
                last_prune = time.time()
                with self.lock:
                    self.db.execute("DELETE FROM logs WHERE t < ?", (time.time() - KEEP_S,))
                    self.db.commit()

    def _where(self, q, srcs, t0, t1, regex):
        w, a = [], []
        if srcs:
            w.append("src IN (%s)" % ",".join("?" * len(srcs)))
            a += list(srcs)
        if t0:
            w.append("t >= ?")
            a.append(t0)
        if t1:
            w.append("t <= ?")
            a.append(t1)
        if q and not regex:
            w.append("msg LIKE ? ESCAPE '\\'")
            a.append("%" + q.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_") + "%")
        return (" WHERE " + " AND ".join(w)) if w else "", a

    def search(self, q="", srcs=None, t0=None, t1=None, limit=500, offset=0, regex=False):
        where, a = self._where(q, srcs, t0, t1, regex)
        rx = re.compile(q, re.I) if (regex and q) else None
        with self.lock:
            if rx:                                              # regex: filter in Python over the time window (bounded scan)
                rows = [r for r in self.db.execute("SELECT t,src,level,msg FROM logs" + where + " ORDER BY t DESC LIMIT 200000", a) if rx.search(r[3])]
                total = len(rows)
                rows = rows[offset:offset + limit]
            else:
                total = self.db.execute("SELECT COUNT(*) FROM logs" + where, a).fetchone()[0]
                rows = self.db.execute("SELECT t,src,level,msg FROM logs" + where + " ORDER BY t DESC LIMIT ? OFFSET ?", a + [limit, offset]).fetchall()
        return {"total": total, "rows": [{"t": r[0], "src": r[1], "level": r[2], "msg": r[3]} for r in rows]}

    def csv(self, **kw):
        kw["limit"] = 500000
        kw["offset"] = 0
        out = io.StringIO()
        w = csv.writer(out)
        w.writerow(["time_utc", "t_epoch", "source", "level", "message"])
        for r in reversed(self.search(**kw)["rows"]):
            w.writerow([time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(r["t"])) + ".%03d" % int((r["t"] % 1) * 1000), "%.3f" % r["t"], r["src"], r["level"], r["msg"]])
        return out.getvalue()

    def stats(self):
        with self.lock:
            n, t0 = self.db.execute("SELECT COUNT(*), MIN(t) FROM logs").fetchone()
        return {"lines": n, "since": t0}


LOGS = LogStore(os.path.join(DATA, "logs.db"), BUS)
