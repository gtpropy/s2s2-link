/* Mission Control pages */
(function () {
  const { h, api, S, useS, utc, utcd, age, fx, Pill, Chip, Metric, Spark, Cube, attitude, useState, useEffect, useMemo } = MC;

  const last = (a, k) => { for (let i = a.length - 1; i >= 0; i--) if (a[i] && (!k || a[i][k] != null)) return a[i]; return null; };
  const col = (a, f, n = 120) => a.slice(-n).map(f).filter(v => v != null && !Number.isNaN(v));
  const limOf = (s, k) => (s.snap && s.snap.limits && s.snap.limits[k]) || null;

  /* newest magnetometer / gyro sample from housekeeping (uT, deg/s) or CW beacon (raw sensor units) */
  function imu(s, want) {
    const hk = last(s.hk), bc = last(s.beacon);
    const cands = [];
    if (hk && hk.mag && (hk.mag[0] || hk.mag[1] || hk.mag[2])) cands.push({ src: 'HK packet', t: hk.t, mag: hk.mag, gyro: hk.gyro, unit: { m: 'µT', g: '°/s' } });
    if (bc && bc.b2 && (bc.b2.magneto.some(v => v))) cands.push({ src: 'CW beacon 2', t: bc.t, mag: bc.b2.magneto, gyro: bc.b2.gyro, unit: { m: 'raw', g: 'raw' } });
    if (want === 'cw') return cands.find(c => c.src.startsWith('CW')) || null;
    if (want === 'hk') return cands.find(c => c.src.startsWith('HK')) || null;
    return cands.sort((a, b) => b.t - a.t)[0] || null;
  }

  /* ---------------------------------------------------------------- Overview */
  function Overview({ go }) {
    const s = useS();
    const hk = last(s.hk), bc = last(s.beacon);
    const b1 = bc && bc.b1;
    const bat = hk ? hk.bat_v : (b1 ? b1.bat_v : null);
    const im = imu(s);
    const rs = (s.snap && s.snap.rssi) || [];
    const lastRssi = rs.length ? rs[rs.length - 1][1] : null;
    const active = Object.values(s.jobs).filter(j => ['queued', 'sending', 'accepted'].includes(j.state));
    const alerts = alertsOf(s);
    return h`<div>
      <div class="grid">
        <div class="card"><h2>Power</h2><div class="metrics">
          <${Metric} label="Battery" value=${bat} unit="V" lim=${limOf(s, 'bat_v')} />
          <${Metric} label="Battery current" value=${hk ? hk.bat_i : (b1 ? b1.bat_i : null)} unit="A" />
          <${Metric} label="Solar" value=${hk ? hk.sol_v : (b1 ? b1.sol_total_v : null)} unit=${hk ? 'V' : ''} d=${hk ? 2 : 0} />
          <${Metric} label="Bus" value=${hk ? hk.bus_v : null} unit="V" /></div>
          <${Spark} data=${col(s.hk, e => e.bat_v)} /><div class="hint">battery voltage, last ${col(s.hk, e => e.bat_v).length} housekeeping packets</div></div>
        <div class="card"><h2>Thermal</h2><div class="metrics">
          <${Metric} label="Battery" value=${hk ? hk.bat_t : (bc && bc.b2 ? bc.b2.bat_temp_c : null)} unit="°C" d=${1} lim=${limOf(s, 'bat_t')} />
          <${Metric} label="Antenna" value=${hk ? hk.ant_t : null} unit="°C" d=${1} />
          <${Metric} label="Power board" value=${hk ? hk.bpb_t : null} unit="°C" d=${1} /></div>
          <${Spark} data=${col(s.hk, e => e.bat_t)} color="var(--warn)" /></div>
        <div class="card"><h2>Link</h2><div class="metrics">
          <${Metric} label="Last contact" value=${age(s.snap && s.snap.last_contact)} />
          <${Metric} label="RSSI" value=${lastRssi} unit="dBm" d=${0} />
          <${Metric} label="Beacons" value=${(s.snap && s.snap.counters.beacon) || 0} d=${0} />
          <${Metric} label="Commands ACKed" value=${(s.snap && s.snap.counters.ack) || 0} d=${0} /></div>
          <${Spark} data=${rs.map(r => r[1])} color="var(--cyan)" /></div>
        <div class="card"><h2>Attitude <small class="hint">${im ? im.src + ', ' + age(im.t) + ' old' : ''}</small></h2>
          <${Cube} mag=${im && im.mag} size=${210} />
          <div class="row" style="margin-top:6px"><button class="sm" onClick=${() => go('attitude')}>Attitude details</button></div></div>
      </div>
      <div class="grid wide">
        <div class="card"><h2>Alerts</h2>${alerts.length ? alerts.map((a, i) => h`<div key=${i} style="padding:3px 0"><${Pill} s=${a.level}>${a.level === 'bad' ? 'ALERT' : 'NOTE'}<//> ${a.text}</div>`) : h`<div class="hint">no alerts</div>`}</div>
        <div class="card"><h2>Command queue</h2>${active.length ? active.map(j => h`<div key=${j.id}><${Pill} s=${j.state} /> ${j.label || j.cmd} <span class="hint mono">${j.line}</span></div>`) : h`<div class="hint">idle</div>`}
          <div class="row" style="margin-top:8px"><button class="pri" onClick=${() => go('commands')}>Send a command</button></div></div>
        <div class="card"><h2>Satellite state <small class="hint">from the CW beacon</small></h2>${b1 ? h`<div class="metrics">
          <${Metric} label="System mode" value=${b1.system} /><${Metric} label="Mission" value=${b1.mission} /><${Metric} label="Hours since reset" value=${b1.hours_since_reset} d=${0} /></div>
          <div class="hint">beacon ${bc.hex} at ${utc(bc.t)}</div>` : h`<div class="hint">no beacon received yet</div>`}</div>
      </div></div>`;
  }

  function alertsOf(s) {
    const out = [];
    const hk = last(s.hk);
    const lim = (s.snap && s.snap.limits) || {};
    if (hk) {
      for (const [k, l] of Object.entries(lim)) { const v = hk[k]; if (typeof v === 'number') { if (l.lo != null && v < l.lo) out.push({ level: 'bad', text: `${k} = ${fx(v)} below ${l.lo}` }); if (l.hi != null && v > l.hi) out.push({ level: 'bad', text: `${k} = ${fx(v)} above ${l.hi}` }); } }
      if (!hk.crc_ok) out.push({ level: 'warn', text: 'last housekeeping packet failed its footer check' });
    }
    const c = s.snap && s.snap.last_contact;
    if (c && Date.now() / 1000 - c > 600) out.push({ level: 'warn', text: 'no contact for ' + age(c) + ' (beacon period is about 90 s)' });
    if (s.snap && !s.snap.gs.connected) out.push({ level: 'bad', text: 'ground station board is not connected' });
    return out;
  }

  /* ---------------------------------------------------------------- Attitude */
  function Attitude() {
    const s = useS();
    const [want, setWant] = useState('cw');
    const im = imu(s, want);
    const a = im ? attitude(im.mag) : null;
    const hist = want === 'hk' ? s.hk : s.beacon;
    const mags = hist.slice(-80).map(e => (want === 'hk' ? e.mag : e.b2 && e.b2.magneto)).filter(Boolean);
    const gyro = hist.slice(-80).map(e => (want === 'hk' ? e.gyro : e.b2 && e.b2.gyro)).filter(Boolean);
    return h`<div>
      <div class="tabs"><button class=${want === 'cw' ? 'on' : ''} onClick=${() => setWant('cw')}>CW beacon 2 (ADCS)</button><button class=${want === 'hk' ? 'on' : ''} onClick=${() => setWant('hk')}>Housekeeping IMU</button></div>
      <div class="grid wide">
        <div class="card"><h2>Orientation relative to the magnetic field</h2><${Cube} mag=${im && im.mag} size=${360} />
          <div class="hint">The cube is the satellite body (+X/+Y/+Z faces); the amber line is the measured magnetic field direction. A magnetometer alone fixes two of the three rotation angles: spin about the field axis is not observable.${im ? ' Sample from ' + im.src + ', ' + utc(im.t) + ' (' + age(im.t) + ' ago).' : ''}</div></div>
        <div class="card"><h2>Measurements</h2>
          ${im ? h`<div class="metrics">
            <${Metric} label="Mag X" value=${im.mag[0]} unit=${im.unit.m} d=${0} /><${Metric} label="Mag Y" value=${im.mag[1]} unit=${im.unit.m} d=${0} /><${Metric} label="Mag Z" value=${im.mag[2]} unit=${im.unit.m} d=${0} />
            <${Metric} label="Gyro X" value=${im.gyro[0]} unit=${im.unit.g} d=${2} /><${Metric} label="Gyro Y" value=${im.gyro[1]} unit=${im.unit.g} d=${2} /><${Metric} label="Gyro Z" value=${im.gyro[2]} unit=${im.unit.g} d=${2} /></div>
            <h2 style="margin-top:14px">Body axes vs. magnetic field</h2>
            <table><tr><th>axis</th><th>angle to B</th></tr>${['X', 'Y', 'Z'].map((n, i) => h`<tr key=${n}><td>+${n}</td><td>${fx(a.angles[i], 1)}°</td></tr>`)}</table>
            <div class="hint">|B| = ${fx(a.magnitude, 1)} ${im.unit.m}. Spin rate ${fx(Math.hypot(...im.gyro), 2)} ${im.unit.g}.</div>`
            : h`<div class="hint">No ${want === 'cw' ? 'CW beacon' : 'housekeeping'} magnetometer sample yet. ${want === 'cw' ? 'The beacon is keyed about every 90 s; the 8 data bytes are shown as ADCS (beacon 2).' : 'Send "Health / housekeeping 1" from Commands.'}</div>`}
          <div class="hint" style="margin-top:8px">All-zero sensor words (a sensor that is not wired yet) are ignored instead of drawn as an orientation.</div></div>
      </div>
      <div class="grid">${['X', 'Y', 'Z'].map((n, i) => h`<div class="card" key=${n}><h2>Magnetometer ${n}</h2><${Spark} data=${mags.map(m => m[i])} color="var(--warn)" />
        <h2 style="margin-top:10px">Gyro ${n}</h2><${Spark} data=${gyro.map(m => m[i])} color="var(--cyan)" /></div>`)}</div>
    </div>`;
  }

  /* ---------------------------------------------------------------- Telemetry */
  const HKF = [
    ['Power', [['bat_v', 'Battery', 'V'], ['sol_v', 'Solar', 'V'], ['bus_v', 'Bus', 'V'], ['bat_i', 'Battery current', 'A'], ['bus_i', 'Bus current', 'A'], ['unreg_i', 'Unregulated', 'A'], ['i_3v3', '3.3 V rail', 'A'], ['i_5v', '5 V rail', 'A']]],
    ['Thermal', [['bat_t', 'Battery', '°C', 1], ['ant_t', 'Antenna', '°C', 1], ['bpb_t', 'Power board', '°C', 1]]],
    ['Attitude sensors', [['gyro.0', 'Gyro X', '°/s'], ['gyro.1', 'Gyro Y', '°/s'], ['gyro.2', 'Gyro Z', '°/s'], ['mag.0', 'Mag X', 'µT', 0], ['mag.1', 'Mag Y', 'µT', 0], ['mag.2', 'Mag Z', 'µT', 0]]],
    ['Solar panels', [['panels_v.SP1', 'SP1', 'V'], ['panels_v.SP2', 'SP2', 'V'], ['panels_v.SP3', 'SP3', 'V'], ['panels_v.SP4', 'SP4', 'V'], ['panels_v.SP5', 'SP5', 'V']]],
  ];
  const get = (o, p) => p.split('.').reduce((a, k) => (a == null ? a : a[k]), o);

  function Telemetry() {
    const s = useS();
    const hk = last(s.hk);
    const [pick, setPick] = useState('bat_v');
    return h`<div>
      <div class="row" style="margin-bottom:12px"><${Chip} cls=${hk && hk.crc_ok ? 'ok' : (hk ? 'warn' : '')}>${hk ? (hk.crc_ok ? 'packet footers OK' : 'footer check FAILED') : 'no housekeeping yet'}<//>
        ${hk && h`<span class="hint">seq ${hk.seq} · received ${utcd(hk.t)} (${age(hk.t)} ago) · RSSI ${hk.rssi} dBm</span>`}
        <span class="grow"></span><a class="btn" href="/api/export?kind=hk">Export CSV</a></div>
      <div class="grid wide">${HKF.map(([title, fields]) => h`<div class="card" key=${title}><h2>${title}</h2><div class="metrics">
        ${fields.map(([k, l, u, d]) => h`<div key=${k} style="cursor:pointer" onClick=${() => setPick(k)}><${Metric} label=${l} value=${hk ? get(hk, k) : null} unit=${u} d=${d == null ? 2 : d} lim=${limOf(s, k)} /></div>`)}</div></div>`)}</div>
      <div class="card"><h2>History: ${pick}</h2><${Spark} data=${col(s.hk, e => get(e, pick), 400)} /><div class="hint">click any value above to chart it · ${s.hk.length} packets in memory</div></div>
      ${hk && h`<div class="card" style="margin-top:14px"><h2>Flags and raw packet</h2><div class="hint">flag 1 = ${hk.flag1}, flag 2 = ${hk.flag2}</div><div class="mono" style="word-break:break-all;color:var(--dim)">${hk.hex}</div></div>`}
    </div>`;
  }

  /* ---------------------------------------------------------------- Beacons */
  function Beacons() {
    const s = useS();
    const list = s.beacon.slice(-60).reverse();
    return h`<div class="card"><div class="row"><h2 class="grow">CW beacons</h2><a class="btn" href="/api/export?kind=beacon">Export CSV</a></div>
      <div class="hint">The 8 data bytes after "S2S2 9N2SJ" decoded as beacon 1 (power / state) and as beacon 2 (ADCS); the Packet Format sheet does not number them, so both readings are shown.</div>
      <table><tr><th>time (UTC)</th><th>bytes</th><th>system</th><th>battery</th><th>solar</th><th>gyro</th><th>magneto</th><th>bat temp</th></tr>
      ${list.map((e, i) => h`<tr key=${i}><td>${utcd(e.t)}</td><td class="mono">${e.hex}</td><td>${e.b1.system} / ${e.b1.mission}</td><td>${e.b1.bat_v} V ${e.b1.bat_i} A</td><td>${e.b1.sol_total_v} / ${e.b1.sol_total_i}</td>
        <td class="mono">${e.b2.gyro.join(', ')}</td><td class="mono">${e.b2.magneto.join(', ')}</td><td>${e.b2.bat_temp_c} °C</td></tr>`)}</table>
      ${!list.length && h`<div class="hint">no beacon decoded yet</div>`}</div>`;
  }

  /* ---------------------------------------------------------------- Commands */
  function Confirm({ cmd, onDone }) {
    const [pw, setPw] = useState('');
    return h`<div class="modal"><div class="card"><h2 style="color:var(--bad)">Confirm: ${cmd.label}</h2>
      <div>${cmd.help || ''}</div><div class="hint mono">GS line: ${cmd.gs}</div>
      <label>Enter your password to send this command</label>
      <input type="password" autofocus value=${pw} onInput=${e => setPw(e.target.value)} onKeyDown=${e => e.key === 'Enter' && onDone(pw)} />
      <div class="row"><button class="danger" onClick=${() => onDone(pw)}>Send</button><button onClick=${() => onDone(null)}>Cancel</button></div></div></div>`;
  }

  function Commands() {
    const s = useS();
    const [cat, setCat] = useState([]);
    const [args, setArgs] = useState({});
    const [ask, setAsk] = useState(null);
    const [msg, setMsg] = useState('');
    const [catErr, setCatErr] = useState('');
    useEffect(() => { api('/api/catalog').then(c => { setCat(c); setCatErr(''); }).catch(e => setCatErr('Could not load the command list: ' + e.message)); }, []);
    const groups = [...new Set(cat.map(c => c.group))];
    const a = c => Object.fromEntries((c.args || []).map(x => [x.name, (args[c.id] || {})[x.name] ?? x.default]));
    const send = async (c, pw) => { try { await api('/api/command', { id: c.id, args: a(c), password: pw }); setMsg(''); } catch (e) { setMsg(e.message); } };
    const jobs = Object.values(s.jobs).sort((x, y) => y.id - x.id).slice(0, 30);
    const connected = s.snap && s.snap.gs.connected;
    return h`<div>
      ${ask && h`<${Confirm} cmd=${ask} onDone=${pw => { const c = ask; setAsk(null); if (pw != null) send(c, pw); }} />`}
      ${msg && h`<div class="card err" style="margin-bottom:12px">${msg}</div>`}
      ${catErr && h`<div class="card err" style="margin-bottom:12px">${catErr}</div>`}
      ${!connected && h`<div class="card err" style="margin-bottom:12px">The ground station board is not connected: commands cannot be sent.</div>`}
      <div class="grid wide">${groups.map(g => h`<div class="card" key=${g}><h2>${g}</h2>
        ${cat.filter(c => c.group === g).map(c => h`<div key=${c.id} style="padding:10px 0;border-top:1px solid var(--line)">
          <div class="row"><b class="grow">${c.label}</b>
            ${(c.args || []).map(x => h`<input key=${x.name} style=${'width:' + (x.name === 'text' ? 260 : 70) + 'px'} class="mono" title=${x.label} value=${(args[c.id] || {})[x.name] ?? x.default}
              onInput=${e => setArgs({ ...args, [c.id]: { ...(args[c.id] || {}), [x.name]: e.target.value } })} />`)}
            <button class=${c.critical ? 'danger' : 'pri'} disabled=${!connected || !c.allowed} title=${!connected ? 'ground station offline' : (c.allowed ? '' : 'needs role ' + c.role)} onClick=${() => (c.critical ? setAsk(c) : send(c, ''))}>▶ Send${c.critical ? ' (password)' : ''}</button></div>
          <div class="hint">${c.help}${c.allowed ? '' : ' Your role cannot send this (needs ' + c.role + ').'}</div></div>`)}</div>`)}</div>
      <div class="card"><h2>Command log</h2><div class="hint">Each command is repeated until the satellite answers (it listens about 50 s of every 90 s), then followed to its completion ACK.</div>
        <table><tr><th>#</th><th>time</th><th>user</th><th>command</th><th>state</th><th>detail</th><th></th></tr>
        ${jobs.map(j => { const d = j.detail || {}; const ev = d.events || []; return h`<tr key=${j.id}><td>${j.id}</td><td>${utc(j.t)}</td><td>${j.user}</td><td>${j.label || j.cmd}<div class="hint mono">${j.line}</div></td>
          <td><${Pill} s=${j.state} /></td><td class="hint">${d.error || ''}${d.attempt ? ' try ' + d.attempt : ''} ${ev.filter(e => e.kind === 'ack').map(e => 'ACK ' + e.what).join(', ')} ${ev.some(e => e.kind === 'hk') ? 'housekeeping received' : ''}${d.note || ''}</td>
          <td>${['queued', 'sending', 'accepted'].includes(j.state) && h`<button class="sm" onClick=${() => api('/api/command/cancel', { id: j.id })}>cancel</button>`}</td></tr>`; })}</table></div>
    </div>`;
  }

  /* ---------------------------------------------------------------- Imagery */
  function Imagery() {
    const s = useS();
    const [sel, setSel] = useState(null);
    const cur = s.images.find(x => x.id === sel) || s.images[0];
    return h`<div class="grid wide"><div class="card"><h2>Pictures (${s.images.length})</h2><div class="gallery">${s.images.map(x => h`<figure key=${x.id} class=${cur && cur.id === x.id ? 'sel' : ''} onClick=${() => setSel(x.id)}>
        <img src=${`/api/images/${x.id}/image?v=${x.got}`} /><div class="hint">${utcd(x.t_first)}<br />${x.got}/${x.total} ${x.complete ? '✓' : '…'}</div></figure>`)}</div>
        ${!s.images.length && h`<div class="hint">No picture yet. Send "Camera: downlink the stored picture"; fragments are stored as they arrive.</div>`}</div>
      <div class="card viewer">${cur ? h`<h2>${cur.id} ${cur.complete ? h`<${Pill} s="ok">complete<//>` : h`<${Pill} s="warn">receiving<//>`}</h2>
        <img src=${`/api/images/${cur.id}/image?v=${cur.got}-${cur.complete}`} /><div class="bar" style="margin:10px 0"><i style=${'width:' + (100 * cur.got / cur.total) + '%'}></i></div>
        <div class="hint">${cur.got}/${cur.total} fragments${cur.missing.length ? ', missing ' + cur.missing.slice(0, 20).join(', ') : ''}${cur.sha256 ? ' · sha256 ' + cur.sha256.slice(0, 16) + '…' : ''}</div>
        <div class="row" style="margin-top:8px"><a class="btn" href=${`/api/images/${cur.id}/image?download=1`}>Download</a></div>` : h`<div class="hint">nothing selected</div>`}</div></div>`;
  }

  /* ---------------------------------------------------------------- Admin */
  function Admin({ me }) {
    const s = useS();
    const [users, setUsers] = useState([]);
    const [audit, setAudit] = useState([]);
    const [nu, setNu] = useState({ user: '', password: '', role: 'operator' });
    const [lim, setLim] = useState({});
    const [msg, setMsg] = useState('');
    const isAdmin = me.role === 'admin';
    useEffect(() => { if (isAdmin) { api('/api/users').then(setUsers); api('/api/audit').then(setAudit); } setLim((s.snap && s.snap.limits) || {}); }, []);
    const gs = s.snap && s.snap.gs;
    const act = (p, b) => api(p, b).then(r => { setMsg('done'); return r; }).catch(e => { setMsg(e.message); throw e; });
    return h`<div class="grid wide">
      <div class="card"><h2>Ground station</h2><div>${gs && gs.connected ? 'connected on ' + gs.port : 'not connected'} ${gs && gs.error ? h`<span class="err">${gs.error}</span>` : ''}</div>
        ${isAdmin && h`<div class="row" style="margin-top:8px"><select id="gsport" defaultValue="auto"><option value="auto">automatic</option>${(gs ? gs.ports : []).map(p => h`<option key=${p}>${p}</option>`)}</select>
          <button onClick=${() => act('/api/gs/port', { port: document.getElementById('gsport').value })}>Use this port</button></div>`}
        <h2 style="margin-top:16px">Exports</h2><div class="row"><a class="btn" href="/api/export?kind=hk">Housekeeping CSV</a><a class="btn" href="/api/export?kind=beacon">Beacons CSV</a><a class="btn" href="/api/export?kind=ack">ACK CSV</a></div></div>
      <div class="card"><h2>Alert limits</h2><div class="hint">Values outside lo / hi turn red on the dashboards and raise an alert. Empty = no limit (nothing is assumed).</div>
        <table>${['bat_v', 'bat_t', 'ant_t', 'bpb_t', 'bus_v'].map(k => h`<tr key=${k}><td>${k}</td><td><input style="width:80px" placeholder="min" value=${(lim[k] || {}).lo ?? ''} disabled=${!isAdmin} onInput=${e => setLim({ ...lim, [k]: { ...(lim[k] || {}), lo: e.target.value === '' ? null : +e.target.value } })} /></td>
          <td><input style="width:80px" placeholder="max" value=${(lim[k] || {}).hi ?? ''} disabled=${!isAdmin} onInput=${e => setLim({ ...lim, [k]: { ...(lim[k] || {}), hi: e.target.value === '' ? null : +e.target.value } })} /></td></tr>`)}</table>
        ${isAdmin && h`<button class="pri" onClick=${() => act('/api/limits', lim)}>Save limits</button>`}</div>
      ${isAdmin && h`<div class="card"><h2>Accounts</h2><table>${users.map(u => h`<tr key=${u.user}><td>${u.user}</td><td>${u.role}</td><td>${u.user !== 'admin' && h`<button class="sm danger" onClick=${() => act('/api/users/delete', { user: u.user }).then(setUsers)}>delete</button>`}</td></tr>`)}</table>
        <div class="row" style="margin-top:8px"><input placeholder="user" value=${nu.user} onInput=${e => setNu({ ...nu, user: e.target.value })} /><input type="password" placeholder="password (8+)" value=${nu.password} onInput=${e => setNu({ ...nu, password: e.target.value })} />
          <select value=${nu.role} onChange=${e => setNu({ ...nu, role: e.target.value })}><option>viewer</option><option>operator</option><option>admin</option></select>
          <button onClick=${() => act('/api/users', nu).then(r => { setUsers(r); setNu({ user: '', password: '', role: 'operator' }); })}>Add / change</button></div></div>
        <div class="card"><h2>Audit trail</h2><table>${audit.slice(0, 60).map((a, i) => h`<tr key=${i}><td>${utcd(a.t)}</td><td>${a.user}</td><td>${a.action}</td><td class="hint mono">${a.detail}</td></tr>`)}</table></div>`}
      ${msg && h`<div class="hint">${msg}</div>`}</div>`;
  }

  window.MCV = { Overview, Attitude, Telemetry, Beacons, Commands, Imagery, Admin, alertsOf };
})();
