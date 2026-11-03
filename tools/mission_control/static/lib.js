/* Mission Control: shared store (live data over SSE), api(), small helpers. Everything the pages show comes from here. */
(function () {
  const { h, utc, utcd, age, fx } = UI;
  const { useState, useEffect, useRef, useMemo } = React;

  const realApi = async (p, body) => {
    const r = await fetch(p, body === undefined ? {} : { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) });
    const j = await r.json().catch(() => ({}));
    if (r.status === 401 && p !== '/api/login' && p !== '/api/me') { window.dispatchEvent(new Event('s2s2-auth')); }
    if (!r.ok) throw new Error(j.error || r.statusText);
    return j;
  };
  /* in mock mode every write is caught by MOCK (nothing reaches the server); reads still work */
  const api = (p, body) => (S.mock && window.MOCK ? MOCK.api(p, body, realApi) : realApi(p, body));

  /* ---- store */
  const S = { link: null, snap: null, hk: [], beacon: [], evt: [], raw: [], jobs: {}, images: [], gslink: null, catalog: [], orbit: null, modal: null, dl: null, tick: 0, mock: false, practice: false };
  const subs = new Set();
  const emit = () => { S.tick++; subs.forEach(f => f()); };
  const cap = (a, n) => (a.length > n ? a.slice(-n) : a);
  let es = null;
  function startLive() {
    if (es) es.close();
    es = new EventSource('/events');
    es.onmessage = m => { if (S.mock) return; JSON.parse(m.data).forEach(handle); emit(); };      // events come in batches: one render per batch
  }
  function handle(e) {
    if (e.kind === 'evt') {
      const v = e.ev;
      S.evt = cap(S.evt.concat(v), 400);
      if (v.kind === 'hk') S.hk = cap(S.hk.concat(v), 2000);
      if (v.kind === 'beacon') S.beacon = cap(S.beacon.concat(v), 2000);
    } else if (e.kind === 'raw') S.raw = cap(S.raw.concat(e.line), 1500);
    else if (e.kind === 'command') S.jobs = { ...S.jobs, [e.job.id]: e.job };
    else if (e.kind === 'image') { const s = { ...e }; delete s.kind; S.images = S.images.some(x => x.id === s.id) ? S.images.map(x => (x.id === s.id ? s : x)) : [s].concat(S.images); }
    else if (e.kind === 'link') S.gslink = e;
    else if (e.kind === 'hb') S.link = e.hb;
  }
  async function loadAll() {
    const [snap, hk, bc, ev, jobs, raw, cat, orb, lnk] = await Promise.all([api('/api/snapshot'), api('/api/events?kind=hk&limit=600'), api('/api/events?kind=beacon&limit=600'), api('/api/events?limit=300'),
      api('/api/commands?n=50'), api('/api/rawlog?n=400'), api('/api/catalog'), api('/api/orbit'), api('/api/link')]);
    S.snap = snap; S.hk = hk; S.beacon = bc; S.evt = ev; S.images = snap.images; S.raw = raw; S.catalog = cat; S.orbit = orb; S.link = lnk;
    S.jobs = Object.fromEntries(jobs.map(j => [j.id, j]));
    if (window.Orbit) Orbit.set(orb.tle, orb.station);
    emit();
  }
  async function poll() { if (S.mock) return; try { S.snap = await api('/api/snapshot'); S.link = await api('/api/link'); emit(); } catch (e) { /* login screen handles 401 */ } }
  const useS = () => { const [, set] = useState(0); useEffect(() => { const f = () => set(S.tick); subs.add(f); return () => subs.delete(f); }, []); return S; };
  /* current time in unix seconds, re-rendered every `sec` seconds */
  const useNow = (sec = 1) => { const [n, set] = useState(Date.now()); useEffect(() => { const t = setInterval(() => set(Date.now()), sec * 1000); return () => clearInterval(t); }, [sec]); return n / 1000; };

  /* ---- derived values */
  const last = (a, k) => { for (let i = a.length - 1; i >= 0; i--) if (a[i] && (!k || a[i][k] != null)) return a[i]; return null; };
  const limOf = (s, k) => (s.snap && s.snap.limits && s.snap.limits[k]) || null;
  const heardKinds = { ack: 1, nack: 1, beacon: 1, hk: 1, cw: 1, rssi: 1 };
  /* newest time anything from the satellite was heard (the snapshot is only polled every 3 s, events are instant) */
  function lastContact(s) {
    const L = s.link;
    let t = (s.snap && s.snap.last_contact) || 0;
    for (let i = s.evt.length - 1; i >= 0; i--) { if (heardKinds[s.evt[i].kind]) { t = Math.max(t, s.evt[i].t); break; } }
    if (L && L.enabled && L.gs && L.last_ok_t) t = Math.max(t, L.last_ok_t);      // anything heard counts, not only the heartbeat answer; "responsive" (the heartbeat) is a separate flag
    return t || null;
  }
  /* newest received-signal sample from any source: a decoded packet, a CW tuning carrier, or the heartbeat answer */
  function rssiNow(s) {
    const L = s.link;
    const c = [];
    if (L && L.enabled && L.gs && L.last_ok_t && L.rssi != null) c.push({ t: L.last_ok_t, rssi: L.rssi, snr: L.snr });
    const e = last(s.evt.filter(x => x.kind === 'rssi'));
    if (e) c.push({ t: e.t, rssi: e.rssi, snr: e.snr });
    const r = s.snap && s.snap.rssi && s.snap.rssi[s.snap.rssi.length - 1];
    if (r) c.push({ t: r[0], rssi: r[1], snr: r[2] });
    return c.length ? c.reduce((a, x) => (x.t > a.t ? x : a)) : null;
  }
  const rssiWord = r => (r == null ? ['–', ''] : r > -90 ? ['Strong', 'ok'] : r > -100 ? ['Good', 'ok'] : r > -110 ? ['Weak', 'warn'] : ['Very weak', 'bad']);
  /* the plain-language state of the satellite and of the link */
  function satState(s) {
    const gs = s.snap && s.snap.gs;
    const lc = lastContact(s);
    const bc = lastBeacon(s, 1) || last(s.beacon);
    const ageS = lc ? Date.now() / 1000 - lc : null;
    if (!s.snap) return { level: 'none', title: 'Loading', text: '', mode: null };
    if (gs && !gs.connected) return { level: 'bad', title: 'Ground station offline', text: 'The ground-station radio is not connected. Connect the USB cable, then select the port under Settings \u203a Ground station.', mode: null };
    if (s.link && s.link.enabled && s.link.gs && !s.link.responsive && !bc) return { level: 'warn', title: 'No response to heartbeat', text: 'The heartbeat (ping) is not being answered. The satellite may be out of range or transmitting its beacon.', mode: null };
    if (!lc) return { level: 'none', title: 'Awaiting signal', text: 'No transmission has been received since start-up. The CW beacon is expected about every 90 seconds while the satellite is within range.', mode: null };
    const sys = bc && bc.b1 ? bc.b1.system : null;
    const mode = sys && sys !== '?' ? sys : null;
    if (ageS > 900) return { level: 'warn', title: 'Out of contact', text: `Last reception ${age(lc)} ago. This is expected between passes.`, mode };
    if (mode === 'safe') return { level: 'warn', title: 'Safe mode', text: 'The satellite has reduced its operation to protect the batteries. Review the power telemetry.', mode };
    if (mode === 'low power') return { level: 'warn', title: 'Low-power mode', text: 'Battery energy is low; non-essential loads are switched off.', mode };
    if (mode === 'normal') return { level: 'ok', title: 'Nominal', text: 'Recent reception. The beacon reports normal operating mode.', mode };
    return { level: 'ok', title: 'Signal received', text: 'The beacon did not report an operating mode.', mode };
  }
  function alertsOf(s) {
    const out = [];
    const hk = last(s.hk);
    const lim = (s.snap && s.snap.limits) || {};
    if (hk) {
      for (const [k, l] of Object.entries(lim)) {
        const v = hk[k];
        if (typeof v === 'number') {
          if (l.lo != null && v < l.lo) out.push({ level: 'bad', text: `${k} = ${fx(v)}, below the lower limit ${l.lo}` });
          if (l.hi != null && v > l.hi) out.push({ level: 'bad', text: `${k} = ${fx(v)}, above the upper limit ${l.hi}` });
        }
      }
      if (!hk.crc_ok) out.push({ level: 'warn', text: 'The last housekeeping packet failed its checksum.' });
    }
    return out;
  }
  /* newest magnetometer / gyro sample from housekeeping (uT, deg/s) or CW beacon (raw sensor units) */
  function imu(s) {
    const hk = last(s.hk), bc = lastBeacon(s, 2);
    const c = [];
    if (hk && hk.mag && (hk.mag[0] || hk.mag[1] || hk.mag[2])) c.push({ src: 'health packet', t: hk.t, mag: hk.mag, gyro: hk.gyro, unit: { m: 'µT', g: '°/s' } });
    if (bc && bc.b2 && bc.b2.magneto.some(v => v)) c.push({ src: 'CW beacon', t: bc.t, mag: bc.b2.magneto, gyro: bc.b2.gyro, unit: { m: 'raw', g: 'raw' } });
    return c.sort((a, b) => b.t - a.t)[0] || null;
  }
  /* beacon 1 carries system status / battery (byte 1 top bits 01..11 = safe / low power / normal); beacon 2 starts with gyro bytes, which the COM sends as 0.
   * The sheet has no beacon number, so this tells them apart by the plausibility of the status bits and the battery voltage. */
  const beaconKind = b => (b && b.b1 && b.b1.system !== '?' && b.b1.bat_v >= 2 && b.b1.bat_v <= 10 ? 1 : 2);
  const lastBeacon = (s, kind) => { for (let i = s.beacon.length - 1; i >= 0; i--) if (beaconKind(s.beacon[i]) === kind) return s.beacon[i]; return null; };
  const fmtBytes = b => (b == null ? '\u2013' : b >= 1e9 ? (b / 1e9).toFixed(2) + ' GB' : b >= 1e6 ? (b / 1e6).toFixed(1) + ' MB' : b >= 1e3 ? (b / 1e3).toFixed(0) + ' kB' : b + ' B');
  /* line of contact: the satellite is above the ground-station horizon according to the TLE */
  const loc = (t) => { if (!window.Orbit || !Orbit.ok()) return null; const l = Orbit.look(new Date((t || Date.now() / 1000) * 1000)); return l ? { above: l.el > 0, el: l.el, az: l.az } : null; };
  const clock = t => new Date(t * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
  const dur = sec => { sec = Math.max(0, Math.round(sec)); const m = Math.floor(sec / 60), s = sec % 60; return m >= 60 ? `${Math.floor(m / 60)} h ${m % 60} min` : m ? `${m} min ${s} s` : `${s} s`; };
  const hexBytes = hex => (hex || '').replace(/\s+/g, '').match(/.{1,2}/g) || [];
  const textHex = t => Array.from(new TextEncoder().encode(t)).map(b => b.toString(16).padStart(2, '0')).join(' ').toUpperCase();

  window.MC = { h, api, S, useS, useNow, startLive, loadAll, poll, emit, utc, utcd, age, fx, last, limOf, lastContact, rssiNow, rssiWord, satState, alertsOf, imu, beaconKind, lastBeacon, fmtBytes, loc, clock, dur, hexBytes, textHex, useState, useEffect, useRef, useMemo };
})();
