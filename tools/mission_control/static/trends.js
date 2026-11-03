/* Telemetry > Trends: telemetry over a chosen time frame next to the quantities computed from the orbit (elevation, altitude, speed, latitude, eclipse),
 * and the time frame with the lowest solar input. Older data is read from the database; the charts share one pointer, which also shows where the satellite was. */
(function () {
  const { h, S, api, useS, useNow, useState, useEffect, useMemo, fx, clock, age } = MC;
  const { Tile } = MCP;
  const SPANS = [['3 h', 10800], ['6 h', 21600], ['12 h', 43200], ['24 h', 86400], ['3 days', 259200]];
  const WINS = [['15 min', 900], ['30 min', 1800], ['1 h', 3600]];
  const TEL = ['bat_v', 'bus_v', 'bat_i', 'sol_v', 'bat_t', 'rssi'], ORB = ['el', 'alt', 'speed', 'lat'];

  const hhmm = t => new Date(t * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', hour12: false });
  const merge = (a, b) => { const m = new Map(); for (const x of a) m.set(x.t, x); for (const x of b) m.set(x.t, x); return [...m.values()].sort((p, q) => p.t - q.t); };

  /* sliding window over the solar-input samples: the window with the lowest (and highest) mean; needs at least 3 samples inside */
  function extremes(pts, w) {
    let lo = null, hi = null;
    for (let i = 0; i < pts.length; i++) {
      const t0 = pts[i].t;
      let j = i, sum = 0;
      while (j < pts.length && pts[j].t <= t0 + w) { sum += pts[j].v; j++; }
      const n = j - i;
      if (n < 3) continue;
      const r = { a: t0, b: pts[j - 1].t, mean: sum / n, n };
      if (!lo || r.mean < lo.mean) lo = r;
      if (!hi || r.mean > hi.mean) hi = r;
    }
    return { lo, hi };
  }
  const eclShare = (a, b, bands) => { let e = 0; for (const [x, y] of bands) e += Math.max(0, Math.min(b, y) - Math.max(a, x)); return b > a ? e / (b - a) : 0; };

  function TrendsView() {
    const s = useS();
    const now = useNow(30);
    const [span, setSpan] = useState(21600), [win, setWin] = useState(1800);
    const [hist, setHist] = useState({ hk: [], beacon: [] });
    const [hov, setHov] = useState(null), [pin, setPin] = useState(null);
    const hover = MCH.useThrottled(setHov);
    const t = pin != null ? pin : hov;
    useEffect(() => { Orbit.probe = t != null ? { t } : null; return () => { Orbit.probe = null; }; }, [t]);
    useEffect(() => {
      if (S.mock) return;
      const since = Date.now() / 1000 - span;
      Promise.all([api('/api/events?kind=hk&since=' + since), api('/api/events?kind=beacon&since=' + since)]).then(([hk, beacon]) => setHist({ hk, beacon })).catch(() => {});
    }, [span, S.mock]);
    const t0 = now - span, t1 = now;
    const src = useMemo(() => ({ hk: merge(hist.hk, s.hk), beacon: merge(hist.beacon, s.beacon), evt: s.evt }), [hist, s.hk, s.beacon, s.evt]);
    const tdata = useMemo(() => Object.fromEntries(TEL.map(k => [k, MCH.series(src, k, t0)])), [src, Math.round(t0 / 30)]);
    const orb = useMemo(() => MCH.orbitSeries(t0, t1, 240), [span, Math.floor(now / 120), Orbit.tle && Orbit.tle.line1]);
    const sol = tdata.sol_v, ex = useMemo(() => extremes(sol, win), [sol, win]);
    const mm = a => (a.length ? [Math.min(...a.map(p => p.v)), Math.max(...a.map(p => p.v))] : null);
    const eclMin = orb.eclipse.reduce((a, [x, y]) => a + (y - x), 0) / 60;
    const [spd, alt] = [mm(orb.speed), mm(orb.alt)];
    const onPin = () => setPin(pin != null ? null : hov);
    const chart = k => { const d = MCH.DEFS[k]; const pts = ORB.includes(k) ? orb[k] : tdata[k]; return h`<${MCH.Chart} key=${k} def=${d} pts=${pts} lim=${d[5] && MC.limOf(s, d[5])} win=${[t0, t1]} bands=${orb.eclipse} t=${t} pinned=${pin != null} onHover=${hover} onPin=${onPin} />`; };
    const ok = Orbit.ok();
    return h`<div class="trends">
      <div class="row"><span class="lbl">Time frame</span><div class="seg" style="margin:0">${SPANS.map(([l, v]) => h`<button key=${l} class=${span === v ? 'on' : ''} onClick=${() => setSpan(v)}>${l}</button>`)}</div>
        <span class="lbl" style="margin-left:1rem">Solar input window</span><div class="seg" style="margin:0">${WINS.map(([l, v]) => h`<button key=${l} class=${win === v ? 'on' : ''} onClick=${() => setWin(v)}>${l}</button>`)}</div></div>
      <div class="tiles wide">
        <${Tile} label="Lowest solar input" value=${ex.lo ? ex.lo.mean : null} unit="V" state=${ex.lo ? 'warn' : ''} sub=${ex.lo ? hhmm(ex.lo.a) + ' to ' + hhmm(ex.lo.b) + (ok ? ' · ' + Math.round(100 * eclShare(ex.lo.a, ex.lo.b, orb.eclipse)) + ' % in eclipse' : '') : 'needs 3 samples within the window'} hint="Mean solar input voltage in the quietest window of the chosen length. Power also needs the current, which the housekeeping packet does not carry for the whole array." />
        <${Tile} label="Highest solar input" value=${ex.hi ? ex.hi.mean : null} unit="V" state=${ex.hi ? 'ok' : ''} sub=${ex.hi ? hhmm(ex.hi.a) + ' to ' + hhmm(ex.hi.b) + (ok ? ' · ' + Math.round(100 * eclShare(ex.hi.a, ex.hi.b, orb.eclipse)) + ' % in eclipse' : '') : ''} />
        <${Tile} label="Solar input samples" value=${sol.length} d=${0} sub=${sol.length ? 'newest ' + age(sol[sol.length - 1].t) + ' ago' : 'none in this time frame'} />
        <${Tile} label="Eclipse in this frame" value=${ok ? eclMin : null} unit="min" d=${0} sub=${ok ? orb.eclipse.length + ' eclipses (computed from the orbit data)' : 'orbit data required'} />
        <${Tile} label="Passes above 5°" value=${ok ? orb.passes.length : null} d=${0} sub=${ok && orb.passes.length ? 'highest elevation ' + fx(Math.max(...orb.passes.map(q => q.maxEl)), 0) + '°' : ''} />
        <${Tile} label="Speed" value=${spd ? spd[1] : null} unit="km/s" d=${3} sub=${spd ? 'from ' + fx(spd[0], 3) + ' to ' + fx(spd[1], 3) : ''} />
        <${Tile} label="Altitude" value=${alt ? alt[1] : null} unit="km" d=${0} sub=${alt ? 'from ' + fx(alt[0], 0) + ' to ' + fx(alt[1], 0) : ''} /></div>
      <${MCH.ProbeLine} t=${t} pinned=${pin != null} />
      <p class="hint">Shaded: the satellite is in the Earth's shadow (computed from the orbit data). Lines are broken where no telemetry was received. Dense data is averaged; the faint band is the minimum to maximum.</p>
      <h4>Telemetry</h4><div class="tgrid trendgrid">${TEL.map(chart)}</div>
      <h4>From the orbit (computed)</h4>${ok ? h`<div class="tgrid trendgrid">${ORB.map(chart)}</div>` : h`<div class="hint">No orbit data. Enter it under <a href="#settings">Settings</a>.</div>`}</div>`;
  }
  MCV2.TrendsView = TrendsView;
})();
