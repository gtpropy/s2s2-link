"""tb.trends -- history of the saved reports: verdict per test over time, numeric results, and a diff of two reports."""
import json
import os

from tb.util import REPORT_DIR

SKIP_KEYS = {"when", "t", "seconds_total"}


def _num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def metrics_of(res):
    """Numbers worth charting from one test result: its top-level numbers plus pass / fail / info counts of its rows."""
    m = {k: v for k, v in (res or {}).items() if _num(v) and k not in SKIP_KEYS}
    rows = (res or {}).get("rows")
    if isinstance(rows, list) and rows and isinstance(rows[0], dict) and "state" in rows[0]:
        for st in ("PASS", "FAIL", "INFO"):
            m["rows_" + st.lower()] = sum(1 for r in rows if r.get("state") == st)
    elif isinstance(rows, list) and rows and isinstance(rows[0], dict) and "ok" in rows[0]:
        m["rows_pass"] = sum(1 for r in rows if r.get("ok"))
        m["rows_fail"] = sum(1 for r in rows if not r.get("ok"))
    return m


def load_reports(report_dir=REPORT_DIR):
    out = []
    if not os.path.isdir(report_dir):
        return out
    for dn in sorted(os.listdir(report_dir)):
        f = os.path.join(report_dir, dn, "report.json")
        try:
            rep = json.load(open(f))
        except (OSError, ValueError):
            continue
        out.append({"dir": dn, "generated": rep.get("generated"), "note": rep.get("note", ""), "results": rep.get("results") or {}})
    return out


def build(reports):
    """{test: [{report, generated, verdict, metrics}]} oldest first."""
    t = {}
    for r in reports:
        for name, res in r["results"].items():
            if not res:
                continue
            t.setdefault(name, []).append({"report": r["dir"], "generated": r["generated"], "verdict": res.get("verdict"), "metrics": metrics_of(res)})
    return t


def diff(a, b):
    """Compare two reports (dicts from load_reports): per test the verdict change and the numbers that moved."""
    names = sorted(set(a["results"]) | set(b["results"]))
    out = []
    for n in names:
        ra, rb = a["results"].get(n), b["results"].get(n)
        va, vb = (ra or {}).get("verdict"), (rb or {}).get("verdict")
        ma, mb = metrics_of(ra), metrics_of(rb)
        moved = []
        for k in sorted(set(ma) | set(mb)):
            x, y = ma.get(k), mb.get(k)
            if x != y:
                moved.append({"name": k, "a": x, "b": y, "delta": (y - x) if (_num(x) and _num(y)) else None})
        if not ra and not rb:
            continue
        status = "same" if va == vb and not moved else ("regressed" if va in ("PASS", "INFO") and vb == "FAIL" else "fixed" if va == "FAIL" and vb in ("PASS", "INFO") else "changed")
        if va == vb and not moved:
            status = "same"
        out.append({"test": n, "a": va, "b": vb, "status": status, "moved": moved})
    return out
