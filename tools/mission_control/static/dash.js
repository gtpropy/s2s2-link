/* Satellite control screen: CW bar and flags on top; next pass and the command list on the left; position map and telemetry charts in the middle
 * (pointing at a chart shows where the satellite was at that moment); the received RF data and the ground-station log on the right.
 * Fills the window: no page scroll; fullscreen from the button or from Home. */
(function () {
  const { h, useS, useNow, useState, useEffect, useRef, useMemo, age, fx, clock, last, limOf, textHex } = MC;
  const { Code, STATES, RawLog } = MCP;
  const store = (k, v) => { try { localStorage.setItem(k, v); } catch (e) { /* not remembered */ } };
  const recall = k => { try { return localStorage.getItem(k) || ''; } catch (e) { return ''; } };

  /* ------------------------------------------------------------------ fullscreen */
  const isFs = () => !!(document.fullscreenElement || document.webkitFullscreenElement);
  function useFs() {
    const [f, set] = useState(isFs());
    useEffect(() => {
      const fn = () => set(isFs());
      document.addEventListener('fullscreenchange', fn); document.addEventListener('webkitfullscreenchange', fn);
      return () => { document.removeEventListener('fullscreenchange', fn); document.removeEventListener('webkitfullscreenchange', fn); };
    }, []);
    return f;
  }
  /* must run inside a click handler: browsers only grant fullscreen to a user action */
  const goFull = () => { try { const el = document.documentElement, p = el.requestFullscreen || el.webkitRequestFullscreen; if (p && !isFs()) { const r = p.call(el); if (r && r.catch) r.catch(() => {}); } } catch (e) { /* refused: the page still fills the window */ } };
  const leaveFull = () => { try { const p = document.exitFullscreen || document.webkitExitFullscreen; if (p && isFs()) p.call(document); } catch (e) { /* ignore */ } };

  /* ------------------------------------------------------------------ small helpers */
  const p2 = n => String(Math.floor(n)).padStart(2, '0');
  const hms = sec => { sec = Math.max(0, Math.round(sec)); const hh = Math.floor(sec / 3600), mm = Math.floor(sec % 3600 / 60), ss = sec % 60; return (hh ? hh + ':' + p2(mm) : p2(mm)) + ':' + p2(ss); };
  const clk24 = t => new Date(t * 1000).toLocaleTimeString('en-GB');
  const utcT = t => new Date(t * 1000).toISOString().slice(11, 19);
  const Pnl = ({ title, sub, cls, tour, actions, children }) => h`<section class=${'dpnl ' + (cls || '')} data-tour=${tour}>
    <header><h2>${title}</h2>${sub && h`<span class="psub">${sub}</span>`}<span class="grow"></span>${actions}</header><div class="pbody">${children}</div></section>`;
  const Row = ({ k, v, u, tip, cls }) => h`<div class=${'stat ' + (cls || '')} title=${tip || ''}><span>${k}</span><b>${v}${u && h`<i>${u}</i>`}</b></div>`;
  function UtcClock() { const now = useNow(1); return h`<span class="dclock mono" title="Coordinated Universal Time">${utcT(now)} UTC</span>`; }

  /* ------------------------------------------------------------------ left: next pass, commands */
  function PassPanel() {
    const s = useS();
    const now = useNow(1);
    const ok = Orbit.ok();
    const pass = ok ? Orbit.nextPass(now) : null;
    const L = s.orbit && s.orbit.launch;
    const inpass = !!pass && pass.aos <= now;
    const frac = !pass ? 0 : inpass ? (now - pass.aos) / (pass.los - pass.aos) : 1 - Math.min(1, (pass.aos - now) / 1800);
    const stName = (Orbit.st.name || 'Ground station').replace(/\s*\(.*\)\s*$/, '');
    let met = null;
    if (L) { const d = Math.abs(now - L); met = (now < L ? '\u2212' : '') + Math.floor(d / 86400) + ' d ' + p2(d % 86400 / 3600) + ':' + p2(d % 3600 / 60) + ':' + p2(d % 60); }
    return h`<${Pnl} title=${inpass ? 'Pass in progress' : 'Next pass'} sub=${ok ? stName : null} cls="pass" tour="orbit">
      ${!ok ? h`<div class="hint">No orbit data. Enter the NORAD number (or a TLE) under <a href="#settings">Settings \u203a Satellite orbit</a>: the pass times and elevation appear here.</div>` : pass ? h`
        <div class="cd"><b class=${inpass ? 'on' : ''}>${hms(inpass ? pass.los - now : pass.aos - now)}</b><span>${inpass ? 'to loss of signal (LOS)' : 'to acquisition of signal (AOS)'}</span></div>
        <div class="pbar" title=${inpass ? 'Progress of the pass' : 'Fills during the 30 minutes before acquisition'}><i style=${'width:' + (100 * frac).toFixed(1) + '%'}></i></div>
        <div class="stats">
          <${Row} k="AOS" v=${clk24(pass.aos)} u=${inpass ? 'passed' : 'in ' + hms(pass.aos - now)} tip=${'Acquisition of signal, local time. UTC ' + utcT(pass.aos)} />
          <${Row} k="LOS" v=${clk24(pass.los)} u=${'in ' + hms(pass.los - now)} tip=${'Loss of signal, local time. UTC ' + utcT(pass.los)} />
          <${Row} k="Time since launch" v=${met || 'not set'} tip=${met ? 'Counted from the launch time entered in Settings' : 'Enter the launch time under Settings \u203a Satellite orbit'} />
        </div>` : h`<div class="hint">No pass above 5\u00b0 in the next 24 hours.</div>`}<//>`;
  }

  function CommandsPanel() {
    const s = useS();
    const [open, setOpen] = useState(() => { try { return JSON.parse(localStorage.getItem('s2s2.mc.cmdopen') || 'null') || { 0: true, 1: true, 2: true, 3: true, 4: true }; } catch (e) { return { 0: true, 1: true, 2: true, 3: true, 4: true }; } });
    const toggle = i => { const o = { ...open, [i]: !open[i] }; setOpen(o); store('s2s2.mc.cmdopen', JSON.stringify(o)); };
    const jobs = Object.values(s.jobs).sort((a, b) => b.t - a.t), j = jobs[0];
    return h`<${Pnl} title="Commands" cls="cmds" tour="cmds" actions=${h`<a class="btn sm" href="#commands">Descriptions</a><a class="btn sm" href="#history">History</a>`}>
      <div class="cmdlist">${MCV.GROUPS.map(([g, ids], gi) => {
        const items = ids.map(id => s.catalog.find(c => c.id === id)).filter(Boolean);
        return items.length ? h`<div class="cgx" key=${g}><button class="cgh" onClick=${() => toggle(gi)} aria-expanded=${!!open[gi]}><i>${open[gi] ? '\u25be' : '\u25b8'}</i>${g}<span class="cnt">${items.length}</span></button>
          ${open[gi] && h`<div class="clist">${items.map(c => h`<a key=${c.id} class=${'crow ' + (c.allowed === false ? 'locked' : '')} href=${c.allowed === false ? null : '#cmd/' + c.id} data-cmd=${c.id} title=${(c.help || '').split(/(?<=\.)\s/)[0]}><${Code} c=${c.icon} /><span>${c.simple}</span>${c.id === 'antenna' && h`<em>Confirmation required</em>`}<i>\u203a</i></a>`)}</div>`}</div>` : null;
      })}${!s.catalog.length && h`<div class="hint">Loading the command list.</div>`}</div>
      ${j && h`<a class="lastcmd" href=${'#job/' + j.id}><span class="hint">Latest</span><b>${j.simple || j.label}</b><span class=${'pill ' + (STATES[j.state] || [0, ''])[1]}>${(STATES[j.state] || [j.state])[0]}</span></a>`}<//>`;
  }

  /* ------------------------------------------------------------------ middle: map */
  function MapPanel() {
    const now = useNow(0.5);
    const [cur, setCur] = useState(null);
    const ok = Orbit.ok();
    const d = new Date(now * 1000);
    const lk = ok ? Orbit.look(d) : null, p = lk && lk.pos;
    const sun = ok ? Orbit.sunlit(d) : null;
    const place = p && window.placeAt ? window.placeAt(p.lon, p.lat) : null;
    const rad = p ? 6371 * Math.acos(6371 / (6371 + p.alt)) : null;
    const sub = lk ? (lk.el > 0 ? 'In view \u00b7 elevation ' : 'Below horizon \u00b7 elevation ') + fx(lk.el, 1) + '\u00b0 \u00b7 azimuth ' + fx(lk.az, 0) + '\u00b0 ' + Orbit.compass(lk.az) + ' \u00b7 range ' + fx(lk.range, 0) + ' km' : null;
    const Item = (k, v) => h`<span class="mi"><small>${k}</small><b>${v}</b></span>`;
    return h`<${Pnl} title="Position" sub=${sub} cls="pm" tour="map" actions=${h`<a class="btn sm" href="#location">3D view</a>`}>
      <div class="mapbox"><${WorldMap} onCursor=${setCur} />
        ${p && h`<div class="mapcard" aria-label="Satellite position">${Item('Sub-satellite point', Math.abs(p.lat).toFixed(2) + '\u00b0' + (p.lat >= 0 ? 'N' : 'S') + ' ' + Math.abs(p.lon).toFixed(2) + '\u00b0' + (p.lon >= 0 ? 'E' : 'W'))}${Item('Over', place ? place.replace(/^over (the )?/, '') : '\u2013')}
          <div class="mrow"><span>Altitude</span><b>${fx(p.alt, 0)} km</b></div><div class="mrow"><span>Speed</span><b>${fx(p.speed, 2)} km/s</b></div><div class="mrow"><span>Illumination</span><b>${sun == null ? '\u2013' : sun ? 'Sunlit' : 'Eclipse'}</b></div>
          ${cur && h`<div class="mptr"><span>Pointer</span><b>${Math.abs(cur[1]).toFixed(1)}\u00b0${cur[1] >= 0 ? 'N' : 'S'} ${Math.abs(cur[0]).toFixed(1)}\u00b0${cur[0] >= 0 ? 'E' : 'W'}</b><em>${window.placeAt ? (window.placeAt(cur[0], cur[1]) || '').replace(/^(over|near) (the )?/, '') : ''}</em></div>`}</div>`}</div>
      <div class="legend2"><span><i class="l1"></i>Flown</span><span><i class="l2"></i>Predicted</span><span><i class="l6"></i>In contact</span><span><i class="l3"></i>Station 5\u00b0 limit</span><span><i class="l5"></i>Selected time</span></div><//>`;
  }

  /* ------------------------------------------------------------------ middle: interactive telemetry charts */
  const CHARTS = ['bat_v', 'bus_v', 'bat_i', 'sol_v', 'bat_t', 'rssi'];
  const WINDOWS = [['1 h', 3600], ['3 h', 10800], ['6 h', 21600], ['24 h', 86400], ['All', 0]];

  function TelPanel() {
    const s = useS();
    const now = useNow(2);
    const [win, setWin] = useState(() => { const v = recall('s2s2.mc.telwin'); return v === '' ? 10800 : +v; });
    const [hov, setHov] = useState(null), [pin, setPin] = useState(null);
    const hover = MCH.useThrottled(setHov);
    const t = pin != null ? pin : hov;
    useEffect(() => { Orbit.probe = t != null ? { t } : null; return () => { Orbit.probe = null; }; }, [t]);
    const since = win ? now - win : 0;
    const data = useMemo(() => Object.fromEntries(CHARTS.map(k => [k, MCH.series({ hk: s.hk, beacon: s.beacon, evt: s.evt }, k, since)])), [s.hk, s.beacon, s.evt, Math.round(since / 30)]);
    const onPin = () => setPin(pin != null ? null : hov);
    const orb = useMemo(() => (win && Orbit.ok() ? MCH.orbitSeries(since, now, 90) : null), [win, Math.floor(now / 60), Orbit.tle && Orbit.tle.line1]);
    return h`<${Pnl} title="Telemetry" cls="pt" tour="tel"
      actions=${h`<span class="wins">${WINDOWS.map(([l, v]) => h`<button key=${l} class=${'chipbtn ' + (win === v ? 'on' : '')} onClick=${() => { setWin(v); store('s2s2.mc.telwin', String(v)); }}>${l}</button>`)}</span><a class="btn sm pri" href="#telemetry" onClick=${() => store('s2s2.mc.teltab', 'Trends')}>Trends</a>`}>
      <div class="tgrid">${CHARTS.map(k => { const d = MCH.DEFS[k]; return h`<${MCH.Chart} key=${k} def=${d} pts=${data[k]} lim=${d[5] && limOf(s, d[5])} win=${win ? [since, now] : null} t=${t} pinned=${pin != null} onHover=${hover} onPin=${onPin} bands=${orb && orb.eclipse} passes=${orb && orb.passes.map(q => [q.aos, q.los])} />`; })}</div>
      <${MCH.ProbeLine} t=${t} pinned=${pin != null} /><//>`;
  }

  /* ------------------------------------------------------------------ right: received RF data, ground-station log */
  /* lines the ground station printed for something it received from the satellite (the Morse symbol noise is left out) */
  const RX = /\[GS RX\]|\[CW DECODED\]|"type":"(CWHK|CWRX|HK|ACK|NACK|CAMERA|FLASH)"|ASCII response|HK TELEMETRY/;
  function RxPanel() {
    const s = useS();
    const [hex, setHex] = useState(false);
    const box = useRef(null);
    const rows = s.raw.filter(r => RX.test(r.text)).slice(-200);
    useEffect(() => { if (box.current) box.current.scrollTop = box.current.scrollHeight; }, [rows.length]);
    return h`<${Pnl} title="RF RX log" sub=${rows.length + ' lines'} cls="grow1" tour="events" actions=${h`<button class=${'sm ' + (hex ? 'on' : '')} onClick=${() => setHex(!hex)} title="Show the received text as hexadecimal bytes">${hex ? 'Hex' : 'Text'}</button>`}>
      <div class="logbox mono rxbox" ref=${box} role="log" aria-live="off">${rows.length ? rows.map((r, i) => h`<div key=${r.t + ':' + i} class="ln"><span class="lt">${clock(r.t)}</span>${hex ? textHex(r.text) : r.text}</div>`) : h`<div class="hint">Nothing received from the satellite yet.</div>`}</div><//>`;
  }
  function LogPanel() {
    return h`<${Pnl} title="Ground station log" cls="grow1" tour="logs" actions=${h`<a class="btn sm" href="#logs">Open full log</a>`}><${RawLog} compact=${true} /><//>`;
  }

  /* ------------------------------------------------------------------ screen */
  function Dashboard() {
    const fs = useFs();
    return h`<div class=${'dash2 ' + (fs ? 'isfs' : '')}>
      <div class="dtop"><a class="home bk" href="#home" data-tour="home">\u2190 Back to Home</a><${MCV2.FlagRow} />
        <span class="dend"><button class="sm fsbtn" data-tour="fs" onClick=${() => (fs ? leaveFull() : goFull())} title=${fs ? 'Leave fullscreen' : 'Use the whole screen'}>${fs ? 'Minimise' : 'Fullscreen'}</button></span></div>
      <div class="dbody">
        <div class="dleft"><${PassPanel} /><${CommandsPanel} /></div>
        <div class="dmid"><${MapPanel} /><${TelPanel} /></div>
        <div class="dright"><${RxPanel} /><${LogPanel} /></div>
      </div></div>`;
  }

  MCV.Dashboard = Dashboard;
  MCV.useFs = useFs; MCV.goFull = goFull;
})();
