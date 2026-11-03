/* Digipeater, Images (gallery + background builder + source picker), Radio (monitor + decoded events) */
(function () {
  const { h, api, useStore, act, log, Chip, Verdict, Kv, Terminal, fmtT, fmtBytes, useState, useEffect, useMemo, state, describe } = S2;
  const startTest = (name, cfg) => api('/api/test/start', { name, ...cfg }).catch(e => log(e.message, 'err'));

  /* ------------------------------------------------------------------ Digipeater */
  function Digipeater() {
    const s = useStore();
    const [txt, setTxt] = useState('K1ABC>APZS2S,APNDIG:>hello store and forward');
    const [store, setStore] = useState(null);
    const [msg, setMsg] = useState('');
    const [sdr, setSdr] = useState(false);
    const r = s.results.digi;
    const gsAir = () => api('/api/gs/send', { text: 'DIGI ' + txt }).then(() => setMsg('sent through the ground station, now watch the timeline')).catch(e => setMsg(e.message));
    const inject = () => { setMsg('injecting … (the satellite answers between beacons, up to ~75 s)'); api('/api/digi/inject', { text: txt }).then(j => { setMsg(j.result); return show(); }).catch(e => setMsg(e.message)); };
    const show = () => api('/api/digi/store', {}).then(setStore).catch(e => setMsg(e.message));
    const clear = () => confirm('Empty the journal in CPU1 flash?') && api('/api/digi/clear', {}).then(j => setMsg(j.note)).catch(e => setMsg(e.message));
    const table = o => h`<table><tr><th>MEMORY OBJECT</th><th>id</th><th>len</th><th>hops left</th><th>sent</th><th>time left</th><th>frame</th></tr>
      ${o.map(x => h`<tr key=${x.n}><td>#${x.n}</td><td>${x.id}</td><td>${x.len} B</td><td>${x.hops_left}</td><td>${x.fwd_count}</td><td>${x.ttl_s} s</td><td class="mono">${x.text}</td></tr>`)}</table>`;
    return h`<div>
      <div class="card"><h2>Send a frame</h2>
        <div class="row"><input class="grow mono" value=${txt} onInput=${e => setTxt(e.target.value)} />
          <button class="pri" onClick=${gsAir}>Ground station → air → satellite</button><button onClick=${inject}>Inject through the Nucleo</button>
          <button onClick=${show}>Show stored</button><button class="danger" onClick=${clear}>Clear journal</button></div>
        <p class="hint">Format SRC>DEST,VIA1,VIA2:text. Via APNDIG or WIDE2-n is repeated (n counts down, the hop budget); another station's call sign is ignored. Rate limit: 6 repeats a minute.</p>
        ${msg && h`<p><b>${msg}</b></p>`}
        ${store && h`<p class="hint">${store.journal} · <b>stored ${store.stored} msg, ${store.bytes} B</b></p>${store.objects.length ? table(store.objects) : null}`}
      </div>
      <div class="card"><h2>8-case decision test <${Verdict} v=${r && r.verdict} /></h2>
        <div class="row"><label><input type="checkbox" checked=${sdr} onChange=${e => setSdr(e.target.checked)} /> also check the air with the SDR + Direwolf</label>
          <button class="pri" disabled=${!!s.running} onClick=${() => startTest('digi', { sdr, fresh: false })}>Run</button>
          <span class="hint">needs only the Nucleo VCP; run at most once a minute (rate limiter)</span></div>
        ${r && r.rows && h`<table><tr><th>#</th><th>case</th><th>frame</th><th>expected</th><th>satellite said</th><th></th><th>air</th></tr>
          ${r.rows.map(x => h`<tr key=${x.n}><td>${x.n}</td><td>${x.name}</td><td class="mono">${x.frame}</td><td>${x.expected}</td><td>${x.got}</td><td><${Verdict} v=${x.ok ? 'PASS' : 'FAIL'} /></td><td>${x.air ? (x.seen ? 'seen' : 'not seen') : ''}</td></tr>`)}</table>
          <p class="hint">${r.journal} · <b>stored ${r.stored} msg, ${r.stored_bytes} B</b></p>${r.objects && r.objects.length ? table(r.objects) : null}`}
      </div>
    </div>`;
  }

  /* ------------------------------------------------------------------ Images */
  function FragMap({ im }) {
    const miss = new Set(im.missing || []);
    return h`<div class="grid">${Array.from({ length: im.total }, (_, i) => h`<i key=${i} class=${miss.has(i + 1) && im.missing.length < 200 ? '' : (im.complete || !miss.has(i + 1) ? 'on' : '')} title=${'fragment ' + (i + 1)}></i>`)}</div>`;
  }

  function ImageSource({ send = true }) {
    const s = useStore();
    const st = s.status;
    const [samples, setSamples] = useState([]);
    const [fit, setFit] = useState('flash');
    const [via, setVia] = useState('image');
    useEffect(() => { api('/api/samples').then(setSamples).catch(() => {}); }, []);
    const info = st && st.image;
    const pick = p => api('/api/image/select', { path: p, fit }).then(() => S2.refresh(false)).catch(e => log(e.message, 'err'));
    const upload = async ev => {
      const f = ev.target.files[0]; if (!f) return;
      const r = await fetch('/api/image/upload?name=' + encodeURIComponent(f.name) + '&fit=' + fit, { method: 'POST', body: await f.arrayBuffer() });
      if (!r.ok) log((await r.json()).error, 'err'); ev.target.value = ''; S2.refresh(false);
    };
    return h`<div class="card"><h2>Picture to send <small class="hint">PC → Nucleo flash (LittleFS) → CPU2 → RF → receiver</small></h2>
      <div class="row"><select onChange=${e => pick(e.target.value)} value=""><option value="">choose a sample …</option>${samples.map(x => h`<option key=${x}>${x}</option>`)}</select>
        <label class="btn" style="cursor:pointer">Upload …<input type="file" hidden onChange=${upload} /></label>
        <select value=${fit} onChange=${e => { setFit(e.target.value); api('/api/image/refit', { fit: e.target.value }).then(() => S2.refresh(false)).catch(() => {}); }}>
          <option value="none">keep file as it is</option><option value="flash">compress to fit CPU1 flash (≤ 44 KB)</option><option value="quick">quick demo (≤ 10 KB)</option></select>
      </div>
      ${info && info.bytes ? h`<div class="row" style="margin-top:8px"><img src=${'/api/image/source?' + info.sha256.slice(0, 8)} style="max-height:90px;border-radius:6px" />
        <${Kv} rows=${[['file', info.name], ['size', `${info.bytes.toLocaleString()} B in ${info.chunks} chunks`], ['info', info.info], ['sha256', h`<span class="mono">${info.sha256.slice(0, 24)}…</span>`]]} /></div>
        ${send && h`<div class="row" style="margin-top:8px">
          <select value=${via} onChange=${e => setVia(e.target.value)}><option value="image">receive on the ground station (UART)</option><option value="sdrimage">receive with SDR + Direwolf</option><option value="imageloop">transmit only (no receiver)</option></select>
          <button class="pri" disabled=${!!s.running} onClick=${() => startTest(via, { mode: 'store', fresh: false })}>Send picture</button>
          <span class="hint">Whatever the receivers hear is stored and rebuilt below as it arrives — no test needs to be running.</span></div>`}`
        : h`<p class="hint">no picture chosen</p>`}
    </div>`;
  }

  function Images() {
    const s = useStore();
    const [sel, setSel] = useState(null);
    const list = s.images;
    const cur = list.find(x => x.id === sel) || list[0];
    const prog = s.chunk;
    return h`<div>
      <${ImageSource} />
      <div class="cards" style="grid-template-columns:minmax(300px,1fr) minmax(300px,2fr)">
        <div class="card"><h2>Stored pictures (${list.length})</h2>
          <div class="gallery">${list.map(x => h`<figure key=${x.id} class=${cur && cur.id === x.id ? 'sel' : ''} onClick=${() => setSel(x.id)}>
            <img src=${`/api/images/${x.id}/image?v=${x.got}`} loading="lazy" />
            <figcaption>${x.id}<br />${x.got}/${x.total} ${x.complete ? '✓' : '…'} ${x.matches_sent === true ? ' = sent' : ''}</figcaption></figure>`)}</div>
          ${!list.length && h`<p class="hint">Empty. Start the SDR monitor or connect the ground station; the first camera fragment creates a session.</p>`}
        </div>
        <div class="card">${cur ? h`<h2>${cur.id} ${cur.complete ? h`<${Verdict} v=${cur.matches_sent === false ? 'FAIL' : 'PASS'} />` : h`<${Verdict} v="running" />`}</h2>
          <div class="big"><img src=${`/api/images/${cur.id}/image?v=${cur.got}-${cur.complete}`} /></div>
          <div class="bar" style="margin:8px 0"><i style=${'width:' + (100 * cur.got / cur.total) + '%'}></i></div>
          <${FragMap} im=${cur} />
          <${Kv} rows=${[['fragments', `${cur.got} / ${cur.total}` + (cur.missing.length ? ` · missing ${cur.missing.slice(0, 24).join(', ')}${cur.missing.length > 24 ? ' …' : ''}` : '')],
            ['size', fmtBytes(cur.size)], ['type', cur.kind], ['sources', Object.entries(cur.sources).map(([k, v]) => `${k}: ${v}`).join(', ') || '–'], ['duplicates', cur.dups],
            ['duration', cur.seconds + ' s'], cur.sha256 && ['sha256', h`<span class="mono">${cur.sha256}</span>`],
            cur.matches_sent != null && ['vs. file sent', cur.matches_sent ? h`<${Verdict} v="PASS" /> identical` : h`<${Verdict} v="FAIL" /> different`]]} />
          <div class="row" style="margin-top:8px"><a class="btn" href=${`/api/images/${cur.id}/image?download=1`}>Download</a>
            <button class="danger" onClick=${() => confirm('Delete this session?') && act('/api/images/delete', { id: cur.id }).then(() => setSel(null))}>Delete</button></div>`
          : h`<p class="hint">nothing selected</p>`}</div>
      </div>
    </div>`;
  }

  /* ------------------------------------------------------------------ Radio */
  function Radio() {
    const s = useStore();
    const mon = s.monitor || {};
    const [f, setF] = useState(mon.freq || 434.9936), [g, setG] = useState(mon.gain || 40);
    const st = s.status;
    const frames = s.evt.filter(e => e.kind === 'frame' || e.kind === 'rssi');
    const cw = s.evt.filter(e => e.kind === 'cw' || e.kind === 'beacon');
    return h`<div>
      <div class="card"><h2>Radio monitor (RTL-SDR + Direwolf) <${Chip} cls=${mon.on ? 'ok' : 'warn'}>${mon.on ? 'decoding' : 'off'}<//></h2>
        <div class="row"><label>MHz</label><input type="number" step="0.0001" style="width:110px" value=${f} onInput=${e => setF(+e.target.value)} />
          <label>gain</label><input type="number" style="width:70px" value=${g} onInput=${e => setG(+e.target.value)} />
          <button class=${mon.on ? '' : 'pri'} onClick=${() => act('/api/monitor', { on: !mon.on, freq: f, gain: g }, mon.on ? 'monitor off' : 'monitor on')}>${mon.on ? 'Stop' : 'Start'}</button>
          <button onClick=${() => S2.refresh(true)}>Detect SDR</button></div>
        <p class="hint">${st ? st.sdr.info.join(' · ') : ''} Tune about 5 kHz below 435.000 MHz (RTL-SDR offset; 434.9936 works). If the dongle stalls, unplug and replug it.</p>
      </div>
      <div class="cards">
        <div class="card"><h2>Decoded frames and packets</h2>
          <${Terminal} lines=${frames.map(e => ({ t: e.t, cls: e.kind === 'frame' ? 'rf' : '', msg: describe(e) }))} height=${260} /></div>
        <div class="card"><h2>CW beacon / text</h2>
          <${Terminal} lines=${cw.map(e => ({ t: e.t, cls: '', msg: describe(e) }))} height=${260} /></div>
      </div>
      <div class="card"><h2>Direwolf / SDR output</h2><${Terminal} lines=${s.sdr} height=${220} /></div>
    </div>`;
  }

  Object.assign(S2, { Digipeater, Images, Radio, startTest, ImageSource });
})();
