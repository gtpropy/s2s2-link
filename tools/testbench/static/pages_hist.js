/* History and automation: Logs (search / export), Trends (reports over time, compare), Scenarios (your own test sequences), Alerts */
(function () {
  const { h, api, useStore, log, Chip, Verdict, Kv, useState, useEffect, useMemo, useRef, UI, startTest, RowsView } = S2;
  const { Spark } = UI;
  const ts = t => { const d = new Date(t * 1000); return d.toLocaleTimeString([], { hour12: false }) + '.' + String(d.getMilliseconds()).padStart(3, '0'); };
  const SRC_CLS = { GS: 'gs', VCP: 'sat', SDR: 'rf', BENCH: '' };

  /* ------------------------------------------------------------------ Logs */
  function Logs() {
    const [q, setQ] = useState(''), [regex, setRegex] = useState(false), [range, setRange] = useState(3600);
    const [srcs, setSrcs] = useState({ GS: true, VCP: true, SDR: true, BENCH: true });
    const [page, setPage] = useState(0), [live, setLive] = useState(true);
    const [res, setRes] = useState({ total: 0, rows: [], stats: null });
    const [err, setErr] = useState('');
    const PS = 200;
    const qs = useMemo(() => {
      const p = new URLSearchParams();
      if (q) p.set('q', q);
      if (regex) p.set('regex', '1');
      const on = Object.keys(srcs).filter(k => srcs[k]);
      if (on.length < 4) p.set('src', on.join(','));
      if (range) p.set('t0', String(Date.now() / 1000 - range));
      return p.toString();
    }, [q, regex, srcs, range]);
    const load = () => api(`/api/logs?${qs}&limit=${PS}&offset=${page * PS}`).then(r => { setRes(r); setErr(''); }).catch(e => setErr(e.message));
    useEffect(() => { setPage(0); }, [qs]);
    useEffect(() => { const t = setTimeout(load, 200); const iv = live ? setInterval(load, 2000) : null; return () => { clearTimeout(t); iv && clearInterval(iv); }; }, [qs, page, live]);
    const hl = m => { if (!q || regex) return m; const i = m.toLowerCase().indexOf(q.toLowerCase()); return i < 0 ? m : h`<span>${m.slice(0, i)}<mark>${m.slice(i, i + q.length)}</mark>${m.slice(i + q.length)}</span>`; };
    return h`<div class="card"><h2>Logs <small class="hint">every GS / VCP / SDR / bench line since the first run, ${res.stats ? res.stats.lines.toLocaleString() + ' lines stored' : ''}</small></h2>
      <div class="row"><input class="grow mono" placeholder=${regex ? 'regular expression' : 'search text'} value=${q} onInput=${e => setQ(e.target.value)} />
        <label><input type="checkbox" checked=${regex} onChange=${e => setRegex(e.target.checked)} /> regex</label>
        ${Object.keys(srcs).map(k => h`<label key=${k}><input type="checkbox" checked=${srcs[k]} onChange=${e => setSrcs({ ...srcs, [k]: e.target.checked })} /> ${k}</label>`)}
        <select value=${range} onChange=${e => setRange(+e.target.value)}><option value="300">last 5 min</option><option value="3600">last hour</option><option value="86400">last 24 h</option><option value="604800">last 7 days</option><option value="0">everything</option></select>
        <label><input type="checkbox" checked=${live} onChange=${e => setLive(e.target.checked)} /> live</label>
        <a class="btn" href=${'/api/logs/export?' + qs}>Export CSV</a></div>
      ${err && h`<div style="color:var(--bad)">${err}</div>`}
      <div class="hint" style="margin:6px 0">${res.total.toLocaleString()} matching lines · page ${page + 1} of ${Math.max(1, Math.ceil(res.total / PS))} (newest first)
        <button class="sm" disabled=${page === 0} onClick=${() => setPage(page - 1)}>‹</button><button class="sm" disabled=${(page + 1) * PS >= res.total} onClick=${() => setPage(page + 1)}>›</button></div>
      <div class="term" style="height:560px">${res.rows.map((r, i) => h`<div key=${i} class=${SRC_CLS[r.src]}><span class="t">${new Date(r.t * 1000).toLocaleDateString([], { month: '2-digit', day: '2-digit' })} ${ts(r.t)}</span><b style="display:inline-block;width:52px">${r.src}</b>${r.level && r.level !== 'info' ? h`<span class=${r.level}>[${r.level}] </span>` : ''}${hl(r.msg)}</div>`)}</div></div>`;
  }

  /* ------------------------------------------------------------------ Trends */
  const VC = { PASS: 'var(--ok)', FAIL: 'var(--bad)', INFO: 'var(--warn)', PARTIAL: 'var(--warn)' };
  function Trends() {
    const [d, setD] = useState(null), [err, setErr] = useState('');
    const [test, setTest] = useState(''), [metric, setMetric] = useState('');
    const [a, setA] = useState(''), [b, setB] = useState(''), [df, setDf] = useState(null);
    useEffect(() => { api('/api/trends').then(r => { setD(r); const names = Object.keys(r.tests); setTest(names[0] || ''); if (r.reports.length > 1) { setA(r.reports[r.reports.length - 2].dir); setB(r.reports[r.reports.length - 1].dir); } }).catch(e => setErr(e.message)); }, []);
    useEffect(() => { if (a && b) api(`/api/trends/diff?a=${a}&b=${b}`).then(setDf).catch(() => setDf(null)); }, [a, b]);
    if (!d) return h`<div class="card">${err || 'loading …'}</div>`;
    const names = Object.keys(d.tests);
    const pts = d.tests[test] || [];
    const metrics = [...new Set(pts.flatMap(p => Object.keys(p.metrics)))];
    const m = metric && metrics.includes(metric) ? metric : metrics[0];
    return h`<div>
      <div class="card"><h2>Verdict history <small class="hint">${d.reports.length} saved reports in docs/reports/; one square per report that ran the test</small></h2>
        ${!names.length && h`<div class="hint">No saved report has a test result yet: run tests and press Generate report.</div>`}
        <table>${names.map(n => h`<tr key=${n} style="cursor:pointer" onClick=${() => setTest(n)}><td style="width:130px"><b style=${n === test ? 'color:var(--acc)' : ''}>${n}</b></td>
          <td>${d.tests[n].map((p, i) => h`<i key=${i} title=${p.report + ': ' + p.verdict} style=${`display:inline-block;width:14px;height:14px;margin:1px;border-radius:3px;background:${VC[p.verdict] || 'var(--line)'}`}></i>`)}</td>
          <td class="hint" style="width:80px">${d.tests[n].filter(p => p.verdict === 'PASS').length} / ${d.tests[n].length} pass</td></tr>`)}</table></div>
      ${test && h`<div class="card"><h2>${test}: numbers over the reports</h2>
        <div class="row"><select value=${m || ''} onChange=${e => setMetric(e.target.value)}>${metrics.map(x => h`<option key=${x}>${x}</option>`)}</select><span class="hint">${m ? pts.filter(p => p.metrics[m] != null).length + ' values' : 'this test has no numbers'}</span></div>
        ${m && h`<${Spark} data=${pts.map(p => p.metrics[m]).filter(v => v != null)} />
          <table><tr><th>report</th><th>verdict</th><th>${m}</th></tr>${pts.slice(-12).reverse().map((p, i) => h`<tr key=${i}><td class="mono">${p.report}</td><td><${Verdict} v=${p.verdict === 'INFO' ? 'PARTIAL' : p.verdict} /></td><td>${p.metrics[m] ?? ''}</td></tr>`)}</table>`}</div>`}
      <div class="card"><h2>Compare two reports</h2>
        <div class="row"><select value=${a} onChange=${e => setA(e.target.value)}>${d.reports.map(r => h`<option key=${r.dir} value=${r.dir}>${r.dir} ${r.note}</option>`)}</select><span>→</span>
          <select value=${b} onChange=${e => setB(e.target.value)}>${d.reports.map(r => h`<option key=${r.dir} value=${r.dir}>${r.dir} ${r.note}</option>`)}</select></div>
        ${df && h`<table style="margin-top:8px"><tr><th>test</th><th>${df.a}</th><th>${df.b}</th><th>change</th><th>numbers that moved</th></tr>
          ${df.diff.map(x => h`<tr key=${x.test}><td><b>${x.test}</b></td><td>${x.a || '–'}</td><td>${x.b || '–'}</td><td><${Verdict} v=${x.status === 'regressed' ? 'FAIL' : x.status === 'fixed' ? 'PASS' : x.status === 'same' ? '' : 'PARTIAL'} /> ${x.status}</td>
            <td class="mono hint">${x.moved.slice(0, 6).map(y => `${y.name}: ${y.a ?? '–'} → ${y.b ?? '–'}${y.delta != null ? ' (' + (y.delta > 0 ? '+' : '') + Math.round(y.delta * 100) / 100 + ')' : ''}`).join(' · ')}</td></tr>`)}</table>`}</div></div>`;
  }

  /* ------------------------------------------------------------------ Scenarios */
  const EXAMPLE = { title: 'my sequence', steps: [{ do: 'obc', cmd: 'STATUS', expect_status: 0, name: 'CPU1 is up' }, { do: 'gs', send: 'HK1' }, { do: 'expect', event: 'ack', what: 'accepted', within_s: 90, resend: 'HK1', every_s: 14, tries: 6 }, { do: 'test', name: 'membudget', cfg: { target: 'sat' } }, { do: 'screenshot', route: 'dashboard' }, { do: 'report', note: 'my sequence' }] };
  function Scenarios() {
    const s = useStore();
    const [list, setList] = useState([]), [sel, setSel] = useState(''), [txt, setTxt] = useState(JSON.stringify(EXAMPLE, null, 1)), [name, setName] = useState('my_sequence');
    const [msg, setMsg] = useState('');
    const load = () => api('/api/scenarios').then(setList).catch(e => setMsg(e.message));
    useEffect(() => { load(); }, []);
    const pick = x => { setSel(x.name); setName(x.builtin ? x.name + '_copy' : x.name); setTxt(JSON.stringify(x.scenario, null, 1)); setMsg(''); };
    const parse = () => { try { return JSON.parse(txt); } catch (e) { setMsg('not valid JSON: ' + e.message); return null; } };
    const validate = () => { const sc = parse(); if (sc) api('/api/scenario/validate', { scenario: sc }).then(r => setMsg(r.errors.length ? r.errors.join(' · ') : 'valid')).catch(e => setMsg(e.message)); };
    const save = () => { const sc = parse(); if (sc) api('/api/scenario/save', { name, scenario: sc }).then(() => { setMsg('saved as ' + name); load(); setSel(name); }).catch(e => setMsg(e.message)); };
    const run = () => startTest('scenario', { scenario: sel, fresh: false });
    const r = s.results.scenario, pg = s.progress.scenario, cur = list.find(x => x.name === sel);
    return h`<div>
      <div class="cards" style="grid-template-columns:minmax(260px,1fr) minmax(420px,2fr)">
        <div class="card"><h2>Scenarios</h2>${list.map(x => h`<div key=${x.name} class="row" style="padding:5px 0;border-top:1px solid var(--line);cursor:pointer" onClick=${() => pick(x)}>
          <b class="grow" style=${x.name === sel ? 'color:var(--acc)' : ''}>${x.name}</b><span class="hint">${x.builtin ? 'built in' : 'yours'} · ${x.steps} steps</span><div class="hint" style="width:100%">${x.title}</div></div>`)}
          <p class="hint">Click one to load it into the editor. Built-in ones can be copied under a new name. Run needs a saved scenario.</p>
          <div class="row"><button class="pri" disabled=${!sel || !!s.running} onClick=${run}>Run ${sel || ''}</button>${s.running === 'scenario' && h`<button onClick=${() => api('/api/test/stop', {})}>Stop</button>`}
            ${cur && !cur.builtin && h`<button class="danger" onClick=${() => confirm('Delete ' + sel + '?') && api('/api/scenario/delete', { name: sel }).then(() => { setSel(''); load(); })}>Delete</button>`}</div></div>
        <div class="card"><h2>Editor</h2><textarea class="mono" style="width:100%;height:340px;background:var(--bg);color:var(--fg);border:1px solid var(--line);border-radius:8px;padding:8px" value=${txt} onInput=${e => setTxt(e.target.value)} spellcheck="false"></textarea>
          <div class="row" style="margin-top:6px"><input value=${name} onInput=${e => setName(e.target.value)} style="width:200px" /><button onClick=${validate}>Validate</button><button class="pri" onClick=${save}>Save</button><span class="hint">${msg}</span></div>
          <details style="margin-top:8px"><summary class="hint">step types</summary><pre class="mono">${'gs  {"do":"gs","send":"HK1"}\nobc {"do":"obc","cmd":"STATUS","args":{},"expect_status":0}\nexpect {"do":"expect","event":"ack","what":"accepted","within_s":90,"resend":"HK1","every_s":14,"tries":8}\n  events: ack nack hk beacon cw rssi\nwait {"do":"wait","s":5}\ntest {"do":"test","name":"membudget","cfg":{"target":"sat"}}   (any test of the Tests page)\nscreenshot {"do":"screenshot","route":"dashboard","note":"..."}\nreport {"do":"report","note":"..."}\nlog {"do":"log","msg":"..."}\nevery step: "name": label, "continue": true = go on after a failure'}</pre></details></div></div>
      ${(r || (s.running === 'scenario')) && h`<div class="card"><h2>Last run ${r && h`<${Verdict} v=${r.verdict === 'INFO' ? 'PARTIAL' : r.verdict} />`} <small class="hint">${r ? r.scenario + ' — ' + r.title : ''}</small></h2>
        ${s.running === 'scenario' && pg && h`<div class="bar" style="margin:6px 0"><i style=${'width:' + (100 * pg.done / pg.total) + '%'}></i></div>`}
        <${RowsView} r=${r} /></div>`}</div>`;
  }

  /* ------------------------------------------------------------------ Alerts */
  function Alerts() {
    const s = useStore();
    const [set, setSet] = useState(null);
    useEffect(() => { api('/api/alerts').then(r => { setSet(r.settings); state.alerts = r.alerts; S2.emit(); }); }, []);
    const state = S2.state;
    const save = c => api('/api/watch', c).then(setSet).catch(e => log(e.message, 'err'));
    if (!set) return null;
    const open = s.alerts.filter(a => !a.acked);
    return h`<div class="cards" style="grid-template-columns:minmax(300px,1fr) minmax(380px,2fr)">
      <div class="card"><h2>Watchdog</h2>
        <label style="display:block;margin:6px 0"><input type="checkbox" checked=${set.enabled} onChange=${e => save({ enabled: e.target.checked })} /> <b>monitors on</b> <span class="hint">(they read the UART and poll the Nucleo: off until you switch them on)</span></label>
        <label style="display:block;margin:6px 0"><input type="checkbox" checked=${set.alert_on_fail} onChange=${e => save({ alert_on_fail: e.target.checked })} /> alert when a test ends FAIL</label>
        <label style="display:block;margin:6px 0"><input type="checkbox" checked=${set.vcp_poll} onChange=${e => save({ vcp_poll: e.target.checked })} /> poll the Nucleo VCP (STATUS) every <input type="number" style="width:70px" value=${set.vcp_every_s} onChange=${e => save({ vcp_every_s: +e.target.value })} /> s</label>
        <label style="display:block;margin:6px 0">GS UART silent for more than <input type="number" style="width:80px" value=${set.gs_silence_s} onChange=${e => save({ gs_silence_s: +e.target.value })} /> s</label>
        <label style="display:block;margin:6px 0"><input type="checkbox" checked=${set.notify} onChange=${e => save({ notify: e.target.checked })} /> macOS notification</label></div>
      <div class="card"><h2>Alerts (${open.length} open)</h2><div class="row"><button disabled=${!open.length} onClick=${() => api('/api/alerts/ack', {})}>Acknowledge all</button></div>
        <table>${s.alerts.slice(0, 60).map(a => h`<tr key=${a.id} style=${a.acked ? 'opacity:.5' : ''}><td style="width:90px">${ts(a.t)}</td><td><${Verdict} v=${a.level === 'bad' ? 'FAIL' : 'PARTIAL'} /></td><td>${a.text}</td>
          <td>${!a.acked && h`<button class="sm" onClick=${() => api('/api/alerts/ack', { id: a.id })}>ack</button>`}</td></tr>`)}</table>
        ${!s.alerts.length && h`<div class="hint">no alerts</div>`}</div></div>`;
  }

  Object.assign(S2, { Logs, Trends, Scenarios, Alerts });
})();
