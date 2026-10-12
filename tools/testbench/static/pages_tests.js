/* Tests: the end-to-end test and every individual bench test */
(function () {
  const { h, api, useStore, log, Chip, Verdict, Kv, fmtT, useState, startTest, UI } = S2;
  const stop = () => api('/api/test/stop', {}).catch(e => log(e.message, 'err'));

  function E2E() {
    const s = useStore();
    const steps = s.e2e.length ? s.e2e : ((s.results.e2e && s.results.e2e.steps) || []);
    const r = s.results.e2e;
    const [o, setO] = useState({ cw: true, freq_mhz: 434.9936, gain: 40, alive_timeout: 150, cw_timeout: 130 });
    const st = s.status;
    const running = s.running === 'e2e';
    const need = [['Nucleo VCP (satellite side)', st && st.obc.connected, true], ['Ground station UART', st && st.gs.connected, false], ['RTL-SDR', st && st.sdr.present, false], ['Picture selected', st && st.image.bytes > 0, false]];
    const icon = x => ({ pass: '✔', fail: '✘', skip: '–', running: '…' }[x] || '·');
    return h`<div>
      <div class="card"><h2>End-to-end test <${Verdict} v=${r && r.verdict} /></h2>
        <p class="hint">Runs on the real hardware, in order: equipment · satellite alive · HEALTH SAT (ACK, housekeeping, ACK) · invalid opcode (NACK) · CW beacon · digipeater (8 frames, journal) · picture PC → CPU1 flash → CPU2 → RF → receiver → background builder → identical picture · report.
        A step that needs a receiver that is not connected is <b>skipped</b> and the verdict becomes PARTIAL; it is never faked.</p>
        <div class="row">${need.map(([k, ok, must]) => h`<${Chip} key=${k} cls=${ok ? 'ok' : (must ? 'bad' : 'warn')}>${k}<//>`)}</div>
        <div class="row" style="margin-top:10px">
          <label><input type="checkbox" checked=${o.cw} onChange=${e => setO({ ...o, cw: e.target.checked })} /> CW beacon step</label>
          <label>SDR MHz</label><input type="number" step="0.0001" style="width:100px" value=${o.freq_mhz} onInput=${e => setO({ ...o, freq_mhz: +e.target.value })} />
          <label>gain</label><input type="number" style="width:60px" value=${o.gain} onInput=${e => setO({ ...o, gain: +e.target.value })} />
          <button class="pri" disabled=${!!s.running || !(st && st.obc.connected)} onClick=${() => startTest('e2e', { ...o, fresh: false })}>Start end-to-end test</button>
          <button disabled=${!running} onClick=${stop}>Stop</button>
          <button onClick=${() => api('/api/report', { note: 'end-to-end test' }).then(j => log('report saved: ' + j.dir, 'ok')).catch(e => log(e.message, 'err'))}>Save report</button>
        </div>
      </div>
      <div class="card"><h2>Steps</h2>
        ${steps.length ? h`<ol class="steps">${steps.map(x => h`<li key=${x.n}><${Verdict} v=${x.state} /><div><b>${x.n}. ${x.name}</b>${x.seconds != null ? h` <span class="hint">${x.seconds} s</span>` : ''}<div class="hint mono">${x.detail}</div></div></li>`)}</ol>`
          : h`<p class="hint">not run yet</p>`}
        ${running && s.chunk && h`<div class="bar"><i style=${'width:' + (100 * (s.chunk.pkt || 0) / (s.chunk.total || 1)) + '%'}></i></div><span class="hint">chunk ${s.chunk.pkt} / ${s.chunk.total} (${s.chunk.src})</span>`}
        ${r && h`<${Kv} rows=${[['passed', r.pass], ['failed', r.fail], ['skipped', r.skip], ['receivers', Object.entries(r.receivers).map(([k, v]) => k + ':' + (v ? 'yes' : 'no')).join(' ')], r.image && r.image.session && ['picture', `session ${r.image.session}, ${r.image.chunks} chunks via ${r.image.src}`]]} />`}
      </div>
    </div>`;
  }

  /* generic test card */
  const Num = ({ v, set, w = 80, step }) => h`<input type="number" step=${step} style=${'width:' + w + 'px'} value=${v} onInput=${e => set(+e.target.value)} />`;

  const RowsView = ({ r }) => !r || !r.rows ? (r && r.why ? h`<p class="hint">${r.why}</p>` : null) : h`<table><tr><th>check</th><th></th><th>result</th><th>expected</th></tr>
    ${r.rows.map((x, i) => h`<tr key=${i}><td>${x.name}</td><td><${Verdict} v=${x.state === 'INFO' ? 'PARTIAL' : x.state} /></td><td class="mono" style="font-size:12px">${x.detail}</td><td class="hint">${x.expected || ''}</td></tr>`)}</table>`;

  function Card({ id, title, hint, fields, render, children }) {
    const s = useStore();
    const [cfg, setCfg] = useState(Object.fromEntries(fields.map(f => [f.k, f.v])));
    const r = s.results[id];
    const pg = s.progress[id];
    const field = f => f.type === 'text' ? h`<input class="mono" style="width:150px" value=${cfg[f.k]} onInput=${e => setCfg({ ...cfg, [f.k]: e.target.value })} />`
      : f.type === 'select' ? h`<select value=${cfg[f.k]} onChange=${e => setCfg({ ...cfg, [f.k]: e.target.value })}>${f.options.map(o => h`<option key=${o}>${o}</option>`)}</select>`
      : f.type === 'bool' ? h`<input type="checkbox" checked=${!!cfg[f.k]} onChange=${e => setCfg({ ...cfg, [f.k]: e.target.checked })} />`
      : h`<${Num} v=${cfg[f.k]} set=${x => setCfg({ ...cfg, [f.k]: x })} step=${f.step} />`;
    return h`<div class="card"><h2>${title} <${Verdict} v=${r && r.verdict === 'INFO' ? 'PARTIAL' : r && r.verdict} /></h2>
      <div class="row">${fields.map(f => h`<${React.Fragment} key=${f.k}><label>${f.label}</label>${field(f)}<//>`)}
        <button class="pri" disabled=${!!s.running} onClick=${() => startTest(id, { ...cfg, fresh: false })}>Run</button>
        ${s.running === id && h`<button onClick=${stop}>Stop</button>`}</div>
      <p class="hint">${hint}</p>
      ${s.running === id && pg && h`<div class="bar" style="margin:6px 0"><i style=${'width:' + (100 * pg.done / (pg.total || 1)) + '%'}></i></div>`}
      ${children}
      ${r ? (render ? render(r) : h`<${RowsView} r=${r} />`) : null}</div>`;
  }

  const why = r => (r.why ? h`<p class="hint">${r.why}</p>` : null);


  /* ---- image test: PC -> CPU1 flash (LittleFS) -> read back -> CPU2 -> RF -> receiver, with the stage bars, chunk grid and full result */
  const SRC_COL = { store: 'store', read: 'read', ack: 'ack', gs: 'on' };
  function ImageTest() {
    const s = useStore();
    const [rx, setRx] = useState('image');
    const [led, setLed] = useState(false);
    const [freq, setFreq] = useState(434.9936), [gain, setGain] = useState(40);
    const st = s.status, info = st && st.image;
    const r = s.results[rx] || null;
    const cm = s.cmap;
    const running = s.running === 'image' || s.running === 'sdrimage';
    const total = cm ? cm.total : (info ? info.chunks : 0);
    const n = cm ? cm.n : { store: 0, read: 0, ack: 0, gs: 0 };
    const stage = (label, v, dot) => h`<div style="margin:6px 0"><div class="row"><i class=${'dot ' + (dot || '')}></i><b>${label}</b><span class="grow"></span><span class="mono">${v} / ${total}</span></div><div class="bar"><i style=${'width:' + (total ? 100 * v / total : 0) + '%'}></i></div></div>`;
    const ph = s.phase;
    const verdict = r && r.verdict;
    const start = () => startTest(rx, rx === 'image' ? { mode: 'store', ledtrace: led, fresh: true } : { freq_mhz: freq, gain, fresh: false });
    return h`<div class="card"><h2>Image test <${Verdict} v=${verdict} /></h2>
      <p class="hint">① the PC sends the picture chunk by chunk to <b>CPU1</b>, which stores it as a file on LittleFS in its internal flash · ② it is read back and compared · ③ CPU1 feeds CPU2, which transmits it · ④ the receiver gets it: the ground station (UART) or an RTL-SDR with Dire Wolf (independent decode). The hash of what arrives must equal the hash of the file sent.</p>
      <${S2.ImageSource} send=${false} />
      <div class="row" style="margin-top:10px"><label>receiver</label><select value=${rx} onChange=${e => setRx(e.target.value)} disabled=${running}><option value="image">ground station UART</option><option value="sdrimage">SDR + Direwolf</option></select>
        ${rx === 'image' ? h`<label><input type="checkbox" checked=${led} onChange=${e => setLed(e.target.checked)} /> LED trace over SWD (uses the ST-Link)</label>` : h`<label>MHz</label><input type="number" step="0.0001" style="width:100px" value=${freq} onInput=${e => setFreq(+e.target.value)} /><label>gain</label><input type="number" style="width:60px" value=${gain} onInput=${e => setGain(+e.target.value)} />`}
        <button class="pri" disabled=${!!s.running || !(info && info.bytes)} onClick=${start}>Start image test</button><button disabled=${!running} onClick=${stop}>Stop</button></div>
      ${(running || cm) && h`<div style="margin-top:10px">
        ${ph && h`<div class="hint">phase: <b>${ph.name}</b> — Nucleo LED ${ph.led}</div>`}
        ${stage('① CPU1 writes the file to its flash', n.store, 'run')}${stage('② read back from CPU1 flash', n.read, 'run')}${stage('③ CPU2 transmitted (acknowledged by the Nucleo)', n.ack, 'warn')}${stage('④ received by ' + (rx === 'image' ? 'the ground station' : 'Direwolf'), n.gs, 'ok')}
        <div class="grid" style="grid-template-columns:repeat(auto-fill,minmax(9px,1fr))">${(cm ? cm.st : []).map((c, i) => h`<i key=${i} class=${SRC_COL[c] || ''} title=${'chunk ' + (i + 1)}></i>`)}</div></div>`}
      ${r && r.verdict && (rx === 'image' ? h`<div style="margin-top:10px"><div class="row" style="align-items:flex-start"><div><img src=${'/api/image/source?' + (r.src_sha256 || '').slice(0, 8)} style="max-height:140px;border-radius:6px" /><div class="hint">sent</div></div>
          <div><img src=${'/api/image/received?' + (r.rx_sha256 || '').slice(0, 8)} style="max-height:140px;border-radius:6px" /><div class="hint">received over the air</div></div>
          <div class="grow"><${Kv} rows=${[['chunks', `${r.received}/${r.chunks} received · ${r.acked} acknowledged · ${r.retransmits} re-send(s)` + (r.missing && r.missing.length ? ' · missing ' + r.missing.slice(0, 20).join(', ') : '')],
            r.flash_sha256 && ['CPU1 flash', `${r.stored_in_flash}/${r.chunks} chunks programmed · read-back ${r.flash_readback_ok} OK · ${r.flash_match ? 'identical to the file' : 'DIFFERENT'}`],
            r.store_info && ['CPU1 file', h`<span class="mono">${r.store_info}</span>`],
            ['sha256 sent', h`<span class="mono">${r.src_sha256}</span>`], ['sha256 received', h`<span class="mono">${r.rx_sha256}</span>`],
            ['air', `${r.elapsed_s} s · ${r.payload_bps} bit/s payload · Nucleo answer ${r.ack_ms_avg ?? '?'} ms · RSSI ${r.rssi_avg ?? '?'} dBm`],
            r.led_check && ['LED pins (SWD)', Object.entries(r.led_check.phase_check || {}).map(([k, v]) => `${k}: expected ${v.expected.join('/')} seen ${Math.round(100 * (v.seen_fraction || 0))} %`).join(' · ')]]} /></div></div></div>`
        : h`<div style="margin-top:10px"><${Kv} rows=${[['Direwolf frames', r.frames], ['valid camera fragments (type 0x08, CRC-16/X.25 ok)', `${r.valid} / ${r.fragments}` + (r.missing && r.missing.length ? ' · missing ' + r.missing.join(', ') : '')], ['other frames', r.other],
          ['AX.25 source -> destination', h`<span class="mono">${(r.addresses || []).join(' | ')}</span>`], ['control / PID / info lengths', `${r.control} / ${r.pid} / ${(r.info_lengths || []).join(', ')}`],
          ['sha256 sent', h`<span class="mono">${r.src_sha256}</span>`], ['sha256 rebuilt from the frames', h`<span class="mono">${r.rx_sha256}</span>`], ['time on air', r.elapsed_s + ' s']]} />
          <img src=${'/api/image/received?' + Math.round(r.when)} style="max-height:160px;border-radius:6px;margin-top:6px" /></div>`)}
      ${r && r.why && h`<p class="hint">${r.why}</p>`}
    </div>`;
  }

  function Tests() {
    const s = useStore();
    const ALL = [['e2e', 'End-to-end'], ['image', 'Image (GS)'], ['sdrimage', 'Image (SDR)'], ['link', 'Link'], ['digi', 'Digipeater'], ['sdr', 'SDR decode'], ['rate', 'Air rate'], ['cwtest', 'CW / GMSK'], ['imageloop', 'Fragment loop'], ['fwcheck', 'Firmware match'], ['lfsproof', 'LittleFS proof'], ['bootcfg', 'Boot config'], ['membudget', 'Memory budget'], ['profcap', 'Profile'], ['vcpfuzz', 'VCP fuzz'], ['flashio', 'Flash I/O'], ['cmdconf', 'Commands'], ['nackmatrix', 'NACK matrix'], ['beacontime', 'Beacon timing'], ['winmap', 'Listen window'], ['soak', 'Soak']];
    const cub = s.status && s.status.cubus;
    return h`<div>
      ${cub && h`<div class="card" style="border-color:var(--warn)"><h2>CUBUS profile</h2><p class="hint">The satellite is configured for the CUBUS flight board: tests that need the Nucleo bench app on CPU1 (image, digipeater, rate, CW, VCP, flash IO, end-to-end) refuse to start. Use the <a href="#cubusdebug">CUBUS debug</a> page and the ground-station / SWD tests below.</p></div>`}
      <div class="card"><h2>All tests</h2><div class="row">${ALL.map(([k, t]) => { const r = s.results[k]; return h`<span key=${k} class="chip"><i class=${'dot ' + (s.running === k ? 'run' : r ? (r.verdict === 'PASS' ? 'ok' : r.verdict === 'FAIL' ? 'bad' : 'warn') : '')}></i>${t} ${r && r.verdict ? r.verdict : s.running === k ? 'running' : 'not run'}</span>`; })}</div>
        <div class="hint">Every result goes into the next report (Reports / logs). Digipeater details: Digipeater page.</div></div>
      <${E2E} />
      <${ImageTest} />
      <div class="cards">
        <${Card} id="digi" title="Digipeater decisions (8 cases + store-and-forward journal)" fields=${[]} hint="Needs only the Nucleo VCP; run at most once a minute (rate limiter). Full table on the Digipeater page."
          render=${r => r.rows ? h`<table><tr><th>#</th><th>case</th><th>expected</th><th>satellite said</th><th></th></tr>${r.rows.map(x => h`<tr key=${x.n}><td>${x.n}</td><td>${x.name}</td><td>${x.expected}</td><td>${x.got}</td><td><${Verdict} v=${x.ok ? 'PASS' : 'FAIL'} /></td></tr>`)}</table><p class="hint">${r.journal} · stored ${r.stored} msg, ${r.stored_bytes} B</p>` : why(r)} />
        <${Card} id="link" title="Link test (PING)" fields=${[{ k: 'count', label: 'PINGs', v: 5 }]} hint="GS → satellite PING. Pass = ACK accepted + housekeeping + ACK completed decoded on the GS." render=${r => r.rows ? h`<table><tr><th>#</th><th>result</th><th>tries</th><th>latency</th><th>RSSI</th></tr>
          ${r.rows.map(x => h`<tr key=${x.n}><td>${x.n}</td><td><${Verdict} v=${x.ok ? 'PASS' : 'FAIL'} />${x.why || ''}</td><td>${x.tries ?? ''}</td><td>${x.latency_ms ?? ''} ms</td><td>${x.rssi ?? ''}</td></tr>`)}</table><p class="hint">${r.passed}/${r.total} passed</p>` : why(r)} />
        <${Card} id="rate" title="Raw air-rate test" fields=${[{ k: 'count', label: 'packets', v: 60 }, { k: 'gaps', label: 'gaps ms', v: '300,100,50,20,0', type: 'text' }]} hint="CPU2 sends N packets straight from the radio with each pause; the GS counts them." render=${r => r.rows ? h`<table><tr><th>gap</th><th>air rate</th><th>payload</th><th>sent</th><th>got</th><th>lost</th></tr>
          ${r.rows.map((x, i) => h`<tr key=${i}><td>${x.gap_ms} ms</td><td>${x.air_bps ?? ''} bit/s</td><td>${x.payload_bps ?? ''}</td><td>${x.sat_sent}/${x.n}</td><td>${x.gs_got}</td><td>${x.lost ?? ''}</td></tr>`)}</table>` : why(r)} />
        <${Card} id="sdr" title="SDR + Direwolf (independent decode)" fields=${[{ k: 'count', label: 'PINGs', v: 3 }, { k: 'freq_mhz', label: 'MHz', v: 434.9936, step: 0.0001 }, { k: 'gain', label: 'gain', v: 40 }]} hint="PINGs through the GS, decoded by an RTL-SDR and Direwolf without the GS's help."
          render=${r => r.frames ? h`<${Kv} rows=${[['Direwolf frames', r.direwolf_frames], ['GS frames', r.gs_frames]]} /><pre>${r.frames.join('\n')}</pre>` : why(r)} />
        <${Card} id="cwtest" title="CW HELLO / carrier / GMSK" fields=${[{ k: 'seconds', label: 'seconds', v: 120 }]} hint="Satellite keys HELLO on the downlink for a spectrum analyser. Mode: cw, gmsk, carrier."
          render=${r => r.seconds ? h`<p class="hint">sent ${r.seconds} s, GS decoded HELLO ${r.gs_decoded_hello}×</p>` : why(r)}>
          <div class="row">${['cw', 'gmsk', 'carrier'].map(m => h`<button key=${m} disabled=${!!s.running} onClick=${() => startTest('cwtest', { seconds: 120, mode: m, fresh: false })}>${m} 120 s</button>`)}
            ${s.running === 'cwtest' && h`<button onClick=${stop}>Stop</button>`}</div><//>
        <${Card} id="imageloop" title="Fragment loop (receiver-side soak)" fields=${[{ k: 'fragments', label: 'fragments', v: 10 }, { k: 'seconds', label: 'seconds', v: 3600 }]} hint="Satellite repeats the first fragments so a receiver can be tuned. Stored by the background builder too." />
      </div>
      <h2 style="margin:18px 0 8px">Boot, firmware and memory (SWD only: no radio, no UART)</h2>
      <div class="cards">
        <${Card} id="fwcheck" title="Firmware match" fields=${[{ k: 'target', label: 'board', v: 'sat', type: 'select', options: ['sat', 'gs'] }]} hint="Reads the board's flash over SWD and compares it with the build in this repository (CPU2 image and NuttX image). Finds boards that run another firmware than the one you think." />
        <${Card} id="lfsproof" title="Picture really in CPU1's LittleFS" fields=${[{ k: 'target', label: 'board', v: 'sat', type: 'select', options: ['sat', 'gs'] }]} hint="Reads flash pages 72..99 (0x08024000) straight over SWD, no firmware involved, finds the LittleFS superblock and checks that the exact bytes of the chosen picture are in it. Run it after an image test, with the same picture still selected." />
        <${Card} id="bootcfg" title="Boot configuration and self-tests" fields=${[{ k: 'target', label: 'board', v: 'sat', type: 'select', options: ['sat', 'gs'] }]} hint="CPU2 boot address option byte, C2BOOT, vector table, the integrity-check and digipeater self-test words, CPU2 heartbeat." />
        <${Card} id="membudget" title="Memory budget" fields=${[{ k: 'target', label: 'board', v: 'sat', type: 'select', options: ['sat', 'gs'] }, { k: 'flash_max_pct', label: 'flash max %', v: 97 }, { k: 'ram_max_pct', label: 'RAM max %', v: 90 }]} hint="Flash and static RAM headroom of both cores from the ELF against limits. No hardware needed." />
        <${Card} id="profcap" title="CPU profile capture" fields=${[{ k: 'target', label: 'board', v: 'sat', type: 'select', options: ['sat', 'gs'] }, { k: 'seconds', label: 'seconds', v: 30 }]} hint="Samples the program counter of both cores for N seconds and puts the result into the report." />
      </div>
      <h2 style="margin:18px 0 8px">Nucleo only (ST-Link VCP)</h2>
      <div class="cards">
        <${Card} id="vcpfuzz" title="VCP robustness (corrupt frames)" fields=${[]} hint="Sends bad-CRC, bit-flipped, truncated, garbage and burst frames to CPU1. It must ignore them, recover and keep answering STATUS." />
        <${Card} id="flashio" title="CPU1 flash file: write, read back, speed" fields=${[{ k: 'chunks', label: 'chunks', v: 40 }, { k: 'rounds', label: 'rounds', v: 2 }, { k: 'confirm', label: 'replace the stored picture', v: false, type: 'bool' }]} hint="Writes a pseudo-random file into CPU1's LittleFS, reads every chunk back and compares. WARNING: replaces the picture stored in CPU1's flash." />
      </div>
      <h2 style="margin:18px 0 8px">Link (ground-station board + satellite)</h2>
      <div class="cards">
        <${Card} id="cmdconf" title="Command conformance" fields=${[]} hint="HK1, HK2, PING, stored housekeeping, ADCS, EPDM and an invalid opcode: ACK accepted, data, ACK completed with latencies; the invalid one must be NACK 0x09." />
        <${Card} id="nackmatrix" title="Malformed commands (NACK matrix)" fields=${[]} hint="Invalid opcode (must NACK 0x09) and four other malformed 13-byte commands whose answers are recorded as observations." />
        <${Card} id="beacontime" title="CW beacon timing" fields=${[{ k: 'seconds', label: 'seconds', v: 600 }, { k: 'tol_s', label: '± s', v: 4 }]} hint="Listens: period of the decoded beacons against the configured period, callsign text, RSSI. Detects missed decodes." />
        <${Card} id="winmap" title="Listen-window map" fields=${[{ k: 'step_s', label: 'step s', v: 10 }, { k: 'sweeps', label: 'sweeps', v: 2 }]} hint="Sends one PING at every offset of the beacon cycle and records which offsets are answered: the real half-duplex listen window (about 50 s expected)." />
        <${Card} id="soak" title="Link soak" fields=${[{ k: 'minutes', label: 'minutes', v: 30 }, { k: 'interval_s', label: 'every s', v: 90 }]} hint="PING once per cycle for N minutes: success rate, latency, sends needed, RSSI." render=${r => h`<div><${RowsView} r=${r} />${r.series && r.series.length > 1 ? h`<${UI.Spark} data=${r.series.map(x => x.seconds || 0)} />` : ''}</div>`} />
      </div>
    </div>`;
  }

  Object.assign(S2, { Tests, RowsView });
})();
