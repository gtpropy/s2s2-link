/* System pages: configuration (same file as the menuconfig), build / flash, log and reports */
(function () {
  const { h, api, useStore, act, log, Chip, Verdict, Kv, Terminal, fmtT, useState, useEffect } = S2;

  function Config() {
    const [data, setData] = useState(null);
    const [edit, setEdit] = useState({});
    const [msg, setMsg] = useState('');
    const st = useStore().status;
    const load = () => api('/api/config').then(d => { setData(d); setEdit({}); }).catch(e => setMsg(e.message));
    useEffect(() => { load(); }, []);
    if (!data) return h`<div class="card">${msg || 'loading …'}</div>`;
    const val = k => (k in edit ? edit[k] : data.config[k]);
    const set = (k, v) => setEdit({ ...edit, [k]: v });
    const save = () => api('/api/config', { changes: edit }).then(d => { setData(d); setEdit({}); setMsg('saved and generated — rebuild and reflash to apply'); }).catch(e => setMsg(e.message));
    const field = it => {
      const v = val(it.key);
      if (it.kind === 'bool') return h`<input type="checkbox" checked=${!!v} onChange=${e => set(it.key, e.target.checked)} />`;
      if (it.kind === 'choice') return h`<select value=${v} onChange=${e => set(it.key, e.target.value)}>${it.options.map(o => h`<option key=${o}>${o}</option>`)}</select>`;
      if (it.kind === 'int') return h`<input type="number" min=${it.lo} max=${it.hi} style="width:150px" value=${v} onInput=${e => set(it.key, parseInt(e.target.value, 10))} />`;
      if (it.kind === 'probe') return h`<select value=${v || ''} onChange=${e => set(it.key, e.target.value)}><option value="">(none)</option>${[...new Set([...(st ? st.probes : []), v].filter(Boolean))].map(o => h`<option key=${o}>${o}</option>`)}</select>`;
      return h`<input class="mono" style="width:150px" value=${v} onInput=${e => set(it.key, it.upper ? e.target.value.toUpperCase() : e.target.value)} />`;
    };
    const visible = it => (it.dep || []).every(k => !!val(k));
    const n = Object.keys(edit).length;
    return h`<div>
      <div class="card"><div class="row"><h2 class="grow" style="margin:0">Configuration <small class="hint">same file as <code>make</code> (menuconfig): .s2s2.config → generated/</small></h2>
        ${n ? h`<${Chip} cls="warn">${n} unsaved<//>` : null}<button class="pri" disabled=${!n} onClick=${save}>Save and generate</button><button disabled=${!n} onClick=${() => setEdit({})}>Revert</button></div>
        ${msg && h`<p class="hint">${msg}</p>`}
        <p class="hint">AUTH key fingerprint: <b>${data.key_fingerprint || 'none yet'}</b> · generated files: ${Object.entries(data.files).map(([k, v]) => k + (v ? ' ✔' : ' ✘')).join(', ')}.
          Both boards must be rebuilt and reflashed after a change (Build page).</p></div>
      <div class="cards">${Object.entries(data.menus).map(([k, m]) => h`<div class="card" key=${k}><h2>${m.title}</h2>
        ${m.items.filter(visible).map(it => h`<div key=${it.key} style="padding:5px 0;border-top:1px solid var(--line)"><div class="row"><label style="min-width:260px;flex:1">${it.label}</label>${field(it)}</div><div class="hint">${it.help}</div></div>`)}</div>`)}</div>
    </div>`;
  }

  function Build() {
    const s = useStore();
    const st = s.status;
    const [probes, setProbes] = useState({ sat_probe: '', gs_probe: '' });
    const [cb, setCb] = useState(null);                       // null = the saved option of .s2s2.config
    if (!st) return null;
    const cubus = cb == null ? !!st.cubus : cb;
    const bar = b => (b ? h`<div class="bar"><i style=${'width:' + Math.min(100, 100 * b.bin / b.limit) + '%'}></i></div><span class="hint">${b.bin.toLocaleString()} B of ${b.limit.toLocaleString()} (${(100 * b.bin / b.limit).toFixed(1)} %), built ${fmtT(b.mtime)}</span>` : h`<span class="hint">not built</span>`);
    const p = (k, def) => probes[k] || def;
    return h`<div class="cards">
      <div class="card"><h2>Satellite (CPU2)</h2>${bar(st.sat_build)}
        <div class="row" style="margin-top:8px"><label title="make CUBUS=T: the CUBUS board profile (docs/CUBUS.md). Saved default comes from .s2s2.config."><input type="checkbox" checked=${cubus} onChange=${e => setCb(e.target.checked)} /> CUBUS board (RSMS CUBUS V.3)</label>
          ${cb != null && cb !== !!st.cubus && h`<${Chip} cls="warn">differs from .s2s2.config<//>`}</div>
        <div class="row" style="margin-top:8px"><button onClick=${() => act('/api/build', { target: 'sat', cubus }, 'building satellite' + (cubus ? ' (CUBUS)' : '') + ' …')}>Build</button>
          <button class="danger" onClick=${() => confirm(cubus ? 'Flash the satellite image built for the CUBUS flight board? Check that the probe is the CUBUS board\'s ST-Link.' : 'Flash the satellite board? (This is the NON-CUBUS image: do not flash it onto the CUBUS board.)') && act('/api/flash', { target: 'sat', cubus, sat_probe: p('sat_probe', ''), gs_probe: p('gs_probe', '') }, 'flashing satellite …')}>Flash</button></div>
        <p class="hint">${cubus ? 'CUBUS: no LEDs on PB15/PB9/PB11, no PB6 UART, PA0 / PC2..PC5 only. After flashing, the DEBUG page reads g_boardId to confirm it.' : 'Nucleo / custom front end image.'}</p></div>
      <div class="card"><h2>Ground station (CPU2)</h2>${bar(st.gs_build)}
        <div class="row" style="margin-top:8px"><button onClick=${() => act('/api/build', { target: 'gs' }, 'building ground station …')}>Build</button>
          <button class="danger" onClick=${() => confirm('Flash the ground station board?') && act('/api/flash', { target: 'gs', sat_probe: p('sat_probe', ''), gs_probe: p('gs_probe', '') }, 'flashing GS …')}>Flash</button></div></div>
      <div class="card"><h2>Probes</h2>
        <div class="row"><label>satellite</label><input class="mono grow" placeholder="ST-Link serial" value=${probes.sat_probe} onInput=${e => setProbes({ ...probes, sat_probe: e.target.value })} /></div>
        <div class="row" style="margin-top:6px"><label>ground station</label><input class="mono grow" placeholder="ST-Link serial" value=${probes.gs_probe} onInput=${e => setProbes({ ...probes, gs_probe: e.target.value })} /></div>
        <p class="hint">Seen: ${st.probes.length ? st.probes.join(', ') : 'press Detect'} <button class="sm" onClick=${() => S2.refresh(true)}>Detect</button></p>
        <p class="hint">Blank = the defaults in the server. Build output and flash log appear in the Log page. Stop the console's serial ports are not needed for flashing, but the ST-Link must be free.</p></div>
      <div class="card"><h2>Satellite fresh start</h2><p class="hint">Resets the replay floor on CPU2 (RESET frame) and waits for the heartbeat.</p>
        <button onClick=${() => act('/api/sat/reset', {}, 'satellite reset sent')}>Reset replay floor</button></div>
      <div class="card"><h2>Source</h2><${Kv} rows=${[['git', st.git]]} /></div>
    </div>`;
  }

  const ROUTES = ['dashboard', 'consoles', 'profiler', 'alerts', 'commands', 'digipeater', 'images', 'radio', 'tests', 'scenarios', 'trends', 'logs', 'reports', 'config', 'build', 'cubusdebug', 'cubuspins'];
  function LogPage() {
    const s = useStore();
    const [tab, setTab] = useState('bench');
    const [note, setNote] = useState('');
    const [route, setRoute] = useState(location.hash.slice(1) || 'dashboard');
    const [busy, setBusy] = useState('');
    const [shots, setShots] = useState([]);
    const [reps, setReps] = useState([]);
    const [pdf, setPdf] = useState(true);
    const [big, setBig] = useState(null);
    const load = () => { api('/api/shots').then(setShots).catch(() => {}); api('/api/reports').then(setReps).catch(() => {}); };
    useEffect(load, []);
    const shot = () => { setBusy('screenshot (a few seconds) …'); api('/api/shot', { route, note }).then(() => { setBusy(''); load(); }).catch(e => { setBusy(e.message); }); };
    const report = () => { setBusy('building report: files, screenshots, PDF, ZIP …'); api('/api/report', { note, pdf }).then(j => { setBusy('saved ' + j.dir); load(); }).catch(e => setBusy(e.message)); };
    return h`<div>
      <div class="card"><h2>Screenshots and reports</h2>
        <div class="row"><select value=${route} onChange=${e => setRoute(e.target.value)}>${ROUTES.map(r => h`<option key=${r}>${r}</option>`)}</select>
          <input class="grow" placeholder="note (setup, antenna, attenuators, what you see …)" value=${note} onInput=${e => setNote(e.target.value)} />
          <button onClick=${shot}>Screenshot of this page</button>
          <label><input type="checkbox" checked=${pdf} onChange=${e => setPdf(e.target.checked)} /> PDF</label><button class="pri" onClick=${report}>Generate report (HTML + ZIP${pdf ? ' + PDF' : ''})</button></div>
        <div class="hint">${busy || 'A report bundles every test result, the last housekeeping packet, the profiler snapshot, pictures, raw UART logs and all screenshots taken since the testbench started, as report.html / .pdf / .zip in docs/reports/.'}</div></div>
      <div class="cards" style="grid-template-columns:repeat(auto-fit,minmax(420px,1fr))">
        <div class="card"><h2>Screenshots (${shots.length})</h2><div class="gallery">${shots.map(x => h`<figure key=${x.file} onClick=${() => setBig(x.file)}><img src=${'/shots/' + x.file} loading="lazy" /><figcaption>${x.route} · ${new Date(x.t * 1000).toLocaleTimeString()}<br />${x.note}</figcaption></figure>`)}</div>
          ${big && h`<div style="margin-top:8px"><img src=${'/shots/' + big} style="max-width:100%;border:1px solid var(--line)" /><div class="row"><a class="btn" href=${'/shots/' + big} download=${big}>Download</a><button class="danger" onClick=${() => api('/api/shots/delete', { file: big }).then(() => { setBig(null); load(); })}>Delete</button></div></div>`}</div>
        <div class="card"><h2>Reports (${reps.length})</h2><table><tr><th>when</th><th>verdicts</th><th>files</th></tr>${reps.map(r => h`<tr key=${r.dir}><td>${r.dir}<div class="hint">${r.note}</div></td>
          <td>${Object.entries(r.verdicts).map(([k, v]) => h`<div key=${k}>${k} <${Verdict} v=${v} /></div>`)}</td>
          <td><a href=${`/reports/${r.dir}/report.html`} target="_blank">HTML</a>${r.pdf ? h` · <a href=${`/reports/${r.dir}/report.pdf`} target="_blank">PDF</a>` : ''}${r.zip ? h` · <a href=${`/reports/${r.dir}/report.zip`}>ZIP</a>` : ''} · <a href=${`/reports/${r.dir}/report.json`} target="_blank">JSON</a><div class="hint">${r.shots} screenshot(s)</div></td></tr>`)}</table></div></div>
      <div class="card"><h2>Logs</h2>
        <div class="tabs">${[['bench', 'Testbench'], ['gs', 'GS UART'], ['sat', 'Nucleo VCP'], ['sdr', 'Direwolf']].map(([k, t]) => h`<button key=${k} class=${tab === k ? 'on' : ''} onClick=${() => setTab(k)}>${t}</button>`)}</div>
        <${Terminal} lines=${s[tab]} height=${360} /></div></div>`;
  }

  Object.assign(S2, { Config, Build, LogPage });
})();
