/* Time-series charts for telemetry: used by the Satellite control page and by Telemetry > Trends.
 * Telemetry arrives in bursts (during passes), so a chart must not join points across hours of silence and must not draw thousands of
 * raw points: dense data is averaged into buckets (with the min / max envelope), and a line is broken wherever there is a gap. */
(function () {
  const { h, useState, useRef, useEffect, useMemo, fx, clock } = MC;
  const { getPath } = MCP;

  function useSize() {
    const ref = useRef(null);
    const [sz, set] = useState({ w: 0, h: 0 });
    useEffect(() => {
      if (!ref.current || !window.ResizeObserver) return undefined;
      const ro = new ResizeObserver(e => { const r = e[0].contentRect; set({ w: Math.round(r.width), h: Math.round(r.height) }); });
      ro.observe(ref.current);
      return () => ro.disconnect();
    }, []);
    return [ref, sz];
  }

  /* [key, label, unit, decimals, colour, limit key] */
  const DEFS = {
    bat_v: ['bat_v', 'Battery voltage', 'V', 2, 'var(--c1)', 'bat_v'], bus_v: ['bus_v', 'Bus voltage', 'V', 2, 'var(--c2)', 'bus_v'], bat_i: ['bat_i', 'Battery current', 'A', 2, 'var(--c3)', null],
    sol_v: ['sol_v', 'Solar voltage', 'V', 2, 'var(--c4)', null], bat_t: ['bat_t', 'Battery temperature', '°C', 1, 'var(--c5)', 'bat_t'], rssi: ['rssi', 'Received signal (RSSI)', 'dBm', 0, 'var(--c6)', null],
    ant_t: ['ant_t', 'Antenna temperature', '°C', 1, 'var(--c3)', 'ant_t'], bpb_t: ['bpb_t', 'Backplane temperature', '°C', 1, 'var(--c4)', 'bpb_t'],
    el: ['el', 'Elevation at the ground station', '°', 1, 'var(--c1)', null], alt: ['alt', 'Altitude', 'km', 1, 'var(--c2)', null], speed: ['speed', 'Speed', 'km/s', 3, 'var(--c3)', null],
    lat: ['lat', 'Latitude of the sub-satellite point', '°', 1, 'var(--c4)', null],
  };

  /* housekeeping packets plus, for the battery values, the CW beacons; RSSI from the receive events. src = { hk, beacon, evt } */
  function series(src, key, since) {
    const out = [];
    if (key === 'rssi') {
      for (const e of src.evt || []) if (e.kind === 'rssi' && typeof e.rssi === 'number') out.push({ t: e.t, v: e.rssi });
      for (const x of src.hk) if (typeof x.rssi === 'number') out.push({ t: x.t, v: x.rssi });
    }
    else {
      for (const x of src.hk) { const v = getPath(x, key); if (typeof v === 'number') out.push({ t: x.t, v }); }
      for (const b of src.beacon) {
        const k = MC.beaconKind(b);
        if (k === 1 && (key === 'bat_v' || key === 'bat_i') && typeof b.b1[key] === 'number') out.push({ t: b.t, v: b.b1[key] });
        if (k === 2 && key === 'bat_t' && b.b2 && b.b2.bat_temp_c !== 0 && Math.abs(b.b2.bat_temp_c) < 80) out.push({ t: b.t, v: b.b2.bat_temp_c });
      }
    }
    out.sort((a, b) => a.t - b.t);
    return since ? out.filter(p => p.t >= since) : out;
  }

  /* dense data -> buckets (mean with envelope); runs = lists of consecutive points without a gap */
  function prepare(pts, t0, t1, px) {
    const n = Math.max(24, Math.floor(px / 3));
    let out = pts, bs = 0;
    if (pts.length > n * 1.5) {
      bs = (t1 - t0) / n;
      const bk = new Map();
      for (const p of pts) {
        const i = Math.max(0, Math.min(n - 1, Math.floor((p.t - t0) / bs)));
        let b = bk.get(i);
        if (!b) bk.set(i, b = { s: 0, c: 0, lo: Infinity, hi: -Infinity, ts: 0 });
        b.s += p.v; b.c++; b.ts += p.t; if (p.v < b.lo) b.lo = p.v; if (p.v > b.hi) b.hi = p.v;
      }
      out = [...bk.entries()].sort((a, b) => a[0] - b[0]).map(([, b]) => ({ t: b.ts / b.c, v: b.s / b.c, lo: b.lo, hi: b.hi }));
    }
    const dts = [];
    for (let i = 1; i < pts.length; i++) dts.push(pts[i].t - pts[i - 1].t);
    dts.sort((a, b) => a - b);
    const med = dts.length ? dts[Math.floor(dts.length / 2)] : 60;
    const gap = Math.max(600, 6 * med, 2.5 * bs);
    const runs = [];
    let cur = [];
    out.forEach((p, i) => { if (i && p.t - out[i - 1].t > gap) { runs.push(cur); cur = []; } cur.push(p); });
    if (cur.length) runs.push(cur);
    return { runs, all: out, gap };
  }

  const fmtT = (t, span) => { const d = new Date(t * 1000); return span > 129600 ? d.toLocaleDateString([], { month: '2-digit', day: '2-digit' }) + ' ' + d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', hour12: false }) : d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', hour12: false }); };

  /* round axis values: ticks at 1, 2 or 5 times a power of ten */
  function niceStep(range, n) {
    const raw = range / Math.max(1, n), p = Math.pow(10, Math.floor(Math.log10(raw))), f = raw / p;
    return (f < 1.5 ? 1 : f < 3.5 ? 2 : f < 7.5 ? 5 : 10) * p;
  }
  const TSTEPS = [60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200, 86400, 172800];
  function timeTicks(t0, t1, maxN) {                       // ticks on round local times
    const step = TSTEPS.find(x => (t1 - t0) / x <= maxN) || TSTEPS[TSTEPS.length - 1], off = -new Date(t0 * 1000).getTimezoneOffset() * 60, out = [];
    for (let t = Math.ceil((t0 + off) / step) * step - off; t <= t1; t += step) out.push(t);
    return out;
  }
  /* monotone cubic curve through the points (no overshoot between samples) */
  function curve(P) {
    if (P.length < 3) return P.map((p, j) => (j ? 'L' : 'M') + p[0].toFixed(1) + ' ' + p[1].toFixed(1)).join('');
    const n = P.length, dx = [], m = [], tg = new Array(n);
    for (let i = 0; i < n - 1; i++) { dx.push(P[i + 1][0] - P[i][0]); m.push((P[i + 1][1] - P[i][1]) / (dx[i] || 1)); }
    tg[0] = m[0]; tg[n - 1] = m[n - 2];
    for (let i = 1; i < n - 1; i++) tg[i] = m[i - 1] * m[i] <= 0 ? 0 : (m[i - 1] + m[i]) / 2;
    for (let i = 0; i < n - 1; i++) {
      if (m[i] === 0) { tg[i] = 0; tg[i + 1] = 0; continue; }
      const a = tg[i] / m[i], b = tg[i + 1] / m[i], r = a * a + b * b;
      if (r > 9) { const k = 3 / Math.sqrt(r); tg[i] = k * a * m[i]; tg[i + 1] = k * b * m[i]; }
    }
    let d = 'M' + P[0][0].toFixed(1) + ' ' + P[0][1].toFixed(1);
    for (let i = 0; i < n - 1; i++) d += 'C' + (P[i][0] + dx[i] / 3).toFixed(1) + ' ' + (P[i][1] + tg[i] * dx[i] / 3).toFixed(1) + ' ' + (P[i + 1][0] - dx[i] / 3).toFixed(1) + ' ' + (P[i + 1][1] - tg[i + 1] * dx[i] / 3).toFixed(1) + ' ' + P[i + 1][0].toFixed(1) + ' ' + P[i + 1][1].toFixed(1);
    return d;
  }
  /* value at time t from the raw samples: interpolated between two neighbours that are not separated by a gap, else null */
  function valueAt(pts, t, gap) {
    if (!pts.length) return null;
    let lo = 0, hi = pts.length - 1;
    if (t <= pts[0].t) return pts[0].t - t <= gap / 2 ? { v: pts[0].v, t: pts[0].t } : null;
    if (t >= pts[hi].t) return t - pts[hi].t <= gap / 2 ? { v: pts[hi].v, t: pts[hi].t } : null;
    while (hi - lo > 1) { const mid = (lo + hi) >> 1; if (pts[mid].t <= t) lo = mid; else hi = mid; }
    const a = pts[lo], b = pts[hi];
    if (b.t - a.t > gap) return t - a.t <= gap / 4 ? { v: a.v, t: a.t } : b.t - t <= gap / 4 ? { v: b.v, t: b.t } : null;
    return { v: a.v + (b.v - a.v) * (t - a.t) / (b.t - a.t || 1), t };
  }
  /* pointer events can arrive faster than the screen refreshes: pass on only the newest value once per frame (this keeps the cursor smooth) */
  function useThrottled(set) {
    const r = useRef({ v: null, raf: 0 });
    useEffect(() => () => cancelAnimationFrame(r.current.raf), []);
    return v => { r.current.v = v; if (!r.current.raf) r.current.raf = requestAnimationFrame(() => { r.current.raf = 0; set(r.current.v); }); };
  }

  /* props: def, pts, lim, win:[t0,t1] (x range, default = data span), t (time pointed at, shared by all charts), pinned, onHover(t|null), onPin(), bands:[[a,b],...] shaded time ranges */
  function Chart({ def, pts, lim, win, t, pinned, onHover, onPin, bands, passes }) {
    const [ref, sz] = useSize();
    const [key, label, unit, d0, color] = def;
    const W = sz.w, H = sz.h, m = W < 240 ? { l: 32, r: 8, t: 6, b: 15 } : { l: 42, r: 12, t: 8, b: 18 }, iw = W - m.l - m.r, ih = H - m.t - m.b;
    let t0 = win ? win[0] : pts.length ? pts[0].t : 0, t1 = win ? win[1] : pts.length ? pts[pts.length - 1].t : 1;
    if (t1 - t0 < 120) t0 = t1 - 120;
    const prep = useMemo(() => {
      const vis = pts.filter(p => p.t >= t0 && p.t <= t1);
      return vis.length ? { ...prepare(vis, t0, t1, Math.max(60, iw)), vis } : { runs: [], all: [], vis, gap: 600 };
    }, [pts, Math.round(t0), Math.round(t1), iw]);
    const at = t != null ? valueAt(prep.vis, t, prep.gap) : null;
    const last = prep.vis[prep.vis.length - 1];
    const shown = t != null ? at : last ? { v: last.v, t: last.t } : null, pointed = t != null;
    let body = null;
    if (W > 60 && H > 24) {
      if (!prep.all.length) body = h`<text x=${W / 2} y=${H / 2} text-anchor="middle" class="ax">no data in this time range</text>`;
      else {
        let lo = Infinity, hi = -Infinity;
        for (const p of prep.all) { lo = Math.min(lo, p.lo != null ? p.lo : p.v); hi = Math.max(hi, p.hi != null ? p.hi : p.v); }
        const span0 = Math.max(hi - lo, Math.abs(hi) * 0.02, 0.1);
        const near = v => v != null && v >= lo - 1.5 * span0 && v <= hi + 1.5 * span0;
        const L = lim || {}, lLo = near(L.lo) ? L.lo : null, lHi = near(L.hi) ? L.hi : null;
        if (lLo != null) lo = Math.min(lo, lLo); if (lHi != null) hi = Math.max(hi, lHi);
        if (hi - lo < span0) { const c = (hi + lo) / 2; lo = c - span0 / 2; hi = c + span0 / 2; }
        const step = niceStep(hi - lo, Math.max(2, Math.floor(ih / 38)));
        const ya = Math.floor(lo / step) * step, yb = Math.ceil(hi / step) * step;
        const ticks = []; for (let v = ya; v <= yb + step / 2; v += step) ticks.push(v);
        const dd = Math.min(d0 + 1, Math.max(0, -Math.floor(Math.log10(step) + 1e-9)));
        const X = tt => m.l + iw * (tt - t0) / (t1 - t0), Y = v => m.t + ih * (1 - (v - ya) / (yb - ya || 1));
        const span = t1 - t0, xt = timeTicks(t0, t1, Math.max(2, Math.floor(iw / 62)));
        const move = e => {
          const r = e.currentTarget.getBoundingClientRect();
          onHover && onHover(Math.max(t0, Math.min(t1, t0 + (e.clientX - r.left - m.l) / iw * span)));
        };
        const sx = pointed ? X(t) : at ? X(at.t) : last ? X(last.t) : null, sy = shown ? Y(shown.v) : null;
        const tipTxt = shown && pointed ? [new Date(t * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' }), fx(shown.v, d0) + ' ' + unit] : null;
        const tw = tipTxt ? Math.max(...tipTxt.map(x => x.length)) * 6.4 + 12 : 0, tx = Math.max(m.l + 2, Math.min(W - m.r - tw - 2, (sx || 0) - tw / 2));       // centred on the cursor, kept inside the plot: it never jumps from side to side
        body = h`<defs><linearGradient id=${'g_' + key} x1="0" y1="0" x2="0" y2="1"><stop offset="0" style=${'stop-color:' + color + ';stop-opacity:.28'}></stop><stop offset="1" style=${'stop-color:' + color + ';stop-opacity:0'}></stop></linearGradient><clipPath id=${'c_' + key}><rect x=${m.l} y=${m.t} width=${iw} height=${ih}></rect></clipPath></defs>
          ${(bands || []).map(([a, b], i) => { const x0 = Math.max(m.l, X(a)), x1 = Math.min(W - m.r, X(b)); return x1 > x0 ? h`<rect key=${'b' + i} x=${x0} y=${m.t} width=${x1 - x0} height=${ih} class="band"></rect>` : null; })}
          ${(passes || []).map(([a, b], i) => { const x0 = Math.max(m.l, X(a)), x1 = Math.min(W - m.r, X(b)); return x1 > x0 ? h`<rect key=${'p' + i} x=${x0} y=${m.t} width=${x1 - x0} height=${ih} class="band pass"></rect>` : null; })}
          ${xt.map((tt, i) => h`<g key=${'x' + i}><line x1=${X(tt)} x2=${X(tt)} y1=${m.t} y2=${m.t + ih} class="gl v"></line><text x=${X(tt)} y=${H - 5} text-anchor="middle" class="ax">${fmtT(tt, span)}</text></g>`)}
          ${ticks.map((v, i) => h`<g key=${i}><line x1=${m.l} x2=${W - m.r} y1=${Y(v)} y2=${Y(v)} class="gl"></line><text x=${m.l - 6} y=${Y(v) + 3.5} text-anchor="end" class="ax">${fx(v, dd)}</text></g>`)}
          ${lLo != null && h`<g><rect x=${m.l} y=${Y(lLo)} width=${iw} height=${Math.max(0, m.t + ih - Y(lLo))} class="lim"></rect><line x1=${m.l} x2=${W - m.r} y1=${Y(lLo)} y2=${Y(lLo)} class="ll"></line></g>`}
          ${lHi != null && h`<g><rect x=${m.l} y=${m.t} width=${iw} height=${Math.max(0, Y(lHi) - m.t)} class="lim"></rect><line x1=${m.l} x2=${W - m.r} y1=${Y(lHi)} y2=${Y(lHi)} class="ll"></line></g>`}
          <g clip-path=${'url(#c_' + key + ')'}>${prep.runs.map((r, i) => {
            if (r.length === 1) return h`<circle key=${'r' + i} cx=${X(r[0].t)} cy=${Y(r[0].v)} r="2.4" style=${'fill:' + color}></circle>`;
            const P = r.map(p => [X(p.t), Y(p.v)]), line = curve(P);
            const area = line + 'L' + P[P.length - 1][0].toFixed(1) + ' ' + (m.t + ih) + 'L' + P[0][0].toFixed(1) + ' ' + (m.t + ih) + 'Z';
            const env = r[0].lo != null && r.some(p => p.hi - p.lo > 1e-9) ? r.map((p, j) => (j ? 'L' : 'M') + X(p.t).toFixed(1) + ' ' + Y(p.hi).toFixed(1)).join('') + r.slice().reverse().map(p => 'L' + X(p.t).toFixed(1) + ' ' + Y(p.lo).toFixed(1)).join('') + 'Z' : null;
            return h`<g key=${'r' + i}>${env && h`<path d=${env} style=${'fill:' + color + ';opacity:.14'}></path>`}<path d=${area} fill=${'url(#g_' + key + ')'}></path><path d=${line} fill="none" style=${'stroke:' + color} stroke-width="1.8" stroke-linejoin="round" stroke-linecap="round"></path>
              ${r.length <= 40 && r.map((p, j) => h`<circle key=${j} cx=${X(p.t)} cy=${Y(p.v)} r="1.8" style=${'fill:var(--panel2);stroke:' + color} stroke-width="1.2"></circle>`)}</g>`;
          })}</g>
          ${pointed && sx != null && h`<line x1=${sx} x2=${sx} y1=${m.t} y2=${m.t + ih} class=${'cross ' + (pinned ? 'pinned' : '')}></line>`}
          ${!pointed && last && t1 - last.t < 600 && h`<circle cx=${X(last.t)} cy=${Y(last.v)} r="3.5" class="pulse" style=${'fill:' + color}></circle>`}
          ${sx != null && sy != null && h`<circle cx=${sx} cy=${sy} r=${pointed ? 4.5 : 3.5} style=${'fill:' + color + ';stroke:var(--panel2)'} stroke-width="1.5"></circle>`}
          ${tipTxt && sx != null && h`<g class="tip" transform=${'translate(' + tx.toFixed(1) + ',' + (m.t + 4) + ')'}><rect width=${tw} height="34" rx="6"></rect><text x="6" y="14" class="t1">${tipTxt[0]}</text><text x="6" y="28" class="t2" style=${'fill:' + color}>${tipTxt[1]}</text></g>`}
          ${pointed && !shown && sx != null && h`<text x=${Math.min(W - m.r - 4, sx + 6)} y=${m.t + 12} text-anchor=${sx > W / 2 ? 'end' : 'start'} class="ax">no data here</text>`}
          <rect x=${m.l} y=${m.t} width=${iw} height=${ih} fill="transparent" class="hit" onMouseMove=${move} onMouseLeave=${() => onHover && onHover(null)} onClick=${onPin}></rect>`;
      }
    }
    let st = null;
    if (prep.vis.length > 1) {
      let mn = Infinity, mx = -Infinity, sum = 0;
      for (const p of prep.vis) { if (p.v < mn) mn = p.v; if (p.v > mx) mx = p.v; sum += p.v; }
      const ref10 = prep.vis.find(p => p.t >= last.t - 600), d = ref10 && last.t - ref10.t > 120 ? last.v - ref10.v : null;
      st = { mn, mx, av: sum / prep.vis.length, d };
    }
    return h`<div class="tc"><div class="ch" title=${shown ? (pinned ? 'Pinned at ' : pointed ? 'Value at ' : 'Latest value, ') + clock(shown.t) : 'No data'}><span class="cl">${label}</span><span class="cv" style=${'color:' + color}>${shown ? fx(shown.v, d0) : '–'}<small>${unit}</small></span></div>
      <div class="cm">${st ? h`<span>min ${fx(st.mn, d0)}</span><span>avg ${fx(st.av, d0)}</span><span>max ${fx(st.mx, d0)}</span>${st.d != null && h`<span class=${'dl ' + (Math.abs(st.d) < Math.pow(10, -d0) / 2 ? '' : st.d > 0 ? 'up' : 'dn')}>${Math.abs(st.d) < Math.pow(10, -d0) / 2 ? '\u2192' : st.d > 0 ? '\u25b2' : '\u25bc'} ${fx(Math.abs(st.d), d0)} / 10 min</span>`}` : h`<span>&nbsp;</span>`}</div>
      <div class="cbox" ref=${ref}><svg width=${W} height=${H} class="chart" role="img" aria-label=${label}>${body}</svg></div></div>`;
  }

  /* where was the satellite at time t (current TLE); also what the map marks through Orbit.probe */
  const latS = v => Math.abs(v).toFixed(2) + '°' + (v >= 0 ? 'N' : 'S'), lonS = v => Math.abs(v).toFixed(2) + '°' + (v >= 0 ? 'E' : 'W');
  const placeMemo = new Map();                                    // the place lookup scans country outlines: do it once per quarter degree
  const cachedPlace = (lon, lat) => { if (!window.placeAt) return null; const k = Math.round(lon * 4) + ',' + Math.round(lat * 4); if (!placeMemo.has(k)) { if (placeMemo.size > 400) placeMemo.clear(); placeMemo.set(k, window.placeAt(lon, lat)); } return placeMemo.get(k); };
  function ProbeLine({ t, pinned }) {
    if (t == null) return h`<div class="probe hint">Point at a chart: where the satellite was at that time. Select to keep it.</div>`;
    if (!Orbit.ok()) return h`<div class="probe hint">${clock(t)} · no orbit data, so no position. Enter it under <a href="#settings">Settings</a>.</div>`;
    const l = Orbit.look(new Date(t * 1000));
    if (!l) return h`<div class="probe hint">${clock(t)} · the orbit data cannot be propagated to this time.</div>`;
    const place = cachedPlace(l.pos.lon, l.pos.lat);
    const sun = Orbit.sunlit(new Date(t * 1000));
    const age = Orbit.tleEpochOffsetDays ? Orbit.tleEpochOffsetDays(t) : null;
    return h`<div class=${'probe ' + (pinned ? 'pinned' : '')}><b>${clock(t)}</b><span>${latS(l.pos.lat)} ${lonS(l.pos.lon)}</span>${place && h`<span>${place}</span>`}<span>altitude ${fx(l.pos.alt, 0)} km</span>
      <span>${sun == null ? '' : sun ? 'sunlit' : 'in eclipse'}</span><span>${l.el > 0 ? 'visible from the ground station, elevation ' + fx(l.el, 1) + '°' : 'below the ground-station horizon'}</span>
      <em title="Computed from the current orbit data (TLE); less exact for times long before its epoch.">${age != null && Math.abs(age) > 7 ? 'TLE epoch ' + fx(Math.abs(age), 0) + ' d away: approximate' : pinned ? 'pinned' : ''}</em></div>`;
  }

  /* orbit-derived series over [t0, t1]: elevation, altitude, speed, latitude, and the eclipse intervals */
  function orbitSeries(t0, t1, n) {
    const out = { el: [], alt: [], speed: [], lat: [], eclipse: [], passes: [] };
    if (!Orbit.ok()) return out;
    const dt = (t1 - t0) / n;
    let inEcl = null;
    for (let i = 0; i <= n; i++) {
      const t = t0 + i * dt, d = new Date(t * 1000), l = Orbit.look(d);
      if (!l) continue;
      out.el.push({ t, v: l.el }); out.alt.push({ t, v: l.pos.alt }); out.speed.push({ t, v: l.pos.speed }); out.lat.push({ t, v: l.pos.lat });
      const sun = Orbit.sunlit(d);
      if (sun === false && inEcl == null) inEcl = t;
      if (sun !== false && inEcl != null) { out.eclipse.push([inEcl, t]); inEcl = null; }
    }
    if (inEcl != null) out.eclipse.push([inEcl, t1]);
    out.passes = Orbit.passes(new Date(t0 * 1000), (t1 - t0) / 3600, 5).filter(p => p.los > t0 && p.aos < t1);
    return out;
  }

  window.MCH = { useSize, useThrottled, DEFS, series, prepare, Chart, ProbeLine, orbitSeries, cachedPlace };
})();
