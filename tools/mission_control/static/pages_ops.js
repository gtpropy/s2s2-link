/* Mission Control: ground-station home, link supervision, overpass alert, report viewer and the telemetry screen (housekeeping / ADCS / EPDM / digipeater). */
(function () {
  const { h, S, api, useS, useNow, useState, useEffect, useRef, utc, utcd, age, fx, dur, clock, last, limOf, emit, fmtBytes, loc, imu } = MC;
  const { Card, PageHead, Led, Code, TEL, TelTile, Tile, HexDump, FileCard, RawLog, STATES, jobEvents, decodeText } = MCP;
  const store = (k, v) => { try { localStorage.setItem(k, v); } catch (e) { /* not remembered */ } };
  const recall = k => { try { return localStorage.getItem(k) || ''; } catch (e) { return ''; } };

  function useFetch(path, ms) {
    const [d, setD] = useState(null), [e, setE] = useState('');
    const load = () => api(path).then(x => { setD(x); setE(''); }).catch(er => setE(er.message));
    useEffect(() => { load(); if (!ms) return undefined; const t = setInterval(load, ms); return () => clearInterval(t); }, [path]);
    return [d, e, load];
  }
  const yn = v => (v == null ? 'n/a' : v ? 'YES' : 'NO');

  /* ------------------------------------------------------------------ link supervision */
  function LinkCard({ compact }) {
    const s = useS();
    const now = useNow(1);
    const L = s.link;
    const lc = loc(now);
    if (!L) return h`<${Card} title="Link supervision"><div class="hint">Loading.</div><//>`;
    const set = b => api('/api/link', b).then(r => { S.link = r; emit(); }).catch(() => {});
    const due = L.next_due ? Math.max(0, Math.round(L.next_due - now)) : null;
    const sampling = !L.autosample ? 'Automatic sampling is off.' : L.in_pass && L.responsive ? 'Sampling housekeeping and ADCS data every 30 s (overpass, satellite responsive).' :
      L.sample_basis && L.sample_basis.startsWith('responsive only') ? 'Sampling every 30 s while the satellite responds (no orbit data: ' + L.sample_basis.replace(/^.*\((.*)\)$/, '$1') + ').' : 'Armed: sampling starts during an overpass once the satellite responds.';
    const state = (v, good) => (v == null ? '' : v === good ? 'ok' : 'bad');
    return h`<${Card} title="Link supervision" tour="link" help="The ground station sends a ping every 30 to 40 seconds. An answer shows that the link is established, that the satellite is in command mode (listening) and that it responds. Line of contact (LOC) is true while the satellite is above the horizon according to the orbit data."
      actions=${h`<button class=${'sm ' + (L.enabled ? 'on' : '')} onClick=${() => set({ enabled: !L.enabled })}>${L.enabled ? 'Heartbeat on' : 'Heartbeat off'}</button>`}>
      <div class="link4">
        <${Tile} label="Satellite responsive" value=${L.enabled ? yn(L.responsive) : 'Heartbeat off'} state=${L.enabled ? state(L.responsive, true) : ''} sub=${L.last_ok_t ? 'Last answer ' + age(L.last_ok_t) + ' ago' : 'No answer yet'} hint="The satellite answered a heartbeat recently." />
        <${Tile} label="Command mode" value=${L.enabled ? yn(L.command_mode) : 'n/a'} state=${L.enabled ? state(L.command_mode, true) : ''} sub="Satellite is listening for commands" hint="A heartbeat answer is only possible while the satellite is in its listening window." />
        <${Tile} label="LOC maintained" value=${lc ? yn(lc.above) : 'n/a'} state=${lc ? state(lc.above, true) : ''} sub=${lc ? (lc.above ? 'Elevation ' + fx(lc.el, 0) + '° (TLE)' : 'Below horizon (TLE)') : 'No orbit data'} hint="Line of contact: the satellite is above the ground-station horizon according to the TLE." />
        <${Tile} label="Heartbeat" value=${L.success != null ? L.success + ' %' : '–'} sub=${(L.enabled && L.gs ? (L.busy ? 'sending now' : due != null ? 'next in ' + due + ' s' : '') : 'ground station offline') + (L.rtt != null ? ' · round trip ' + fx(L.rtt, 1) + ' s' : '')} hint="Share of the last ten heartbeats that were answered." />
      </div>
      <div class="row" style="margin:.2rem 0 0;gap:1rem">${!compact && h`<label class="chk" style="margin:0"><input type="checkbox" checked=${!!L.autosample} onChange=${e => set({ autosample: e.target.checked })} /> Sample housekeeping and ADCS data every 30 s during an overpass</label>`}
        ${(L.hist || []).length > 0 && h`<span class="hbstrip" style="margin:0;min-width:8rem" title="Last heartbeats, oldest to newest">${L.hist.map((r, i) => h`<i key=${i} class=${r.ok ? 'ok' : 'bad'} title=${clock(r.t)}></i>`)}</span>`}<span class="hint">${sampling}</span></div><//>`;
  }



  /* ------------------------------------------------------------------ flag row of the satellite control page (LED pills, link group + satellite group + switches) */
  function Flag({ label, value, state, tip, tour }) {
    return h`<span class=${'flag ' + (state || 'none')} data-tip=${tip || ''} data-tour=${tour} tabIndex="0"><i class="fd"></i><b>${label}</b><span class="fv">${value}</span></span>`;
  }
  function FlagRow() {
    const s = useS();
    const now = useNow(1);
    const L = s.link, gs = s.snap && s.snap.gs;
    const st = MC.satState(s), lc = MC.lastContact(s), rs = MC.rssiNow(s), lk = loc(now);
    const [word, wcls] = MC.rssiWord(rs && now - rs.t < 900 ? rs.rssi : null);
    const ok = Orbit.ok();
    const nxt = ok ? Orbit.nextPass(now) : null;
    const inpass = !!nxt && nxt.aos <= now;
    const today = ok ? Orbit.passesToday().length : null;
    const bc = MC.lastBeacon(s, 1), hk = last(s.hk);
    const on = !!(L && L.enabled);
    const set = b => api('/api/link', b).then(r => { S.link = r; emit(); }).catch(() => {});
    const mode = st.mode;
    const ago = lc ? age(lc) : null;
    return h`<div class="flagrow" role="status">
      <div class="fgrp flags-link" data-tour="glance"><small>LINK</small>
        <${Flag} label="GS" value=${gs && gs.connected ? (gs.port || '').split('/').pop().replace('cu.', '') || 'online' : 'offline'} state=${gs && gs.connected ? 'ok' : 'bad'} tip=${gs && gs.connected ? 'Ground-station radio connected on ' + gs.port : 'Ground-station radio is not connected. Connect the USB cable, then select the port under Settings.'} />
        <${Flag} tour="link" label="RESPONSIVE" value=${on ? yn(L.responsive) : 'HB off'} state=${on ? (L.responsive ? 'ok' : 'bad') : 'none'} tip=${on ? 'The satellite answered the heartbeat (ping): the link is up and the satellite is in command mode. ' + (L.last_ok_t ? 'Last answer ' + age(L.last_ok_t) + ' ago, round trip ' + (L.rtt != null ? fx(L.rtt, 1) + ' s' : 'n/a') + ', heartbeat success ' + (L.success != null ? L.success + ' %' : 'n/a') + '.' : 'No answer yet.') : 'The heartbeat is off. Switch HB on to supervise the link.'} />
        <${Flag} label="CONTACT" value=${ago ? ago + ' ago' + (rs && now - rs.t < 900 ? ' \u00b7 ' + rs.rssi + ' dBm' : '') : 'none'} state=${!lc ? 'none' : now - lc < 300 ? (wcls === 'bad' ? 'warn' : 'ok') : 'warn'} tip=${(lc ? 'Last contact ' + clock(lc) + '. ' : 'Nothing received yet. ') + (rs && now - rs.t < 900 ? 'Signal ' + rs.rssi + ' dBm (' + word + (rs.snr != null ? ', SNR ' + rs.snr + ' dB' : '') + ').' : '') + ' Anything received counts: a decoded packet, a CW tuning carrier or the heartbeat answer.'} />
        <${Flag} label="LOC" value=${lk ? (lk.above ? 'YES \u00b7 ' + fx(lk.el, 0) + '\u00b0' : 'NO') : 'n/a'} state=${lk ? (lk.above ? 'ok' : 'none') : 'none'} tip=${lk ? 'Line of contact: the satellite is ' + (lk.above ? 'above' : 'below') + ' the ground-station horizon according to the TLE.' : 'No orbit data (TLE). Add it under Settings.'} />
        <${Flag} label="NEXT PASS" value=${!ok ? 'n/a' : inpass ? 'in progress' : nxt ? 'in ' + dur(nxt.aos - now) : 'none'} state=${inpass ? 'ok' : nxt && (nxt.aos - now) < 1800 ? 'warn' : 'none'} tip=${nxt ? 'Acquisition ' + clock(nxt.aos) + ', loss ' + clock(nxt.los) + ', maximum elevation ' + fx(nxt.maxEl, 0) + '\u00b0. A notification starts 30 minutes before.' : 'No pass in the next 24 hours, or no orbit data.'} />
        <${Flag} label="PASSES" value=${today == null ? 'n/a' : today + ' today'} state="none" tip="Passes above 5\u00b0 elevation today." /></div>
      <div class="fgrp flags-sat" data-tour="state"><small>SAT</small>
        <${Flag} label="MODE" value=${mode || '\u2013'} state=${mode === 'normal' ? 'ok' : mode ? 'warn' : 'none'} tip="Operating mode reported by CW beacon 1 (normal, low power or safe)." />
        <${Flag} label="MISSION" value=${bc ? bc.b1.mission : '\u2013'} state="none" tip="Active mission reported by CW beacon 1." />
        <${Flag} label="UPTIME" value=${bc ? bc.b1.hours_since_reset + ' h' : '\u2013'} state="none" tip="Hours since the last reset (CW beacon 1, saturates at 15)." />
        <${Flag} label="HK" value=${hk ? age(hk.t) + ' ago' : 'none'} state="none" tip="Age of the newest housekeeping packet." /></div>
      <div class="fsws"><button class=${'fsw ' + (on ? 'on' : '')} onClick=${() => set({ enabled: !on })} title=${on ? 'Heartbeat on: a ping is sent every 30 to 40 s. Click to switch off.' : 'Heartbeat off. Click to send a ping every 30 to 40 s and supervise the link.'}>HB</button>
        <button class=${'fsw ' + (L && L.autosample ? 'on' : '')} onClick=${() => L && set({ autosample: !L.autosample })} title=${'Automatic sampling of housekeeping and ADCS data every 30 s during an overpass while the satellite responds: ' + (L && L.autosample ? 'on' : 'off')}>AUTO</button></div></div>`;
  }

  /* ------------------------------------------------------------------ overpass alert */
  function OverpassAlert() {
    const now = useNow(1);
    const [acked, setAcked] = useState(() => { try { return JSON.parse(recall('s2s2.mc.pass_ack') || '{}'); } catch (e) { return {}; } });
    const ctx = useRef(null), lastBeep = useRef(0);
    const [locked, setLocked] = useState(false);
    const nxt = Orbit.ok() ? Orbit.nextPass(now) : null;
    const inPass = !!nxt && nxt.aos <= now;
    const mins = nxt && !inPass ? (nxt.aos - now) / 60 : null;
    const key = nxt ? String(Math.round(nxt.aos)) : '';
    const due = !!nxt && !inPass && mins <= 30 && !acked[key];
    useEffect(() => {          // browsers only allow sound after a click: create / resume the audio context on the first one
      const f = () => { try { ctx.current = ctx.current || new (window.AudioContext || window.webkitAudioContext)(); ctx.current.resume(); setLocked(false); } catch (e) { /* no audio */ } };
      document.addEventListener('click', f); document.addEventListener('keydown', f);
      return () => { document.removeEventListener('click', f); document.removeEventListener('keydown', f); };
    }, []);
    const alarm = (m) => {
      try {
        if (!ctx.current) { setLocked(true); return; }
        const c = ctx.current; if (c.state === 'suspended') { c.resume(); setLocked(true); }
        const t0 = c.currentTime + 0.05;
        for (let i = 0; i < 8; i++) {
          const o = c.createOscillator(), g = c.createGain();
          o.type = 'square'; o.frequency.value = i % 2 ? 660 : 1040; g.gain.value = 0.9;
          o.connect(g); g.connect(c.destination); o.start(t0 + i * 0.3); o.stop(t0 + i * 0.3 + 0.26);
        }
        if (window.speechSynthesis) { const u = new SpeechSynthesisUtterance('Overpass window starting in ' + Math.max(1, Math.round(m)) + ' minutes'); u.volume = 1; setTimeout(() => speechSynthesis.speak(u), 2500); }
      } catch (e) { /* alarm is best effort */ }
    };
    useEffect(() => { if (due && now - lastBeep.current >= 59.5) { lastBeep.current = now; alarm(mins); } if (!due) lastBeep.current = 0; }, [due, Math.floor(now)]);
    const ack = () => { const a = { ...acked, [key]: Date.now() }; setAcked(a); store('s2s2.mc.pass_ack', JSON.stringify(a)); };
    if (due) return h`<div class="alarm" role="alertdialog" aria-live="assertive"><div class="alarmbox">
      <small>OVERPASS</small><h2>Overpass window starting in ${Math.max(1, Math.ceil(mins))} ${Math.ceil(mins) <= 1 ? 'minute' : 'minutes'}</h2>
      <p>Acquisition at ${clock(nxt.aos)} (local time), maximum elevation ${fx(nxt.maxEl, 0)}°, duration ${dur(nxt.los - nxt.aos)}.</p>
      <p class="hint">This notification repeats every minute until you confirm.</p>
      <button class="pri big" onClick=${ack} data-tour="ready">Yes, I am ready</button>${locked && h`<p class="hint">Click anywhere once to enable the audible alarm in this browser.</p>`}</div></div>`;
    if (inPass) return h`<div class="inpass">Overpass in progress – ends in ${dur(nxt.los - now)} (maximum elevation ${fx(nxt.maxEl, 0)}°)</div>`;
    return null;
  }

  /* ------------------------------------------------------------------ home */
  function Bar({ v, max }) { return h`<div class="bar"><i style=${'width:' + Math.min(100, max ? 100 * v / max : 0) + '%'}></i></div>`; }
  function HomePage() {
    const s = useS();
    const [home, herr, reload] = useFetch('/api/home', 8000);
    const [drive, , reloadDrive] = useFetch('/api/drive/stats?age=120', 60000);
    const [reps, , reloadReps] = useFetch('/api/reports', 60000);
    const [pick, setPick] = useState(''), [msg, setMsg] = useState(''), [busy, setBusy] = useState('');
    const L = s.link, gs = s.snap && s.snap.gs;
    const days = (reps && reps.days) || [];
    const open = d => { if (d.local) { location.hash = 'report/' + encodeURIComponent(d.name); return; }
      setBusy(d.name); setMsg('Downloading ' + d.name + ' from Google Drive'); api('/api/reports/pull', { name: d.name }).then(() => { setMsg(''); location.hash = 'report/pulled/' + encodeURIComponent(d.name); }).catch(e => setMsg(e.message)).finally(() => setBusy('')); };
    const upload = () => { setBusy('upload'); setMsg('Uploading to Google Drive'); api('/api/store/upload', {}).then(r => { setMsg(r.configured === false ? 'Google Drive is not configured.' : 'Uploaded ' + (r.uploaded || 0) + ' file(s)' + ((r.errors || []).length ? ', ' + r.errors.length + ' error(s)' : '')); reload(); reloadDrive(); reloadReps(); }).catch(e => setMsg(e.message)).finally(() => setBusy('')); };
    const st = home && home.store, dr = home && home.drive;
    const local = (home && home.days) || [];
    const chosen = days.find(d => d.name === pick);
    return h`<div class="page wide"><div class="homehero"><div><small>S2S GROUND STATION</small><h1>Mission control overview</h1><p>Ground-station status, data storage, Google Drive backup and operational reports.</p></div>
        <a class="btn pri huge" href="#dashboard" data-tour="gosat" onClick=${() => MCV.goFull()}>Go to satellite control →</a></div>
      ${herr && h`<div class="note bad">${herr}</div>`}
      <div class="cols3">
        <${Card} title="Ground station"><table class="kv"><tr><td>Radio board</td><td><b>${gs && gs.connected ? 'Connected' : 'Disconnected'}</b></td></tr><tr><td>Serial port</td><td><b>${gs && gs.port ? gs.port : '–'}</b></td></tr>
          <tr><td>Satellite responsive</td><td><b>${L && L.enabled ? yn(L.responsive) : 'Heartbeat off'}</b></td></tr><tr><td>Last contact</td><td><b>${L && L.last_ok_t ? age(L.last_ok_t) + ' ago' : 'none'}</b></td></tr>
          <tr><td>Received signal</td><td><b>${L && L.rssi != null ? L.rssi + ' dBm' : '–'}</b></td></tr>
          <tr><td>Next overpass</td><td><b>${L && L.next_pass ? 'in ' + dur(L.next_pass.aos - Date.now() / 1000) : L && L.pass_reason ? L.pass_reason : '–'}</b></td></tr></table><//>
        <${Card} title="Local storage" help="Everything the ground station records is stored in the GS_Data folder next to the program, one folder per day, in the same layout as the operational reports."><table class="kv">
          <tr><td>GS_Data size</td><td><b>${st ? fmtBytes(st.bytes) : '–'}</b></td></tr><tr><td>Files</td><td><b>${st ? st.files : '–'}</b></td></tr>
          <tr><td>Today’s folder</td><td><b>${st && st.today.name ? st.today.name : '–'}</b></td></tr><tr><td>Today’s data</td><td><b>${st ? fmtBytes(st.today.bytes) + ', ' + st.today.files + ' files' : '–'}</b></td></tr>
          <tr><td>Last write</td><td><b>${st && st.last_flush ? age(st.last_flush) + ' ago' : '–'}</b></td></tr></table>${st && st.error && h`<div class="note bad">${st.error}</div>`}
          <p class="hint">Folders older than ${home ? home.retention_days : 7} days are removed from this computer once they are on Google Drive.</p><//>
        <${Card} title="Google Drive" tour="drive" actions=${h`<button class="sm" disabled=${!!busy} onClick=${upload}>${busy === 'upload' ? 'Uploading' : 'Upload now'}</button>`}>${!dr || !dr.configured ? h`<div class="note warn">Google Drive is not configured. Reports are not uploaded to the S2S_operational_reports folder and older days cannot be retrieved.</div><a class="btn" href="#settings">Open settings</a>` :
          !drive ? h`<div class="hint">Reading Google Drive.</div>` : drive.error && !drive.used ? h`<div class="note bad">${drive.error}</div>` : h`<table class="kv"><tr><td>Account</td><td><b>${drive.user || '–'}</b></td></tr>
            <tr><td>Drive used</td><td><b>${fmtBytes(drive.used)}${drive.limit ? ' of ' + fmtBytes(drive.limit) : ''}</b></td></tr></table>${drive.limit ? h`<${Bar} v=${drive.used} max=${drive.limit} />` : null}
            <table class="kv"><tr><td>Report folder</td><td><b>${fmtBytes(drive.folder_bytes)} in ${drive.folder_files} files</b></td></tr><tr><td>Last upload</td><td><b>${drive.last_upload ? age(drive.last_upload) + ' ago' : 'none yet'}</b></td></tr></table>${drive.error && h`<div class="note bad">${drive.error}</div>`}`}<//></div>
      <${Card} title="Operational reports" tour="reports" help="One folder per day. Past seven days are kept on this computer; older days are on Google Drive and are downloaded when opened.">
        <div class="row"><select value=${pick} onChange=${e => setPick(e.target.value)} aria-label="Select a day"><option value="">Select a day…</option>${days.map(d => h`<option key=${d.name} value=${d.name}>${d.name} – ${d.local ? 'on this computer' : 'Google Drive'}</option>`)}</select>
          <button class="pri" disabled=${!chosen || !!busy} onClick=${() => chosen && open(chosen)}>${chosen && !chosen.local ? 'Download and open' : 'Open report'}</button>${msg && h`<span class="hint">${msg}</span>`}</div>
        ${local.length ? h`<table class="tbl"><thead><tr><th>Day folder</th><th>Size</th><th>Files</th><th>Google Drive</th><th></th></tr></thead><tbody>${local.slice(0, 10).map(d => h`<tr key=${d.name} class="clickable" onClick=${() => open({ ...d, local: true })}>
          <td><b>${d.name}</b></td><td>${fmtBytes(d.bytes)}</td><td>${d.files}</td><td>${d.uploaded ? h`<span class="pill ok">Uploaded</span>` : h`<span class="pill warn">Not uploaded</span>`}</td><td><a href=${'#report/' + encodeURIComponent(d.name)}>Open</a></td></tr>`)}</tbody></table>` :
      h`<div class="empty"><b>No report folder yet.</b><p>A folder is created automatically for each day.</p></div>`}
        ${reps && reps.drive_error && h`<div class="note warn">Google Drive list unavailable: ${reps.drive_error}</div>`}<//></div>`;
  }

  /* ------------------------------------------------------------------ report viewer */
  function SheetTable({ rows, max = 200 }) {
    if (!rows || !rows.length) return h`<div class="hint">Empty sheet.</div>`;
    return h`<div class="sheet"><table class="tbl dense"><thead><tr>${rows[0].map((c, i) => h`<th key=${i}>${String(c)}</th>`)}</tr></thead><tbody>${rows.slice(1, max + 1).map((r, i) => h`<tr key=${i}>${rows[0].map((_, j) => h`<td key=${j} class=${String(r[j] || '').length > 40 ? 'mono' : ''}>${r[j] == null ? '' : String(r[j])}</td>`)}</tr>`)}</tbody></table>${rows.length > max + 1 && h`<div class="hint">First ${max} of ${rows.length - 1} rows.</div>`}</div>`;
  }
  function FileView({ q, path }) {
    const ext = (path.match(/\.([a-z0-9]+)$/i) || [])[1] || '';
    const url = k => `/api/report/${k}?${q}&path=${encodeURIComponent(path)}`;
    const [d, setD] = useState(null), [err, setErr] = useState('');
    useEffect(() => {
      setD(null); setErr('');
      if (ext === 'xlsx') api(url('sheet')).then(setD).catch(e => setErr(e.message));
      else if (ext === 'docx') api(url('docx')).then(setD).catch(e => setErr(e.message));
      else if (['txt', 'log'].includes(ext)) fetch(url('file')).then(r => r.text()).then(t => setD({ text: t })).catch(e => setErr(e.message));
    }, [path, q]);
    if (['jpeg', 'jpg', 'png'].includes(ext)) return h`<img class="pic" src=${url('file')} alt=${path} />`;
    if (err) return h`<div class="note bad">${err}</div>`;
    if (!d) return ['xlsx', 'docx', 'txt', 'log'].includes(ext) ? h`<div class="hint">Loading.</div>` : h`<a class="btn" href=${url('file') + '&download=1'}>Download</a>`;
    if (ext === 'xlsx') return h`<div>${d.map(sh => h`<div key=${sh.name}><h4>${sh.name}</h4><${SheetTable} rows=${sh.rows} /></div>`)}</div>`;
    return h`<pre class="filetext mono">${d.text}</pre>`;
  }
  function ReportPage({ args }) {
    const pulled = args[0] === 'pulled';
    const name = pulled ? args[1] : args[0];
    const q = `name=${encodeURIComponent(name || '')}`;
    const [rep, err] = useFetch('/api/report?' + q);
    const [tab0, setTab] = useState(''), [file, setFile] = useState('');
    if (err) return h`<div class="page"><${PageHead} title="Report" section=${false} /><div class="note bad">${err}</div></div>`;
    if (!rep) return h`<div class="page"><${PageHead} title="Report" section=${false} /><div class="hint">Loading.</div></div>`;
    const g = rep.gs_report || {}, files = rep.files || [];
    const tab = tab0 || (g['At a glance'] ? 'At a glance' : 'Summary');
    const sum = g.Summary || [];
    const tabs = [...(g['At a glance'] ? ['At a glance', 'Anomalies', 'Contacts', 'Satellite health', 'Day: Link quality', 'Environment'] : []), 'Summary', 'Commands', 'Overpasses', 'Link (heartbeat)', 'Signal at commands', 'ADCS', 'EPDM', 'CAM', 'DPM', 'Files'];
    const missionFiles = m => files.filter(f => f.path.startsWith('Storage/Mission_Details/' + m + '/'));
    const body = () => {
      if (tab === 'Summary') return h`<div class="tiles wide">${sum.slice(1).map(r => h`<${Tile} key=${r[0]} label=${r[0]} value=${r[1] == null ? '–' : r[1]} d=${Number.isInteger(r[1]) ? 0 : 1} />`)}</div>`;
      if (g[tab]) return h`<${SheetTable} rows=${g[tab]} max=${400} />`;
      if (['ADCS', 'EPDM', 'CAM', 'DPM'].includes(tab)) {
        const fs = missionFiles(tab), x = fs.find(f => f.path.endsWith('.xlsx')), lg = fs.find(f => f.path.endsWith('operation_raw.log')), resp = fs.filter(f => f.path.includes('/data/'));
        return h`<div>${x && h`<${FileView} q=${q} path=${x.path} />`}
          <h4>Responses (${resp.length})</h4>${resp.length ? h`<div class="joblist">${resp.map(f => h`<button key=${f.path} class=${'jrow ' + (file === f.path ? 'on' : '')} onClick=${() => setFile(file === f.path ? '' : f.path)}>${f.path.split('/').pop()}<span class="grow"></span><span class="hint">${fmtBytes(f.bytes)}</span></button>`)}</div>` : h`<div class="hint">No stored responses for this mission on this day.</div>`}
          ${file && fs.some(f => f.path === file) && h`<${FileView} q=${q} path=${file} />`}
          ${lg && h`<h4>Operation log (raw)</h4><${FileView} q=${q} path=${lg.path} />`}</div>`;
      }
      return h`<div class="cols2 start"><div class="joblist tall">${files.map(f => h`<button key=${f.path} class=${'jrow ' + (file === f.path ? 'on' : '')} onClick=${() => setFile(f.path)}><span class="mono">${f.path}</span><span class="grow"></span><span class="hint">${fmtBytes(f.bytes)}</span></button>`)}</div>
        <div>${file ? h`<${FileView} q=${q} path=${file} />` : h`<div class="hint">Select a file to view it here. Nothing needs to be opened in another program.</div>`}</div></div>`;
    };
    return h`<div class="page wide"><${PageHead} title=${name} section=${false} sub=${(pulled ? 'Downloaded from Google Drive' : 'On this computer') + ' · ' + files.length + ' files'}
        actions=${h`<span class="hint">${fmtBytes(files.reduce((a, f) => a + f.bytes, 0))}</span>`} />
      <div class="seg wrap">${tabs.map(t => h`<button key=${t} class=${tab === t ? 'on' : ''} onClick=${() => setTab(t)}>${t}</button>`)}</div>
      <${Card} title=${tab}>${body()}<//></div>`;
  }

  /* ------------------------------------------------------------------ telemetry */
  function jobText(job) {
    const ev = jobEvents(job).filter(e => e.kind === 'file');
    const by = new Map(); ev.forEach(c => by.set(c.pkt, c));
    const bytes = [];
    [...by.keys()].sort((a, b) => a - b).forEach(k => { const c = by.get(k); (c.hex.match(/.{2}/g) || []).slice(0, c.len || 128).forEach(x => bytes.push(parseInt(x, 16))); });
    while (bytes.length && bytes[bytes.length - 1] === 0) bytes.pop();
    return { chunks: ev, bytes };
  }
  function MissionTelemetry({ mission, cmds, file }) {
    const s = useS();
    const [open, setOpen] = useState(null);
    const jobs = Object.values(s.jobs).filter(j => cmds.includes(j.cmd)).sort((a, b) => b.t - a.t).slice(0, 25);
    const data = jobs.filter(j => j.cmd === file);
    const lastRun = jobs.find(j => j.cmd !== file);
    return h`<div><div class="note">The ${mission} data format is not final. Samples are shown as the text decoded from the received bytes, without interpretation.</div>
      <div class="row"><a class="btn pri" href=${'#cmd/' + file}>Read ${mission} data file</a><a class="btn" href=${'#cmd/' + mission.toLowerCase()}>Start ${mission} mission</a>${lastRun && h`<span class="hint">Last mission start: ${age(lastRun.t)} ago, ${(STATES[lastRun.state] || [lastRun.state])[0]}</span>`}</div>
      ${data.length ? h`<table class="tbl"><thead><tr><th>Time (local)</th><th>Source</th><th>Status</th><th>Bytes</th><th>Decoded text (first line)</th></tr></thead><tbody>${data.map(j => { const t = jobText(j); return h`<${React.Fragment} key=${j.id}>
        <tr class="clickable" onClick=${() => setOpen(open === j.id ? null : j.id)}><td>${clock(j.t)}</td><td>${j.user === 'autosample' ? 'Automatic sampling' : 'Manual'}</td><td><span class=${'pill ' + (STATES[j.state] || [0, ''])[1]}>${(STATES[j.state] || [j.state])[0]}</span></td>
          <td>${t.bytes.length}</td><td class="mono">${decodeText(t.bytes).split('\n')[0].slice(0, 90) || (j.state === 'nack' ? 'file not present on board' : '')}</td></tr>
        ${open === j.id && h`<tr><td colSpan="5">${t.chunks.length ? h`<${FileCard} chunks=${t.chunks} name=${file === 'epdm_file' ? 'epdm.txt' : 'adcs.txt'} rawOnly=${true} />` : h`<div class="hint">No file data in this response.</div>`}</td></tr>`}<//>`; })}</tbody></table>` :
      h`<div class="empty"><b>No ${mission} data received yet.</b><p>The ground station requests ADCS data automatically every 30 s during an overpass while the satellite responds; ${mission} data can also be requested manually.</p></div>`}</div>`;
  }
  function DigiTelemetry() {
    const s = useS();
    const jobs = Object.values(s.jobs).filter(j => j.cmd === 'digi').sort((a, b) => b.t - a.t);
    const heard = s.evt.filter(e => e.kind === 'digi' || (e.kind === 'ascii' && /[>].*:/.test(e.text))).slice(-20).reverse();
    return h`<div><div class="note">The satellite does not report digipeater statistics yet. This view lists the test frames sent through it and the frames repeated on the downlink that were received.</div>
      <div class="tiles"><${Tile} label="Frames sent" value=${jobs.length} d=${0} /><${Tile} label="Repeats received" value=${heard.length} d=${0} />
        <${Tile} label="Digipeater alias" value="APNDIG" /></div>
      <div class="row"><a class="btn pri" href="#cmd/digi">Send a digipeater test frame</a></div>
      <h4>Frames sent</h4>${jobs.length ? h`<table class="tbl"><thead><tr><th>Time</th><th>Frame</th><th>Status</th></tr></thead><tbody>${jobs.slice(0, 20).map(j => h`<tr key=${j.id}><td>${clock(j.t)}</td><td class="mono">${j.line.replace(/^DIGI /, '')}</td><td>${(STATES[j.state] || [j.state])[0]}</td></tr>`)}</tbody></table>` : h`<div class="hint">None.</div>`}
      <h4>Repeats received</h4>${heard.length ? h`<table class="tbl"><tbody>${heard.map((e, i) => h`<tr key=${i}><td>${clock(e.t)}</td><td class="mono">${e.text}</td></tr>`)}</tbody></table>` : h`<div class="hint">None.</div>`}</div>`;
  }
  function TelemetryPage() {
    const s = useS();
    const [tab, setTab] = useState(() => { let t = 'Housekeeping'; try { t = localStorage.getItem('s2s2.mc.teltab') || t; localStorage.removeItem('s2s2.mc.teltab'); } catch (e) { /* none */ } return t; });
    const m = imu(s);
    const tabs = ['Housekeeping', 'Trends', 'Beacons', 'ADCS', 'EPDM', 'Digipeater'];
    return h`<div class="page"><${PageHead} title="Telemetry" sub="Telemetry by subsystem. Housekeeping is only one part of it: ADCS, EPDM and digipeater data have their own views."
      actions=${h`<a class="btn pri" href="#cmd/health">Request health report</a><a class="btn" href="/api/export?kind=hk">Export CSV</a>`} />
      <div class="seg" data-tour="teltabs">${tabs.map(t => h`<button key=${t} class=${tab === t ? 'on' : ''} onClick=${() => setTab(t)}>${t}</button>`)}</div>
      ${tab === 'Housekeeping' && h`<div>${TEL.map(([g, defs]) => h`<${Card} key=${g} title=${g}><div class="tiles wide">${defs.map(d => h`<${TelTile} key=${d[0]} s=${s} def=${d} spark=${true} />`)}</div><//>`)}
        <${Card} title="Magnetic field orientation" help="Orientation of the satellite body relative to the measured magnetic field. A magnetometer alone cannot resolve rotation about the field direction.">
          ${m ? h`<div class="two"><${UI.Cube} mag=${m.mag} size=${280} /><div><table class="kv"><tr><td>Source</td><td><b>${m.src}</b> (${age(m.t)} ago)</td></tr>
            <tr><td>Field vector</td><td><b>${m.mag.map(v => fx(v, 1)).join(', ')} ${m.unit.m}</b></td></tr><tr><td>Angular rate</td><td><b>${m.gyro.map(v => fx(v, 2)).join(', ')} ${m.unit.g}</b></td></tr></table></div></div>` : h`<div class="hint">No magnetometer data received yet.</div>`}<//>
        <${Card} title="Limits"><${MCV.AlertList} /><//></div>`}
      ${tab === 'Trends' && h`<${MCV2.TrendsView} />`}
      ${tab === 'Beacons' && h`<${MCV.CwCard} />`}
      ${tab === 'ADCS' && h`<${Card} title="ADCS telemetry"><${MissionTelemetry} mission="ADCS" cmds=${['adcs', 'adcs_file']} file="adcs_file" /><//>`}
      ${tab === 'EPDM' && h`<${Card} title="EPDM telemetry"><${MissionTelemetry} mission="EPDM" cmds=${['epdm', 'epdm_file']} file="epdm_file" /><//>`}
      ${tab === 'Digipeater' && h`<${Card} title="Digipeater telemetry"><${DigiTelemetry} /><//>`}</div>`;
  }


  /* ------------------------------------------------------------------ settings: Google Drive, storage, link */
  function SettingsExtra({ me }) {
    const isAdmin = me.role === 'admin';
    const [g, , reloadG] = useFetch('/api/gdrive');
    const [home, , reloadH] = useFetch('/api/home');
    const [f, setF] = useState({ folder_id: '', client_id: '', client_secret: '', sa_json: '' }), [msg, setMsg] = useState(''), [sc, setSc] = useState(null);
    useEffect(() => { if (g) setF(x => ({ ...x, folder_id: g.folder_id || '', client_id: g.client_id || '' })); }, [g && g.folder_id, g && g.client_id]);
    useEffect(() => { if (home && !sc) setSc(home.store.cfg); }, [home]);
    const save = (extra) => api('/api/gdrive', { ...f, ...extra }).then(r => { setMsg('Saved.'); setF(x => ({ ...x, client_secret: '', sa_json: '' })); reloadG(); return r; }).catch(e => setMsg(e.message));
    const authorise = () => save({}).then(() => api('/api/gdrive/oauth')).then(r => r && r.url && window.open(r.url, '_blank')).catch(e => setMsg(e.message));
    const saveStore = () => api('/api/store/cfg', sc).then(() => { setMsg('Saved.'); reloadH(); }).catch(e => setMsg(e.message));
    return h`<${React.Fragment}>
      <${Card} title="Google Drive backup" help="Operational report folders are uploaded here automatically. Use OAuth with your own Google account (Desktop client) or a service account with a Shared Drive folder.">
        <div class="hint">${g ? (g.configured ? 'Connected' : g.authorised ? 'Authorised' : 'Not configured') : ''}${g && g.error ? ' \u00b7 ' + g.error : ''}</div>
        ${isAdmin ? h`<p class="hint">Day folders are uploaded into a Google Drive folder named <b>S2S_operational_reports</b>, which is created automatically.</p>
          <label class="field">Drive folder id (optional: use this folder instead, needed for a Shared Drive)<input value=${f.folder_id} onInput=${e => setF({ ...f, folder_id: e.target.value })} placeholder="leave empty for S2S_operational_reports" /></label>
          <label class="field">OAuth client id<input value=${f.client_id} onInput=${e => setF({ ...f, client_id: e.target.value })} /></label>
          <label class="field">OAuth client secret${g && g.has_secret ? ' (stored)' : ''}<input type="password" value=${f.client_secret} onInput=${e => setF({ ...f, client_secret: e.target.value })} /></label>
          <details><summary>Or use a service-account key (Shared Drive)</summary><textarea rows="4" class="mono wide" placeholder="paste the JSON key file" value=${f.sa_json} onInput=${e => setF({ ...f, sa_json: e.target.value })}></textarea></details>
          <div class="row"><button class="pri" onClick=${() => save({ enabled: true })}>Save and enable</button><button onClick=${authorise}>Authorise with Google</button>${g && g.enabled && h`<button class="danger" onClick=${() => save({ enabled: false })}>Disable</button>`}</div>
          <p class="hint">Create a Desktop OAuth client in the Google Cloud console, enter its id and secret, press \u201cAuthorise with Google\u201d and allow access. The redirect returns to this program.</p>` : h`<div class="hint">Only an administrator can change this.</div>`}
        ${msg && h`<div class="hint">${msg}</div>`}<//>
      <${Card} title="Data storage" help="One folder per day directly in GS_Data, named <YYYY-MM-DD, HH_MMAM>, in the operational report layout.">
        ${sc ? h`<div class="row"><label class="field">Keep on this computer (days)<input type="number" style="width:7rem" min="1" max="365" value=${sc.retention_days} disabled=${!isAdmin} onInput=${e => setSc({ ...sc, retention_days: +e.target.value })} /></label></div>
          <label class="chk"><input type="checkbox" checked=${!!sc.delete_requires_upload} disabled=${!isAdmin} onChange=${e => setSc({ ...sc, delete_requires_upload: e.target.checked })} /> Delete old folders only after they are on Google Drive</label>
          ${isAdmin && h`<button class="pri" onClick=${saveStore}>Save</button>`}<p class="hint">Folder: ${home ? home.store.root : ''}</p>` : h`<div class="hint">Loading.</div>`}<//>
      <${Card} title="Link supervision"><p class="hint">Heartbeat every 30 to 40 s and automatic sampling during an overpass are switched on the Satellite control page.</p><a class="btn" href="#dashboard">Open satellite control</a><//><//>`;
  }

  window.MCV2 = { FlagRow, SettingsExtra, LinkCard, OverpassAlert, HomePage, ReportPage, TelemetryPage };
})();
