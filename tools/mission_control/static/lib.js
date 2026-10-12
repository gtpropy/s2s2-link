/* shared: html tag, api, live store (SSE), small widgets */
(function () {
  const { h, utc, utcd, age, fx, Pill, Chip, Metric, Spark, Cube, attitude } = UI;
  const { useState, useEffect, useRef, useMemo } = React;
  const api = async (p, body) => {
    const r = await fetch(p, body === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    const j = await r.json().catch(() => ({}));
    if (r.status === 401 && p !== '/api/login' && p !== '/api/me') { window.dispatchEvent(new Event('s2s2-auth')); }
    if (!r.ok) throw new Error(j.error || r.statusText);
    return j;
  };

  /* ---- store */
  const S = { snap: null, hk: [], beacon: [], evt: [], jobs: {}, images: [], link: null, tick: 0 };
  const subs = new Set();
  const emit = () => { S.tick++; subs.forEach(f => f()); };
  const cap = (a, n) => (a.length > n ? a.slice(-n) : a);
  let es = null;
  function startLive() {
    if (es) es.close();
    es = new EventSource('/events');
    es.onmessage = m => { JSON.parse(m.data).forEach(handle); emit(); };      // events come in batches: one render per batch
  }
  function handle(e) {
    if (e.kind === 'evt') {
      const v = e.ev;
      S.evt = cap(S.evt.concat(v), 300);
      if (v.kind === 'hk') S.hk = cap(S.hk.concat(v), 2000);
      if (v.kind === 'beacon') S.beacon = cap(S.beacon.concat(v), 2000);
    } else if (e.kind === 'command') S.jobs = { ...S.jobs, [e.job.id]: e.job };
    else if (e.kind === 'image') { const s = { ...e }; delete s.kind; S.images = S.images.some(x => x.id === s.id) ? S.images.map(x => (x.id === s.id ? s : x)) : [s].concat(S.images); }
    else if (e.kind === 'link') S.link = e;
  }
  async function loadAll() {
    const [snap, hk, bc, ev, jobs] = await Promise.all([api('/api/snapshot'), api('/api/series?kind=hk&n=1000'), api('/api/series?kind=beacon&n=1000'), api('/api/events?limit=200'), api('/api/commands?n=50')]);
    S.snap = snap; S.hk = hk; S.beacon = bc; S.evt = ev; S.images = snap.images;
    S.jobs = Object.fromEntries(jobs.map(j => [j.id, { ...j, label: j.cmd, state: j.state }]));
    emit();
  }
  async function poll() { try { S.snap = await api('/api/snapshot'); emit(); } catch (e) { /* login screen handles 401 */ } }
  const useS = () => { const [, set] = useState(0); useEffect(() => { const f = () => set(S.tick); subs.add(f); return () => subs.delete(f); }, []); return S; };

  window.MC = { h, api, S, useS, startLive, loadAll, poll, emit, utc, utcd, age, fx, Pill, Chip, Metric, Spark, Cube, attitude, useState, useEffect, useRef, useMemo };
})();
