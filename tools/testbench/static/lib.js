/* shared pieces: html tag, api helper, event store, small components. Everything is attached to window.S2. */
(function () {
  const h = UI.h;
  const { useState, useEffect, useRef, useCallback, useMemo } = React;

  const api = async (p, body) => {
    const r = await fetch(p, body === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    const j = await r.json().catch(() => ({}));
    if (!r.ok) throw new Error(j.error || r.statusText);
    return j;
  };

  /* ---------------------------------------------------------------- store: SSE + polled status */
  const CAP = 2000;
  const state = {
    status: null, live: null, bench: [], gs: [], sat: [], sdr: [], evt: [], images: [], e2e: [], jobs: {}, chunk: null,
    results: {}, running: null, monitor: null, tick: 0, prof: {}, profHist: {}, cmap: null, phase: null, progress: {}, alerts: [], shots: [], reports: [],
  };
  const subs = new Set();
  let queue = [], flushing = false;
  const emit = () => { state.tick++; subs.forEach(f => f()); };
  const push = (k, v) => { const a = state[k].concat(v); state[k] = a.length > CAP ? a.slice(-CAP) : a; };
  const fmtT = t => new Date(t * 1000).toLocaleTimeString([], { hour12: false });

  function apply(e) {
    const t = e.t || Date.now() / 1000;
    switch (e.kind) {
      case 'log': push('bench', { t, cls: e.level === 'info' ? '' : e.level, msg: e.msg }); break;
      case 'gs': push('gs', { t, cls: /ACK|Completed|Accepted/i.test(e.msg) ? 'ack' : /NACK|ERROR|FAIL/i.test(e.msg) ? 'nack' : '', msg: e.msg }); break;
      case 'sat': push('sat', { t, cls: '', msg: e.msg }); break;
      case 'sdr': push('sdr', { t, cls: 'rf', msg: e.msg }); break;
      case 'evt': push('evt', e.ev); break;
      case 'image': {
        const s = { ...e }; delete s.kind;
        const i = state.images.findIndex(x => x.id === s.id);
        state.images = i < 0 ? [s].concat(state.images) : state.images.map((x, j) => (j === i ? s : x));
        break; }
      case 'e2e': state.e2e = e.steps; break;
      case 'job': state.jobs = { ...state.jobs, [e.id]: e }; break;
      case 'chunk': {
        state.chunk = e;
        const RANK = { store: 1, read: 2, ack: 3, gs: 4 }, src = RANK[e.src] ? e.src : 'gs';
        const cm = state.cmap && state.cmap.total === e.total ? state.cmap : { total: e.total, st: new Array(e.total).fill(''), n: { store: 0, read: 0, ack: 0, gs: 0 }, seen: { store: {}, read: {}, ack: {}, gs: {} } };
        cm.seen[src][e.pkt] = 1; cm.n[src] = Object.keys(cm.seen[src]).length;
        const cur = RANK[cm.st[e.pkt - 1]] || 0;
        if (RANK[src] >= cur) cm.st[e.pkt - 1] = src;
        state.cmap = { ...cm, st: cm.st.slice() };
        break; }
      case 'phase': state.phase = e; break;
      case 'alert':
        if (e.alert) state.alerts = [e.alert].concat(state.alerts).slice(0, 200);
        else state.alerts = state.alerts.map(a => (e.ack == null || a.id === e.ack ? { ...a, acked: true } : a));
        break;
      case 'progress': state.progress = { ...state.progress, [e.test]: e }; break;
      case 'result': state.results = { ...state.results, [e.name]: e.result }; break;
      case 'test': state.running = e.state === 'running' ? e.name : null; if (e.state === 'running' && /image|imageloop/.test(e.name)) { state.cmap = null; state.phase = null; } break;
      case 'monitor': state.monitor = e; break;
      case 'prof': {
        state.prof = { ...state.prof, [e.target]: e };
        const hh = { ...state.profHist }; const t = { ...(hh[e.target] || {}) };
        e.cores.forEach(c => { t[c.core] = (t[c.core] || []).concat(c.busy_pct).slice(-120); });
        hh[e.target] = t; state.profHist = hh; break; }
    }
  }
  function flush() { flushing = false; const q = queue; queue = []; q.forEach(apply); emit(); }
  function connectSSE() {
    const es = new EventSource('/events');
    es.onmessage = m => { queue.push(...JSON.parse(m.data)); if (!flushing) { flushing = true; setTimeout(flush, 60); } };
  }
  async function refresh(fresh) {
    try {
      const s = await api('/api/status' + (fresh ? '?fresh=1' : ''));
      state.status = s; state.running = s.running; state.results = { ...state.results, ...s.results };
      emit();
    } catch (e) { push('bench', { t: Date.now() / 1000, cls: 'err', msg: 'status: ' + e.message }); emit(); }
    try { state.live = await api('/api/live'); state.images = state.live.images; state.monitor = state.live.monitor; emit(); } catch (e) { /* ignore */ }
  }
  const useStore = () => {
    const [, set] = useState(0);
    useEffect(() => { const f = () => set(state.tick); subs.add(f); return () => subs.delete(f); }, []);
    return state;
  };
  const log = (msg, cls) => { push('bench', { t: Date.now() / 1000, cls: cls || '', msg }); emit(); };
  const act = (p, body, ok) => api(p, body).then(r => { if (ok) log(ok, 'ok'); refresh(false); return r; }).catch(e => { log(p + ': ' + e.message, 'err'); throw e; });

  /* ---------------------------------------------------------------- components */
  const Chip = ({ cls, children, title }) => h`<span class="chip" title=${title || ''}><i class=${'dot ' + (cls || '')}></i>${children}</span>`;
  const Verdict = ({ v }) => (v ? h`<span class=${'verdict ' + v}>${v}</span>` : null);
  const Kv = ({ rows }) => h`<div class="kv">${rows.filter(Boolean).map(([k, v], i) => h`<${React.Fragment} key=${i}><b>${k}</b><span>${v}</span><//>`)}</div>`;

  const Terminal = React.memo(function Terminal({ lines, height, onClear, filterable, showTime = true }) {
    const ref = useRef(null);
    const [stick, setStick] = useState(true);
    const [f, setF] = useState('');
    const shown = useMemo(() => (f ? lines.filter(l => (l.msg || '').toLowerCase().includes(f.toLowerCase())) : lines), [lines, f]);
    useEffect(() => { const el = ref.current; if (el && stick) el.scrollTop = el.scrollHeight; });
    const copy = () => navigator.clipboard && navigator.clipboard.writeText(shown.map(l => fmtT(l.t) + ' ' + l.msg).join('\n'));
    return h`<div>
      <div class="row" style="margin-bottom:4px">
        ${filterable !== false && h`<input class="grow" placeholder="filter" value=${f} onInput=${e => setF(e.target.value)} />`}
        <label><input type="checkbox" checked=${stick} onChange=${e => setStick(e.target.checked)} /> follow</label>
        <button class="sm" onClick=${copy}>copy</button>
        ${onClear && h`<button class="sm" onClick=${onClear}>clear</button>`}
      </div>
      <div class="term" ref=${ref} style=${height ? 'height:' + height + 'px' : ''}>
        ${shown.slice(-600).map((l, i) => h`<div key=${i} class=${l.cls || ''}>${showTime && h`<span class="t">${fmtT(l.t)}</span>`}${l.msg}</div>`)}
      </div></div>`;
  }, (a, b) => a.lines === b.lines && a.height === b.height && a.showTime === b.showTime && a.filterable === b.filterable);   // onClear is a fresh closure on every render: ignore it

  const fmtBytes = n => (n == null ? '' : n.toLocaleString() + ' B');
  const useAsync = fn => {
    const [busy, setBusy] = useState(false);
    const run = useCallback(async (...a) => { setBusy(true); try { return await fn(...a); } finally { setBusy(false); } }, [fn]);
    return [run, busy];
  };

  window.S2 = { h, api, state, useStore, refresh, connectSSE, log, act, Chip, Verdict, Kv, Terminal, fmtT, UI, fmtBytes, useAsync, push, emit,
                React, useState, useEffect, useRef, useMemo, useCallback };
})();
