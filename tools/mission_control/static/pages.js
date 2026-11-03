/* Mission Control screens. The dashboard summarises; every command opens its own page; history, logs, telemetry and the 3D position view have their own screens. */
(function () {
  const { h, S, api, useS, useNow, useState, useEffect, useMemo, utc, utcd, age, fx, dur, clock, last, limOf, lastContact, rssiNow, rssiWord, satState, alertsOf, imu, emit } = MC;
  const { Card, PageHead, Led, Code, Help, TEL, TelTile, Tile, HexDump, ReplyView, RawLog, STATES, RAW_ONLY } = MCP;

  /* ------------------------------------------------------------------ dashboard */
  /* The CW tape is built incrementally from the raw lines (each line is read once): the old version re-read only the last 500 lines, which is
   * less than one 30 s beacon (every Morse element prints a line), so the start of the beacon fell off. A gap of more than 6 s without a
   * character or tuning carrier starts a new beacon; a gap of more than 0.3 s between two characters is a space (the decoder reports none). */
  const CWT = { chars: '', start: 0, last: 0, lastCh: 0, tone: false, tone_t: 0, seen: new WeakSet() };
  const tapeOf = raw => {
    for (const r of raw) {
      if (!r || typeof r !== 'object' || CWT.seen.has(r)) continue;
      CWT.seen.add(r);
      const x = r.text || '';
      if (x.charCodeAt(0) !== 123) continue;                        // only the JSON lines
      if (x.includes('"CWCHAR"')) {
        const m = x.match(/"ch":"(.)"/);
        if (!m) continue;
        if (CWT.chars && r.t - CWT.last > 6) { CWT.chars = ''; CWT.start = r.t; }
        if (!CWT.chars) CWT.start = r.t;
        if (CWT.chars && r.t - CWT.lastCh > 0.3 && !CWT.chars.endsWith(' ')) CWT.chars += ' ';
        CWT.chars += m[1] === ' ' ? '' : m[1];
        CWT.last = CWT.lastCh = r.t;
      } else if (x.includes('"CWCAR"')) {
        if (r.t - CWT.last > 6) { CWT.chars = ''; CWT.start = r.t; }
        CWT.last = r.t;
      } else if (x.includes('"CWSIG"')) { CWT.tone = /"on":1/.test(x); CWT.tone_t = r.t; }
    }
    return { chars: CWT.chars, tone: CWT.tone, tone_t: CWT.tone_t, start: CWT.start, last: CWT.last };
  };
  /* slim live bar: the Morse tape of the CW beacon, in one line */
  function CwBar() {
    const s = useS();
    const now = useNow(1);
    const bc = last(s.beacon), lastText = last(s.evt.filter(e => e.kind === 'cw'));
    const tape = tapeOf(s.raw);
    const live = bc && now - bc.t < 150, decoding = tape.chars && now - tape.last < 8;
    const toneOn = tape.tone && now - tape.tone_t < 2;
    return h`<div class=${'cwbar ' + (live || decoding ? 'live' : '')} data-tour="cw" aria-live="polite"><b>CW</b><span class=${'tone ' + (toneOn ? 'on' : '')} title="Morse tone detected"></span>
      <span class=${'cwtext mono ' + (decoding || live ? '' : 'old')}>${tape.chars || (lastText ? lastText.text : 'no beacon received yet')}${decoding && now - tape.tone_t < 5 ? h`<span class="cursor">\u258c</span>` : ''}</span>
      <span class="cwmeta">${decoding ? 'decoding' : bc ? (live ? 'LIVE' : 'idle') + ' \u00b7 ' + age(bc.t) + ' ago' : 'waiting'}<a href="#telemetry" onClick=${() => { try { localStorage.setItem('s2s2.mc.teltab', 'Beacons'); } catch (e) { /* ok */ } }}>beacon details</a></span></div>`;
  }

  function CwCard() {
    const s = useS();
    const now = useNow(1);
    const [view, setView] = useState('');
    const [raw, setRaw] = useState(false);
    const bc = last(s.beacon);
    const kind = MC.beaconKind(bc);
    const since = bc ? now - bc.t : null;
    const live = since != null && since < 150;
    const tape = tapeOf(s.raw);
    const toneOn = tape.tone && now - tape.tone_t < 2;
    const lastText = last(s.evt.filter(e => e.kind === 'cw'));
    const next = bc ? 90 - (since % 90) : null;
    const v = view || (kind === 1 ? 'a' : 'b');
    const b1 = bc && bc.b1, b2 = bc && bc.b2;
    const T = (label, val, unit, d, hint) => h`<${Tile} label=${label} value=${val} unit=${unit} d=${d == null ? 2 : d} hint=${hint} />`;
    return h`<${Card} title="CW beacon" tour="cw" help="Morse-code beacon, transmitted about every 90 s. It carries the call sign and eight data bytes and remains available when other links are not."
      actions=${h`<span class=${'livepill ' + (live ? 'on' : '')}><i></i>${live ? 'Receiving' : bc ? 'Idle' : 'No beacon'}</span>`}>
      <div class="tape" aria-live="polite"><span class=${'tone ' + (toneOn ? 'on' : '')} title="Morse tone detected"></span>
        <div><div class=${'tapetext mono ' + (tape.chars || live ? '' : 'old')}>${tape.chars || (lastText ? lastText.text : '—')}${tape.chars && now - tape.tone_t < 5 ? h`<span class="cursor">▌</span>` : ''}</div>
          <div class="hint">${tape.chars && now - tape.start < 60 ? 'Decoding in progress' : !live && lastText ? 'Last decoded ' + age(lastText.t) + ' ago. Awaiting next beacon.' : bc ? `Last beacon ${age(bc.t)} ago (${clock(bc.t)}). Next expected in about ${dur(next)}.` : 'No beacon received yet.'}</div></div></div>
      ${bc && h`<div class="seg" data-tour="cwview"><button class=${v === 'a' ? 'on' : ''} onClick=${() => setView('a')}>Beacon 1: power and status</button><button class=${v === 'b' ? 'on' : ''} onClick=${() => setView('b')}>Beacon 2: sensors</button></div>
        ${v === 'a' ? h`<div class="tiles">${T('Operating mode', b1.system, '', 0)}${T('Active mission', b1.mission, '', 0)}${T('Time since reset', b1.hours_since_reset, 'h', 0)}${T('Battery voltage', b1.bat_v, 'V')}${T('Battery current', b1.bat_i, 'A')}${T('Solar current', b1.sol_total_i, '', 0, 'Raw units from the beacon.')}</div>` :
        h`<div class="tiles">${T('Angular rate X', b2.gyro[0], '', 0, 'Raw units.')}${T('Angular rate Y', b2.gyro[1], '', 0)}${T('Angular rate Z', b2.gyro[2], '', 0)}${T('Magnetic X', b2.magneto[0], '', 0)}${T('Magnetic Y', b2.magneto[1], '', 0)}${T('Magnetic Z', b2.magneto[2], '', 0)}${T('Battery temperature', b2.bat_temp_c, '°C', 0)}
          <div class="tile"><small>Illuminated panels</small><b>${Object.entries(b2.solar_cells).filter(([, x]) => x).map(([k]) => k).join(' ') || 'none'}</b></div></div>`}
        <p class="hint">The packet carries no beacon number; the beacon shown is classified from its contents (last received: beacon ${kind}). Inertial and magnetic bytes read 0 while the OBC does not supply them.</p>`}
      <div class="row"><button class="sm" data-tour="cwraw" onClick=${() => setRaw(!raw)}>${raw ? 'Hide raw' : 'Show raw'}</button><span class="hint">${s.beacon.length} ${s.beacon.length === 1 ? 'beacon' : 'beacons'} received in this session</span></div>
      ${raw && h`<div class="rawbox"><div class="hint">Beacon bytes${bc ? ' (received ' + clock(bc.t) + ')' : ''}</div>${bc ? h`<${HexDump} hex=${bc.hex} />` : h`<div class="hint">None.</div>`}
        <div class="hint">Decoder output</div><pre class="mono rawlines">${s.raw.filter(r => /CW/.test(r.text) && !/"CWSIG"/.test(r.text)).slice(-12).map(r => clock(r.t) + '  ' + r.text).join('\n') || '(none)'}</pre></div>`}
    <//>`;
  }

  function PositionLine({ compact }) {
    const now = useNow(1);
    if (!Orbit.ok()) return h`<div class="hint">Orbit data is not configured. An administrator can enter it under Settings › Satellite orbit.</div>`;
    const d = new Date(now * 1000), p = Orbit.at(d), lk = Orbit.look(d);
    if (!p) return h`<div class="hint">The orbit data cannot be propagated to the current time.</div>`;
    return h`<table class="kv"><tr><td>Latitude / longitude</td><td><b>${fx(p.lat, 2)}°, ${fx(p.lon, 2)}°</b></td></tr><tr><td>Altitude</td><td><b>${fx(p.alt, 0)} km</b></td></tr><tr><td>Velocity</td><td><b>${fx(p.speed, 2)} km/s</b></td></tr>
      <tr><td>From the ground station</td><td><b>${lk.el > 0 ? `elevation ${fx(lk.el, 0)}°, azimuth ${fx(lk.az, 0)}° (${Orbit.compass(lk.az)})` : 'below the horizon'}</b></td></tr></table>`;
  }
  /* ------------------------------------------------------------------ commands */
  const GROUPS = [['Link and housekeeping', ['health', 'ping', 'hk2', 'hkdl']], ['Missions', ['epdm', 'adcs']], ['Data files', ['epdm_file', 'adcs_file', 'satlog', 'flags', 'rsvtable']], ['Imaging', ['cam_run', 'cam_dl', 'dummycam']], ['Special', ['digi', 'antenna']]];
  function CommandsPage() {
    const s = useS();
    return h`<div class="page"><${PageHead} title="Commands" sub="Select a command to review its specification and transmit it. Each command opens on its own page." />
      ${GROUPS.map(([g, ids]) => {
      const items = ids.map(id => s.catalog.find(c => c.id === id)).filter(Boolean);
      return items.length ? h`<section class="cgroup" key=${g}><h3>${g}</h3><div class="cgrid">${items.map(c => h`<a key=${c.id} class=${'ccard ' + (c.allowed ? '' : 'locked')} href=${c.allowed ? '#cmd/' + c.id : null} data-cmd=${c.id}>
        <${Code} c=${c.icon} /><div><b>${c.simple}</b><span>${c.help.split(/(?<=\.)\s/)[0]}</span>${c.id === 'antenna' && h`<em>Confirmation required</em>`}</div></a>`)}</div></section>` : null;
    })}</div>`;
  }

  /* the image download was started: close the pop-up and show the live reception on the Images page */
  function toImages(jobId, onClose) { S.dl = { t: Date.now() / 1000, job: jobId }; if (onClose) onClose(); location.hash = 'pictures'; emit(); }
  /* modal = shown in a pop-up over the current page (onClose closes it); otherwise it is the page #cmd/<id> */
  function CommandPage({ id, modal, onClose }) {
    const s = useS();
    const cmd = s.catalog.find(c => c.id === id);
    const [args, setArgs] = useState({}), [ok, setOk] = useState(false), [err, setErr] = useState(''), [jid, setJid] = useState(null), [prac, setPrac] = useState(false), [busy, setBusy] = useState(false);
    useEffect(() => { if (cmd) setArgs(Object.fromEntries((cmd.args || []).map(a => [a.name, a.default]))); setJid(null); setPrac(false); setErr(''); setOk(false); }, [id, !!cmd]);
    if (!cmd) return h`<div class=${modal ? 'cmdmodal' : 'page'}>${modal ? h`<div class="mhead"><h2>Command</h2><button class="sm" onClick=${onClose}>Close \u2715</button></div>` : h`<${PageHead} title="Command" back=${['#commands', 'Commands']} />`}<div class="empty"><b>Command not found.</b></div></div>`;
    const job = jid && s.jobs[jid];
    const need = cmd.id === 'antenna';                                       // only the irreversible action asks for one confirmation; there is no password and no role check
    const go = () => {
      setErr('');
      if (S.practice) { setPrac(true); window.dispatchEvent(new CustomEvent('s2s2-practice-sent')); return; }
      setBusy(true);
      api('/api/command', { id: cmd.id, args }).then(r => { setJid(r.job); if (cmd.id === 'cam_dl') toImages(r.job, onClose); }).catch(e => setErr(/not connected/.test(e.message) ? 'The ground station is not connected. Connect the USB cable and select the port under Settings › Ground station.' : e.message)).finally(() => setBusy(false));
    };
    const spec = [['Command', cmd.label], ['Command bytes', cmd.opcode || '–'], ['Ground station command', (cmd.gs || '').replace('{count}', (args.count || '5')).replace('{text}', '…')], ['Expected response', cmd.reply], ['Response window', 'up to ' + Math.max(1, Math.ceil(cmd.wait_s / 60)) + ' min']];
    const close = modal ? h`<button class="btn" onClick=${onClose}>Close</button>` : h`<a class="btn" href="#commands">Back to Commands</a>`;
    const getImage = () => { setBusy(true); const dl = s.catalog.find(c => c.id === 'cam_dl'); api('/api/command', { id: 'cam_dl', args: Object.fromEntries(((dl && dl.args) || []).map(a => [a.name, a.default])) }).then(r => toImages(r.job, onClose)).catch(e => setErr(e.message)).finally(() => setBusy(false)); };
    return h`<div class=${modal ? 'cmdmodal' : 'page'}>${modal ? h`<div class="mhead"><div><h2>${cmd.simple}</h2><p class="hint">${cmd.help}</p></div><button class="sm" onClick=${onClose} aria-label="Close">Close \u2715</button></div>` : h`<${PageHead} title=${cmd.simple} sub=${cmd.help} back=${['#commands', 'Commands']} />`}
      ${S.practice && h`<div class="note warn">Interface tour active: no command is transmitted.</div>`}${S.mock && h`<div class="note warn">Mock mode: the command is sent to a simulated satellite.</div>`}
      <div class="cols2 start"><${Card} title="Specification"><table class="kv">${spec.map(([k, v]) => h`<tr key=${k}><td>${k}</td><td class=${k === 'Command bytes' ? 'mono' : ''}>${v}</td></tr>`)}</table><//>
        <${Card} title=${job ? 'Transmission' : 'Transmit'} tour="cmdform">
          ${!job && !prac && h`<div>
            ${(cmd.args || []).map(a => h`<label class="field" key=${a.name}>${a.label}<input value=${args[a.name] ?? ''} type=${a.min != null ? 'number' : 'text'} min=${a.min} max=${a.max} onInput=${e => setArgs({ ...args, [a.name]: e.target.value })} /></label>`)}
            ${need && h`<div class="note bad"><b>Irreversible action.</b> The antenna release cannot be repeated or undone.</div>
              <label class="chk"><input type="checkbox" checked=${ok} onChange=${e => setOk(e.target.checked)} /> I confirm that the antenna is to be deployed</label>`}
            ${err && h`<div class="note bad">${err}</div>`}
            <div class="row end">${modal ? h`<button class="btn" onClick=${onClose}>Cancel</button>` : h`<a class="btn" href="#commands">Cancel</a>`}<button class="pri big" data-tour="sendgo" disabled=${busy || (need && !S.practice && !ok)} onClick=${go}>${S.practice ? 'Transmit (tour)' : 'Transmit command'}</button></div></div>`}
          ${prac && h`<div class="note">During normal operation the response is displayed here: <b>Transmitted → Acknowledged → Completed</b>, followed by each decoded packet.</div><div class="row end">${close}</div>`}
          ${job && h`<${ReplyView} job=${job} />${err && h`<div class="note bad">${err}</div>`}<div class="row end"><a class="btn" href=${'#job/' + job.id}>Open in history</a>${cmd.id === 'cam_run' && ['completed', 'sent', 'accepted'].includes(job.state) && h`<button class="pri" disabled=${busy} onClick=${getImage}>Download image</button>`}${close}</div>`}<//></div></div>`;
  }

  function HistoryPage() {
    const s = useS();
    const [f, setF] = useState('');
    const jobs = Object.values(s.jobs).sort((a, b) => b.t - a.t).filter(j => !f || j.state === f);
    return h`<div class="page"><${PageHead} title="Command history" sub="All commands transmitted from this system, newest first. Select an entry to view its decoded response." />
      <${Card} title=${'Entries (' + jobs.length + ')'} actions=${h`<select value=${f} onChange=${e => setF(e.target.value)} aria-label="Filter by state"><option value="">All states</option>${Object.keys(STATES).map(k => h`<option key=${k} value=${k}>${STATES[k][0]}</option>`)}</select>`}>
        ${jobs.length ? h`<table class="tbl hist"><thead><tr><th>Time</th><th>Command</th><th>Status</th><th>Duration</th><th>Operator</th></tr></thead><tbody>${jobs.map(j => h`<tr key=${j.id} onClick=${() => { location.hash = 'job/' + j.id; }} class="clickable">
          <td>${utcd(j.t)}</td><td><b>${j.simple || j.label}</b></td><td><span class=${'pill ' + (STATES[j.state] || [0, ''])[1]}>${(STATES[j.state] || [j.state])[0]}</span></td><td>${j.t_end ? dur(j.t_end - j.t) : '–'}</td><td>${j.user || '–'}</td></tr>`)}</tbody></table>` :
      h`<div class="empty"><b>No commands recorded.</b><p>Commands transmitted from this system appear here.</p></div>`}<//></div>`;
  }
  function JobPage({ id }) {
    const s = useS();
    const job = s.jobs[id];
    return h`<div class="page"><${PageHead} title=${job ? job.simple || job.label : 'Command'} sub=${job ? 'Transmitted ' + utcd(job.t) + ' by ' + (job.user || 'unknown') : null} back=${['#history', 'Command history']} actions=${job && h`<a class="btn" href=${'#cmd/' + job.cmd}>Transmit again</a>`} />
      <${Card} title="Response">${job ? h`<${ReplyView} job=${job} key=${job.id} />` : h`<div class="empty"><b>Entry not found.</b></div>`}<//></div>`;
  }

  /* ------------------------------------------------------------------ logs, pictures */
  function AlertList() {
    const s = useS();
    const al = alertsOf(s);
    return al.length ? h`<div>${al.map((a, i) => h`<div key=${i} class=${'note ' + a.level}>${a.text}</div>`)}</div>` : h`<div class="hint">All monitored values are within their limits. Limits are configured under Settings; none are assumed.</div>`;
  }
  function LogsPage() {
    return h`<div class="page"><${PageHead} title="Ground station log" sub="Every line printed by the ground station. Filter by category, search, pause the stream, or switch to a hexadecimal view." />
      <${Card} title="Log" tour="logs"><${RawLog} /><//>
      <${Card} title="Exports"><div class="row"><a class="btn" href="/api/export?kind=hk">Housekeeping (CSV)</a><a class="btn" href="/api/export?kind=beacon">CW beacons (CSV)</a><a class="btn" href="/api/export?kind=ack">Acknowledgements (CSV)</a></div><//></div>`;
  }
  /* live reception of one picture: the fragments as they arrive and the picture decoded so far */
  function LiveImage({ im }) {
    const got = new Set();
    const miss = new Set(im.missing || []);
    for (let i = 1; i <= im.total; i++) if (!miss.has(i)) got.add(i);
    const full = im.total <= 2000 && (im.missing || []).length === im.total - im.got;           // the server lists every missing fragment
    const secs = Math.max(1, (im.complete ? im.seconds : Date.now() / 1000 - im.t_first));
    const rate = im.got / secs, left = im.total - im.got;
    return h`<div class="imglive">
      <div class="liveimg"><img class="pic" src=${im.src || `/api/images/${im.id}/image?v=${im.got}-${im.complete}`} alt="picture decoded so far" />
        <div class="liveinfo"><b>${im.complete ? 'Complete' : 'Receiving'}</b> \u00b7 ${im.got} of ${im.total} fragments (${Math.round(100 * im.got / im.total)} %)</div></div>
      <div class="livepanel">
        <div class="bar"><i style=${'width:' + (100 * im.got / im.total) + '%'}></i></div>
        <div class="stats"><div class="stat"><span>Fragments</span><b>${im.got} / ${im.total}</b></div><div class="stat"><span>Rate</span><b>${fx(rate, 2)}<i>fragments/s</i></b></div>
          <div class="stat"><span>${im.complete ? 'Duration' : 'Remaining (estimate)'}</span><b>${im.complete ? dur(im.seconds) : left > 0 && rate > 0 ? dur(left / rate) : '\u2013'}</b></div><div class="stat"><span>Duplicate fragments</span><b>${im.dups}</b></div></div>
        ${full && h`<div class="fraggrid" aria-label="Fragments received" role="img">${Array.from({ length: im.total }, (_, i) => h`<i key=${i} class=${got.has(i + 1) ? 'on' : ''} title=${'Fragment ' + (i + 1) + (got.has(i + 1) ? ' received' : ' missing')}></i>`)}</div>
          <div class="hint">Each square is one fragment (green = received). The picture on the left is rebuilt from the fragments received so far; missing parts appear as gaps until the satellite sends them.</div>`}
        ${im.complete && im.matches_sent != null && h`<div class="note ${im.matches_sent ? 'ok' : 'warn'}">${im.matches_sent ? 'The rebuilt picture matches the size and checksum announced by the satellite.' : 'The rebuilt picture does not match the announced size or checksum.'}</div>`}
        <div class="row"><a class="btn" href=${`/api/images/${im.id}/image?download=1`}>Save picture</a></div></div></div>`;
  }
  function PicturesPage() {
    const s = useS();
    const now = useNow(1);
    const [sel, setSel] = useState(null);
    const dl = S.dl && now - S.dl.t < 900 ? S.dl : null;
    const live = s.images.find(x => !x.complete && (now - x.t_last < 120 || (dl && x.t_last > dl.t - 5)));
    const cur = s.images.find(x => x.id === sel) || live || s.images[0];
    const job = dl && s.jobs[dl.job];
    return h`<div class="page"><${PageHead} title="Images" sub="Image fragments are assembled automatically as they are received."
      actions=${h`<a class="btn" href="#cmd/cam_run">Capture image</a><a class="btn pri" href="#cmd/cam_dl">Download stored image</a>`} />
      ${dl && !live && h`<${Card} title="Image download started"><div class="row"><span class="spin"></span><b>Waiting for the first fragment.</b><span class="pill ${(STATES[job && job.state] || [0, ''])[1]}">${job ? (STATES[job.state] || [job.state])[0] : 'Queued'}</span></div>
        <div class="hint">The satellite sends one fragment about every 0.8 s. They appear here as soon as they are received.</div><//>`}
      ${cur && h`<${Card} title=${(live && cur.id === live.id ? 'Live download \u00b7 ' : 'Image \u00b7 ') + cur.id} tour="livepic"><${LiveImage} im=${cur} key=${cur.id} /><//>`}
      <${Card} title=${'Received (' + s.images.length + ')'}><div class="gallery">${s.images.map(x => h`<figure key=${x.id} class=${cur && cur.id === x.id ? 'sel' : ''} onClick=${() => setSel(x.id)}>
        <img src=${x.src || `/api/images/${x.id}/image?v=${x.got}`} alt="received image" /><div class="hint">${utcd(x.t_first)}<br />${x.got}/${x.total} ${x.complete ? '\u2713' : '\u2026'}</div></figure>`)}</div>
        ${!s.images.length && h`<div class="empty"><b>No image received.</b><p>Capture an image, then use \u201cDownload image\u201d. Fragments are assembled here as they arrive.</p></div>`}<//></div>`;
  }

  /* ------------------------------------------------------------------ position (3D) */
  function LocationPage({ me }) {
    const s = useS();
    const now = useNow(1);
    const [mode, setMode] = useState('earth'), [orbit, setOrbit] = useState(true), [axes, setAxes] = useState(true), [info, setInfo] = useState(null), [fail, setFail] = useState('');
    const o = s.orbit, tle = o && o.tle;
    const [busy, setBusy] = useState(false), [msg, setMsg] = useState('');
    const ok = Orbit.ok();
    const passes = ok ? Orbit.passes(new Date(now * 1000), 24) : [];
    const ageD = Orbit.tleAgeDays();
    const refresh = () => { setBusy(true); api('/api/orbit', { refresh: true }).then(r => { s.orbit = r; Orbit.set(r.tle, r.station); setMsg(r.tle && r.tle.error ? r.tle.error : 'Orbit data updated.'); emit(); }).catch(e => setMsg(e.message)).finally(() => setBusy(false)); };
    const att = info && info.att;
    const im = imu(s), mag = im && im.unit.m === 'µT' ? im : null;
    const magn = mag ? Math.hypot(...mag.mag) : null;
    return h`<div class="page wide"><${PageHead} title="Satellite position and attitude" sub="Live three-dimensional view: Earth with Sun illumination, the satellite at its computed position, its orbit and its estimated attitude." />
      <div class="globebox" data-tour="globe">
        ${fail ? h`<div class="gfail"><b>3D view unavailable</b><p>${fail}. The 2D map is shown instead.</p><${WorldMap} big=${true} /></div>` : h`<${Globe3D} mode=${mode} orbit=${orbit} axes=${axes} onInfo=${setInfo} onFail=${setFail} />`}
        ${!fail && h`<div class="gctl"><div class="seg"><button class=${mode === 'earth' ? 'on' : ''} onClick=${() => setMode('earth')}>Earth</button><button class=${mode === 'sat' ? 'on' : ''} onClick=${() => setMode('sat')}>Satellite</button><button class=${mode === 'free' ? 'on' : ''} onClick=${() => setMode('free')}>Free</button></div>
          <label class="chk"><input type="checkbox" checked=${orbit} onChange=${e => setOrbit(e.target.checked)} /> Orbit path</label><label class="chk"><input type="checkbox" checked=${axes} onChange=${e => setAxes(e.target.checked)} /> Body axes</label></div>
        <div class="gread">${info ? h`<b>${fx(info.lat, 2)}° ${info.lat >= 0 ? 'N' : 'S'}, ${fx(Math.abs(info.lon), 2)}° ${info.lon >= 0 ? 'E' : 'W'}</b><span>Altitude ${fx(info.alt, 0)} km · ${fx(info.speed, 2)} km/s</span><span>${info.eclipse ? 'In eclipse' : 'In sunlight'}</span>` : h`<span>No orbit data</span>`}</div>
        <div class="glegend"><span><i style="background:#ff5c6c"></i>X</span><span><i style="background:#38d39f"></i>Y</span><span><i style="background:#4da3ff"></i>Z</span><span class="hint">Drag to rotate · scroll to zoom · model not to scale</span></div>`}
      </div>
      <div class="cols3"><${Card} title="Position"><${PositionLine} /><table class="kv">${info ? h`<tr><td>Range from ground station</td><td><b>${info.range ? fx(info.range, 0) + ' km' : '–'}</b></td></tr><tr><td>Orbital radius</td><td><b>${fx(info.r, 0)} km</b></td></tr>` : null}</table><//>
        <${Card} title="Attitude estimate" help="Derived from the magnetometer and a geomagnetic model. One vector observation resolves two of three rotation angles; rotation about the field direction cannot be determined.">
          ${att ? h`<table class="kv"><tr><td>Status</td><td><b>Estimated</b></td></tr><tr><td>Source</td><td><b>${att.src}, ${age(att.t)} ago</b></td></tr><tr><td>Measured field</td><td><b>${fx(magn, 1)} µT</b></td></tr><tr><td>Model field at orbit</td><td><b>${fx(att.fieldUT, 1)} µT</b></td></tr><tr><td>Deviation from nadir-pointing</td><td><b>${fx(att.offsetDeg, 1)}°</b></td></tr></table>
            <p class="hint">Reference: centred dipole model (IGRF-13 dipole), typical error of several degrees to tens of degrees. Body axes are assumed to coincide with the magnetometer axes.</p>` :
      h`<table class="kv"><tr><td>Status</td><td><b>Unavailable</b></td></tr></table><p class="hint">No magnetometer data (housekeeping packet) has been received. The model is drawn in the nominal nadir-pointing reference frame; this is not a measurement. Request a health report to obtain data.</p>`}<//>
        <${Card} title="Orbit data (TLE)" help="Two-line element set describing the orbit. Its accuracy decreases over days to weeks.">
          ${tle ? h`<table class="kv"><tr><td>Name</td><td><b>${tle.name}</b></td></tr><tr><td>NORAD catalogue number</td><td><b>${tle.norad}</b></td></tr><tr><td>Source</td><td><b>${tle.source}</b></td></tr><tr><td>Age</td><td><b class=${ageD > 14 ? 'badtxt' : ''}>${ageD == null ? '–' : fx(ageD, 1) + ' days'}</b></td></tr></table>
            ${ageD > 14 && h`<div class="note warn">The orbit data is older than 14 days; position error may be large.</div>`}${tle.error && h`<div class="note bad">${tle.error}</div>`}<pre class="mono tle">${tle.line1}\n${tle.line2}</pre>` : h`<div class="note warn">No orbit data is stored. Enter the NORAD catalogue number under Settings › Satellite orbit.</div>`}
          <div class="row">${me.role === 'admin' && tle && tle.query ? h`<button class="pri" disabled=${busy} onClick=${refresh}>${busy ? 'Updating' : 'Update TLE now'}</button>` : null}<a class="btn" href="#settings">Settings</a>${msg && h`<span class="hint">${msg}</span>`}</div><//></div>
      <${Card} title="Passes in the next 24 hours" help="Intervals during which the satellite is above the ground-station horizon.">
        ${ok ? h`<table class="tbl"><thead><tr><th>Acquisition (AOS)</th><th>Loss (LOS)</th><th>Duration</th><th>Maximum elevation</th><th>Azimuth AOS → LOS</th></tr></thead><tbody>
          ${passes.filter(p => p.los > now).slice(0, 10).map((p, i) => h`<tr key=${i} class=${p.aos <= now ? 'hot' : ''}><td>${clock(p.aos)}${p.aos <= now ? ' (in view)' : ''}</td><td>${clock(p.los)}</td><td>${dur(p.los - p.aos)}</td><td>${fx(p.maxEl, 0)}°</td><td>${Orbit.compass(p.azIn)} → ${Orbit.compass(p.azOut)}</td></tr>`)}</tbody></table>
          ${!passes.length && h`<div class="hint">No pass above 5° in the next 24 hours.</div>`}<div class="hint">Station: ${Orbit.st.name || 'Ground station'} (${fx(Orbit.st.lat, 3)}°, ${fx(Orbit.st.lon, 3)}°). Times are shown in the local time zone of this computer.</div>` : h`<div class="hint">No orbit data.</div>`}<//></div>`;
  }

  /* ------------------------------------------------------------------ help */
  const GLOSSARY = [['CW beacon', 'Morse-code transmission containing the call sign and eight data bytes, repeated about every 90 s.'], ['Telemetry', 'Measurements reported by the satellite: voltages, currents, temperatures, inertial and magnetic data.'],
    ['ACK / NACK', 'Positive or negative acknowledgement of a command. A NACK carries an error code.'], ['RSSI', 'Received signal strength indication in dBm. Values closer to 0 dBm are stronger.'],
    ['Pass', 'An interval in which the satellite is above the ground-station horizon, typically 5 to 10 minutes.'], ['AOS / LOS', 'Acquisition and loss of signal: the start and end of a pass.'],
    ['TLE', 'Two-line element set. Orbital parameters from which the position is computed.'], ['Ground station', 'The radio board connected to this computer.'],
    ['Reception window', 'The satellite transmits for about 35 s, then receives for about 50 s of each 90 s cycle. Commands are only received during the reception window.'], ['Housekeeping', 'Periodic status data of the satellite (HK packet, 128 bytes).']];
  function HelpPage({ startTour }) {
    return h`<div class="page"><${PageHead} title="Operations guide" />
      <${Card} title="Interface tour"><p>An interactive tour of the main screen. No command is transmitted while it runs.</p><button class="pri big" onClick=${startTour}>Start tour</button><//>
      <${Card} title="Status indicators"><div class="legend"><span><${Led} s="ok" /> Nominal</span><span><${Led} s="warn" /> Attention required</span><span><${Led} s="bad" /> Fault</span><span><${Led} s="none" /> No information</span></div><//>
      <${Card} title="Terms"><table class="kv">${GLOSSARY.map(([a, b]) => h`<tr key=${a}><td><b>${a}</b></td><td>${b}</td></tr>`)}</table><//>
      <${Card} title="Troubleshooting"><ul class="plain"><li><b>Ground station offline.</b> Connect the USB cable of the radio board, then select the port under Settings › Ground station.</li>
        <li><b>No response to a command.</b> The satellite may be out of range or transmitting its beacon. Check Passes today and retry during a pass.</li><li><b>Command rejected (NACK).</b> The response states the error and the corrective action.</li>
        <li><b>No orbit data.</b> An administrator enters the NORAD catalogue number under Settings › Satellite orbit.</li></ul><//></div>`;
  }

  /* ------------------------------------------------------------------ settings */
  function SettingsPage({ me, size, setSize, theme, setTheme }) {
    const s = useS();
    const [users, setUsers] = useState([]), [audit, setAudit] = useState([]), [nu, setNu] = useState({ user: '', password: '', role: 'operator' }), [lim, setLim] = useState({}), [msg, setMsg] = useState('');
    const o = s.orbit || {}, tle = o.tle || {}, st = o.station || {};
    const [launch, setLaunch] = useState(''), [norad, setNorad] = useState(''), [cospar, setCospar] = useState(''), [turl, setTurl] = useState(''), [pname, setPname] = useState(''), [ptext, setPtext] = useState(''), [sta, setSta] = useState(null);
    const isAdmin = me.role === 'admin';
    useEffect(() => { if (isAdmin && !s.mock) { api('/api/users').then(setUsers).catch(() => {}); api('/api/audit').then(setAudit).catch(() => {}); } setLim((s.snap && s.snap.limits) || {}); }, []);
    useEffect(() => { setLaunch(o.launch ? new Date(o.launch * 1000).toISOString().slice(0, 16) : ''); }, [o.launch]);
    useEffect(() => { setNorad(tle.norad || ''); setSta({ name: st.name || '', lat: st.lat, lon: st.lon, alt: st.alt }); }, [o.tle && o.tle.norad, o.station && o.station.lat]);
    const gs = s.snap && s.snap.gs;
    const act = (p, b, after) => api(p, b).then(r => { setMsg('Saved.'); after && after(r); return r; }).catch(e => setMsg(e.message));
    const setOrbit = b => act('/api/orbit', b, r => { s.orbit = r; Orbit.set(r.tle, r.station); if (r.tle && r.tle.error) setMsg(r.tle.error); emit(); });
    const input = (k, ph, w) => h`<input style=${'width:' + (w || 90) + 'px'} placeholder=${ph} value=${sta ? sta[k] ?? '' : ''} disabled=${!isAdmin} onInput=${e => setSta({ ...sta, [k]: e.target.value })} />`;
    return h`<div class="page"><${PageHead} title="Settings" sub=${msg} /><div class="cols2 start">
      <${Card} title="Display"><div class="row"><span class="lbl">Text size</span>${[['', 'Standard'], ['big', 'Large'], ['huge', 'Extra large']].map(([k, l]) => h`<button key=${k} class=${size === k ? 'pri' : ''} onClick=${() => setSize(k)}>${l}</button>`)}</div>
        <div class="row"><span class="lbl">Theme</span><button class=${theme !== 'light' ? 'pri' : ''} onClick=${() => setTheme('dark')}>Dark</button><button class=${theme === 'light' ? 'pri' : ''} onClick=${() => setTheme('light')}>Light</button></div><//>
      <${Card} title="Ground station"><div>${gs && gs.connected ? 'Connected on ' + gs.port : 'Not connected'} ${gs && gs.error ? h`<span class="badtxt">${gs.error}</span>` : ''}</div>
        ${isAdmin && h`<div class="row"><select id="gsport" defaultValue="auto"><option value="auto">Detect automatically</option>${(gs ? gs.ports : []).map(p => h`<option key=${p}>${p}</option>`)}</select>
          <button onClick=${() => act('/api/gs/port', { port: document.getElementById('gsport').value })}>Apply</button></div>`}<//>
      <${Card} title="Satellite orbit" help="Source of the position data: the NORAD catalogue number or the COSPAR id on CelesTrak, any address that answers with a TLE, or a TLE pasted by hand. CelesTrak has data only once the satellite is in the public catalogue; until then use an address or paste the TLE.">
        ${isAdmin ? h`<div class="row"><label class="field">NORAD catalogue number<input style="width:140px" value=${norad} onInput=${e => setNorad(e.target.value)} placeholder="e.g. 25544" /></label>
          <button class="pri" onClick=${() => setOrbit({ norad: +norad })}>Fetch from CelesTrak</button></div>
          <div class="row"><label class="field">COSPAR id (international designator)<input style="width:170px" value=${cospar} onInput=${e => setCospar(e.target.value)} placeholder="e.g. 2026-123A" /></label>
          <button class="pri" onClick=${() => setOrbit({ cospar })}>Fetch by COSPAR id</button></div>
          <div class="row"><label class="field">Address that returns a TLE (CelesTrak query, Space-Track, any web page with the two lines)<input class="wide" value=${turl} onInput=${e => setTurl(e.target.value)} placeholder="https://celestrak.org/NORAD/elements/gp.php?NAME=S2S2&FORMAT=tle" /></label>
          <button class="pri" onClick=${() => setOrbit({ url: turl })}>Fetch from this address</button></div>
          <details><summary>Enter a TLE manually</summary><input class="wide" placeholder="Name (optional)" value=${pname} onInput=${e => setPname(e.target.value)} />
            <textarea rows="3" class="mono wide" placeholder="1 NNNNNU ...&#10;2 NNNNN ..." value=${ptext} onInput=${e => setPtext(e.target.value)}></textarea>
            <button onClick=${() => { const L = ptext.split('\n').map(x => x.trimEnd()).filter(Boolean); setOrbit({ name: pname, line1: L[0] || '', line2: L[1] || '' }); }}>Apply</button></details>` : h`<div class="hint">Only an administrator can change this.</div>`}
        <div class="hint">${tle.name ? `Current: ${tle.name} (NORAD ${tle.norad}), ${tle.source}` : 'No orbit data stored.'}</div>
        ${tle.error && h`<div class="note bad">${tle.error}</div>`}
        <div class="row"><label class="field">Launch date and time (UTC), for the mission clock<input type="datetime-local" value=${launch} disabled=${!isAdmin} onInput=${e => setLaunch(e.target.value)} /></label>
          ${isAdmin && h`<button class="pri" onClick=${() => setOrbit({ launch })}>Save launch time</button>`}</div><//>
      <${Card} title="Ground station position" help="Used for pass prediction and elevation. Defaults to Kathmandu until set.">
        <div class="row">${input('name', 'Name', 170)}${input('lat', 'Latitude')}${input('lon', 'Longitude')}${input('alt', 'Altitude (km)', 100)}</div>
        ${isAdmin && h`<div class="row"><button class="pri" onClick=${() => setOrbit({ station: sta })}>Save position</button><span class="hint">${o.station_is_default ? 'The default position is in use.' : ''}</span></div>`}<//>
      <${Card} title="Alert limits" help="A value outside its limits is marked as a fault. An empty field means no limit."><table class="kv">${['bat_v', 'bat_t', 'ant_t', 'bpb_t', 'bus_v'].map(k => h`<tr key=${k}><td>${{ bat_v: 'Battery voltage', bat_t: 'Battery temperature', ant_t: 'Antenna temperature', bpb_t: 'Backplane temperature', bus_v: 'Bus voltage' }[k]}</td>
          <td><input style="width:70px" placeholder="min" value=${(lim[k] || {}).lo ?? ''} disabled=${!isAdmin} onInput=${e => setLim({ ...lim, [k]: { ...(lim[k] || {}), lo: e.target.value === '' ? null : +e.target.value } })} /></td>
          <td><input style="width:70px" placeholder="max" value=${(lim[k] || {}).hi ?? ''} disabled=${!isAdmin} onInput=${e => setLim({ ...lim, [k]: { ...(lim[k] || {}), hi: e.target.value === '' ? null : +e.target.value } })} /></td></tr>`)}</table>
        ${isAdmin && h`<button class="pri" onClick=${() => act('/api/limits', lim)}>Save limits</button>`}<//>
      <${MCV2.SettingsExtra} me=${me} />
      ${isAdmin && h`<${Card} title="Accounts"><table class="kv">${users.map(u => h`<tr key=${u.user}><td>${u.user}</td><td>${u.role}</td><td>${u.user !== 'admin' && h`<button class="sm danger" onClick=${() => act('/api/users/delete', { user: u.user }, setUsers)}>Delete</button>`}</td></tr>`)}</table>
        <div class="row"><input placeholder="User name" value=${nu.user} onInput=${e => setNu({ ...nu, user: e.target.value })} /><input type="password" placeholder="Password (8+ characters)" value=${nu.password} onInput=${e => setNu({ ...nu, password: e.target.value })} />
          <select value=${nu.role} onChange=${e => setNu({ ...nu, role: e.target.value })}><option>viewer</option><option>operator</option><option>admin</option></select>
          <button onClick=${() => act('/api/users', nu, r => { setUsers(r); setNu({ user: '', password: '', role: 'operator' }); })}>Add or update</button></div><//>
        <${Card} title="Audit log"><div class="joblist tall"><table class="tbl">${audit.slice(0, 60).map((a, i) => h`<tr key=${i}><td>${utcd(a.t)}</td><td>${a.user}</td><td>${a.action}</td><td class="hint mono">${a.detail}</td></tr>`)}</table></div><//>`}
      </div></div>`;
  }

  window.MCV = { CwCard, CwBar, GROUPS, CommandsPage, CommandPage, HistoryPage, JobPage, AlertList, LocationPage, LogsPage, PicturesPage, HelpPage, SettingsPage };
})();
