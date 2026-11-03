/* Mission Control: building blocks shared by the screens (cards, page header, tiles, decoded replies, raw log). */
(function () {
  const { h, S, api, useS, useNow, useState, useEffect, useRef, utc, clock, age, fx, dur, hexBytes, textHex, last, limOf, emit } = MC;
  const { Spark } = UI;

  const Help = ({ text }) => h`<span class="qm" tabIndex="0" role="note" aria-label=${text}>i<span class="tip">${text}</span></span>`;
  const Card = ({ title, help, tour, actions, cls, children }) => h`<section class=${'card ' + (cls || '')} data-tour=${tour}>
    <header><h2>${title}</h2>${help && h`<${Help} text=${help} />`}<span class="grow"></span>${actions}</header><div class="cbody">${children}</div></section>`;
  /* every screen except the dashboard starts with this: a way back to Home, the title and an optional sub-line */
  const PageHead = ({ title, sub, back, actions, section = true }) => h`<div class="phead">
    <div class="crumbs"><a class="home" href="#home" data-tour="home">← Back to Home</a>${section && h`<span class="sep">/</span><a href="#dashboard">Satellite control</a>`}${back && h`<span class="sep">/</span><a href=${back[0]}>${back[1]}</a>`}</div>
    <div class="ptitle"><h1>${title}</h1><span class="grow"></span>${actions}</div>${sub && h`<p class="sub">${sub}</p>`}</div>`;
  const Led = ({ s }) => h`<i class=${'led ' + (s || 'none')}></i>`;
  const Code = ({ c }) => h`<span class="code">${c}</span>`;

  /* ---------- telemetry items: [key, label, unit, decimals, limit key, description] */
  const TEL = [
    ['Power', [['bat_v', 'Battery voltage', 'V', 2, 'bat_v', 'Battery terminal voltage.'], ['bus_v', 'Bus voltage', 'V', 2, 'bus_v', 'Main power bus voltage.'],
      ['sol_v', 'Solar input voltage', 'V', 2, null, 'Combined solar array voltage.'], ['bat_i', 'Battery current', 'A', 2, null, 'Positive while charging, negative while discharging.'],
      ['unreg_i', 'Unregulated current', 'A', 2, null, ''], ['i_3v3', '3.3 V rail current', 'A', 2, null, ''], ['i_5v', '5 V rail current', 'A', 2, null, ''], ['bus_i', 'Bus current', 'A', 2, null, '']]],
    ['Solar panels', [1, 2, 3, 4, 5].map(i => ['panels_v.SP' + i, 'Panel ' + i + ' voltage', 'V', 2, null, 'A value near 0 V indicates the panel is not illuminated.'])],
    ['Temperatures', [['bat_t', 'Battery', '°C', 1, 'bat_t', ''], ['ant_t', 'Antenna deployment', '°C', 1, 'ant_t', ''], ['bpb_t', 'Backplane board', '°C', 1, 'bpb_t', '']]],
    ['Inertial and magnetic', [['gyro.0', 'Angular rate X', '°/s', 2, null, ''], ['gyro.1', 'Angular rate Y', '°/s', 2, null, ''], ['gyro.2', 'Angular rate Z', '°/s', 2, null, ''],
      ['mag.0', 'Magnetic field X', 'µT', 1, null, ''], ['mag.1', 'Magnetic field Y', 'µT', 1, null, ''], ['mag.2', 'Magnetic field Z', 'µT', 1, null, '']]],
  ];
  const getPath = (o, p) => p.split('.').reduce((a, k) => (a == null ? a : a[k]), o);
  /* newest value of one item: housekeeping packet, or (battery voltage / current / temperature) from the matching CW beacon */
  function telValue(s, key) {
    const hk = last(s.hk.filter(x => typeof getPath(x, key) === 'number'));
    const b1 = MC.lastBeacon(s, 1), b2 = MC.lastBeacon(s, 2);
    const fromBeacon = () => {
      if (key === 'bat_v' && b1) return { v: b1.b1.bat_v, t: b1.t, src: 'CW beacon 1' };
      if (key === 'bat_i' && b1) return { v: b1.b1.bat_i, t: b1.t, src: 'CW beacon 1' };
      if (key === 'bat_t' && b2 && Math.abs(b2.b2.bat_temp_c) < 80 && b2.b2.bat_temp_c !== 0) return { v: b2.b2.bat_temp_c, t: b2.t, src: 'CW beacon 2' };
      return null;
    };
    const b = fromBeacon();
    if (hk && (!b || hk.t >= b.t)) return { v: getPath(hk, key), t: hk.t, src: 'housekeeping' };
    return b;
  }
  const seriesOf = (s, key, n = 60) => s.hk.slice(-n).map(x => getPath(x, key)).filter(v => typeof v === 'number');

  function Tile({ label, value, unit, d = 2, state, hint, sub, spark }) {
    const txt = typeof value === 'number' ? fx(value, d) : value == null ? '–' : value;
    return h`<div class=${'tile ' + (state || '')} title=${hint || ''}><small>${label}</small>
      <b><span key=${txt} class="val">${txt}</span>${unit && value != null && h`<span class="unit">${unit}</span>`}</b>
      ${sub && h`<em>${sub}</em>`}${spark && spark.length > 1 && h`<${Spark} data=${spark} h=${24} />`}</div>`;
  }
  function TelTile({ s, def, spark }) {
    const [key, label, unit, d, lim, hint] = def;
    const v = telValue(s, key);
    const l = lim && limOf(s, lim);
    const bad = v && l && ((l.lo != null && v.v < l.lo) || (l.hi != null && v.v > l.hi));
    return h`<${Tile} label=${label} value=${v ? v.v : null} unit=${unit} d=${d} state=${bad ? 'bad' : ''} hint=${hint} sub=${v ? age(v.t) + ' ago · ' + v.src : 'no data'} spark=${spark ? seriesOf(s, key) : null} />`;
  }

  /* ---------- byte views */
  const decodeText = bytes => bytes.map(b => (b === 10 || b === 13 || b === 9 || (b >= 32 && b < 127)) ? String.fromCharCode(b) : (b === 0 ? '' : '·')).join('');
  function HexDump({ hex, text }) {
    const b = text != null ? hexBytes(textHex(text)) : hexBytes(hex);
    if (!b.length) return h`<div class="hint">No data.</div>`;
    const rows = [];
    for (let i = 0; i < b.length; i += 16) rows.push(b.slice(i, i + 16));
    return h`<pre class="hexdump mono">${rows.map((r, i) => (i * 16).toString(16).padStart(4, '0') + '  ' + r.join(' ').padEnd(47) + '  ' + r.map(x => { const c = parseInt(x, 16); return c >= 32 && c < 127 ? String.fromCharCode(c) : '·'; }).join('')).join('\n')}</pre>`;
  }

  /* ---------- decoded replies */
  const NACK_HELP = {
    2: 'The command packet was received corrupted. Retransmit.', 3: 'Checksum mismatch. Retransmit.', 4: 'The command was shorter than the format requires.',
    5: 'The requested data is not present on the on-board computer.', 6: 'The data exists but cannot be read at this time.', 7: 'The satellite is busy with another operation. Retry later.',
    8: 'The addressed mission is inactive.', 9: 'The command is not in the satellite’s command table.', 10: 'Memory or file system error on board.', 11: 'The satellite could not transmit the response.',
  };
  const STATES = { queued: ['Queued', 'wait'], sending: ['Transmitting', 'busy'], accepted: ['Accepted, in progress', 'busy'], completed: ['Completed', 'ok'], nack: ['Rejected by satellite', 'bad'],
    timeout: ['No response', 'warn'], incomplete: ['Accepted, completion not confirmed', 'warn'], cancelled: ['Cancelled', 'none'], error: ['Transmission error', 'bad'], sent: ['Transmitted (no response expected)', 'ok'] };
  const REPLY_KINDS = new Set(['ack', 'nack', 'hk', 'data', 'digi', 'file', 'ascii']);
  const RAW_ONLY = ['adcs', 'epdm', 'adcs_file', 'epdm_file'];      // formats not final: the received bytes are shown as decoded text, nothing is interpreted
  function jobEvents(job) {
    const end = job.t_end ? job.t_end + 8 : Infinity;
    const a = S.evt.filter(e => REPLY_KINDS.has(e.kind) && e.t >= job.t - 0.5 && e.t <= end);
    ((job.detail && job.detail.events) || []).forEach(e => { if (REPLY_KINDS.has(e.kind) && !a.some(x => x.kind === e.kind && Math.abs(x.t - e.t) < 0.01)) a.push(e); });
    return a.sort((x, y) => x.t - y.t);
  }
  function jobRaw(job) {
    const d = (job.detail && job.detail.raw) || [];
    const end = job.t_end ? job.t_end + 8 : Infinity;
    const live = S.raw.filter(r => r.t >= job.t - 0.5 && r.t <= end);
    return live.length >= d.length ? live : d;
  }

  /* a file download arrives as numbered 128-byte pieces: assemble them. Unfinished formats (ADCS / EPDM) are shown only as the text decoded from the received bytes. */
  function FileCard({ chunks, raw, name, rawOnly }) {
    const [hexv, setHexv] = useState(false);
    const by = new Map();
    chunks.forEach(c => by.set(c.pkt, c));
    const total = Math.max(...chunks.map(c => c.total || 0), by.size);
    const missing = [];
    for (let i = 1; i <= total; i++) if (!by.has(i)) missing.push(i);
    const bytes = [];
    [...by.keys()].sort((a, b) => a - b).forEach(k => { const c = by.get(k); hexBytes(c.hex).slice(0, c.len || 128).forEach(x => bytes.push(parseInt(x, 16))); });
    let end = bytes.length;
    while (end > 0 && bytes[end - 1] === 0) end--;
    const body = bytes.slice(0, end);
    const printable = body.filter(b => b === 9 || b === 10 || b === 13 || (b >= 32 && b < 127)).length;
    const isText = end > 0 && printable / end > 0.85;
    const hex = body.map(b => b.toString(16).padStart(2, '0')).join('');
    const save = () => {
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([Uint8Array.from(body)], { type: 'application/octet-stream' }));
      a.download = name || 'satellite_file.bin'; a.click();
    };
    const st = chunks.find(c => c.status);
    const showHex = hexv || raw || (!rawOnly && !isText && end > 0);
    return h`<div class=${'rep ' + (missing.length || st ? 'warn' : 'ok')}><b>${name ? 'File ' + name : 'File'} received</b>
      <span class="hint">${by.size} of ${total} blocks · ${bytes.length} bytes${missing.length ? ' · missing blocks: ' + missing.slice(0, 20).join(', ') : ''}${st ? ' · read error reported (status ' + st.status + ')' : ''}</span>
      <div class="bar"><i style=${'width:' + (100 * by.size / (total || 1)) + '%'}></i></div>
      ${rawOnly && h`<div class="note">Content format not yet specified. The received bytes are shown as decoded text, without interpretation.</div>`}
      ${end === 0 ? h`<div class="hint">The file is empty.</div>` : (rawOnly || isText) ? h`<pre class="filetext mono">${decodeText(body)}</pre>` : h`<div class="hint">The content is not text; the bytes are shown below.</div>`}
      ${showHex && end > 0 && h`<${HexDump} hex=${hex} />`}
      ${end > 0 && h`<div class="row"><button class="sm" onClick=${() => setHexv(!hexv)}>${hexv ? 'Hide hexadecimal' : 'Show hexadecimal'}</button><button class="sm" onClick=${save}>Download file</button></div>`}</div>`;
  }

  function EventCard({ e, raw }) {
    const b2 = n => (n == null ? '00' : n.toString(16).toUpperCase().padStart(2, '0'));
    if (e.kind === 'ack') return h`<div class="rep ok"><b>${e.what === 'completed' ? 'Command completed' : 'Command accepted'}</b>
      <span class="hint">ACK · ${e.mission_name ? 'mission: ' + e.mission_name + ' · ' : ''}${clock(e.t)}</span>${raw && h`<div class="hint">Over the air: AC ${e.what === 'completed' ? '01' : '00'} ${b2(e.mission)}</div><${HexDump} hex=${'AC' + (e.what === 'completed' ? '01' : '00') + b2(e.mission)} />`}</div>`;
    if (e.kind === 'nack') return h`<div class="rep bad"><b>Command rejected: ${e.name || 'error ' + e.err}</b>
      <span>${NACK_HELP[e.err] || ''}</span><span class="hint">NACK · error 0x${b2(e.err)}${e.mission_name ? ' · mission: ' + e.mission_name : ''} · ${clock(e.t)}</span>${raw && h`<div class="hint">Over the air: EE ${b2(e.err)} ${b2(e.mission)}</div><${HexDump} hex=${'EE' + b2(e.err) + b2(e.mission)} />`}</div>`;
    if (e.kind === 'hk') return h`<div class="rep ok"><b>Housekeeping packet</b><span class="hint">${clock(e.t)} · sequence ${e.seq} · checksum ${e.crc_ok ? 'valid' : 'invalid'}</span>
      <div class="tiles mini">${[['bat_v', 'Battery', 'V'], ['bus_v', 'Bus', 'V'], ['sol_v', 'Solar', 'V'], ['bat_i', 'Battery current', 'A'], ['bat_t', 'Battery temp.', '°C'], ['ant_t', 'Antenna temp.', '°C']].map(([k, l, u]) =>
        h`<${Tile} key=${k} label=${l} value=${e[k]} unit=${u} d=${2} />`)}</div>${raw && h`<${HexDump} hex=${e.hex} />`}</div>`;
    if (e.kind === 'ascii') return h`<div class="rep ok"><b>Text response</b><span class="mono big2">${e.text}</span><span class="hint">${clock(e.t)}</span>${raw && h`<${HexDump} text=${e.text} />`}</div>`;
    if (e.kind === 'data') return h`<div class="rep ok"><b>${e.title || 'Data'}</b><span class="hint">${e.source || ''} · ${clock(e.t)}</span>
      <table class="kv">${(e.fields || []).map(([k, v]) => h`<tr key=${k}><td>${k}</td><td><b>${v}</b></td></tr>`)}</table>${raw && h`<${HexDump} hex=${e.hex} />`}</div>`;
    if (e.kind === 'digi') return h`<div class="rep ok"><b>Frame repeated by the satellite</b><span class="mono">${e.text}</span></div>`;
    return null;
  }

  function Stepper({ job, evs }) {
    const st = job.state;
    const acc = evs.some(e => e.kind === 'ack' && e.what === 'accepted') || ['accepted', 'completed', 'incomplete'].includes(st);
    const fin = evs.some(e => e.kind === 'ack' && e.what === 'completed') || st === 'completed';
    const nack = st === 'nack', gone = st === 'timeout' || st === 'error';
    const steps = [[job.mock ? 'Transmitted (mock)' : 'Transmitted', st !== 'queued', false], [nack ? 'Rejected by satellite' : gone ? 'No response' : 'Acknowledged by satellite', acc || nack, nack || gone], ['Completed', fin, false]];
    const cur = steps.findIndex(x => !x[1]);
    const stopped = ['nack', 'timeout', 'error', 'cancelled', 'incomplete'].includes(st);
    return h`<ol class="steps">${steps.map(([t, done, fail], i) => h`<li key=${i} class=${fail ? 'fail' : done ? 'done' : i === cur && !stopped ? 'now' : ''}><i>${fail ? '✕' : done ? '✓' : i + 1}</i>${t}</li>`)}</ol>`;
  }

  function ReplyView({ job, defaultRaw }) {
    useS();
    const rawOnly = RAW_ONLY.includes(job.cmd);
    const [raw, setRaw] = useState(!!defaultRaw);
    const now = useNow(1);
    const evs = jobEvents(job);
    const [txt, cls] = STATES[job.state] || [job.state, ''];
    const running = ['queued', 'sending', 'accepted'].includes(job.state);
    const el = (job.t_end || now) - job.t;
    const files = evs.filter(e => e.kind === 'file');
    const data = evs.filter(e => ['hk', 'data', 'digi', 'file', 'ascii'].includes(e.kind));
    const fname = (job.label.match(/\(([a-z_]+\.txt)/) || [])[1];
    const lines = jobRaw(job);
    return h`<div class="reply" data-tour="reply">
      <div class="rhead"><${Code} c=${job.icon} /><div><b>${job.simple || job.label}</b><div class="hint">${clock(job.t)} · elapsed ${dur(el)}${job.mock ? ' · MOCK' : ''}</div></div><span class="grow"></span>
        <span class=${'pill ' + cls}>${running && h`<i class="spin"></i>`}${txt}</span></div>
      <${Stepper} job=${job} evs=${evs} />
      ${running && h`<div class="note">${job.state === 'accepted' ? 'The satellite is executing the command. Long transfers, such as images, can take several minutes.' :
        `The satellite receives for about 50 s of every 90 s cycle. The command is repeated automatically (attempt ${(job.detail && job.detail.attempt) || 1} of 8).`}</div>`}
      ${job.detail && job.detail.error && !running && !evs.some(e => e.kind === 'nack') && h`<div class="note bad">${job.detail.error}</div>`}
      <div class="repl">${evs.length ? [...evs.filter(e => e.kind !== 'file').map((e, i) => h`<${EventCard} key=${e.kind + e.t + i} e=${e} raw=${raw} />`), files.length ? h`<${FileCard} key="file" chunks=${files} raw=${raw} name=${fname} rawOnly=${rawOnly} />` : null] : h`<div class="hint">${running ? 'Waiting for the first response.' : 'No response received.'}</div>`}
        ${job.state === 'completed' && !data.length && h`<div class="hint">This command returns confirmations only; no data packet is part of the response.</div>`}</div>
      <div class="row"><button class="sm" onClick=${() => setRaw(!raw)} data-tour="rawbtn">${raw ? 'Hide raw' : 'Show raw'}</button>
        ${running && !job.mock && h`<button class="sm danger" onClick=${() => api('/api/command/cancel', { id: job.id })}>Abort</button>`}</div>
      ${raw && h`<div class="rawbox"><div class="hint">Command line sent to the ground station</div><pre class="mono">${job.line}</pre>
        ${job.opcode && h`<div class="hint">Command bytes (SAT-ID, MCU, opcode; data id, address and count follow)</div><pre class="mono">${job.opcode}</pre>`}
        <div class="hint">Ground station output during this command (${lines.length} lines)</div>
        <pre class="mono rawlines">${lines.map(r => clock(r.t) + '  ' + r.text).join('\n') || '(none)'}</pre></div>`}</div>`;
  }

  /* ---------- raw log: lines the ground station printed */
  const FILTERS = [['all', 'All', null], ['cw', 'CW beacon', /CW|S2S2 9N2SJ/i], ['cmd', 'Commands', /UPLINK|ACK|NACK|\[GS TX\]/], ['radio', 'Radio packets', /\[GS RX\]|RSSI/], ['err', 'Faults', /NACK|error|fail|timeout/i]];
  function lineClass(t) { return /NACK|error|fail/i.test(t) ? 'bad' : /ACK/.test(t) ? 'ok' : /CW/.test(t) ? 'cw' : /UPLINK|TX/.test(t) ? 'tx' : ''; }
  function RawLog({ compact }) {
    const s = useS();
    const [f, setF] = useState('all'), [q, setQ] = useState(''), [hex, setHex] = useState(false), [paused, setPaused] = useState(false);
    const frozen = useRef([]);
    const box = useRef(null);
    const flt = FILTERS.find(x => x[0] === f)[2];
    let rows = paused ? frozen.current : s.raw;
    useEffect(() => { if (!paused) frozen.current = s.raw; });
    rows = rows.filter(r => (!flt || flt.test(r.text)) && (!q || r.text.toLowerCase().includes(q.toLowerCase())));
    const shown = rows.slice(compact ? -40 : -400);
    useEffect(() => { if (!paused && box.current) box.current.scrollTop = box.current.scrollHeight; }, [s.raw.length, f, q, paused]);
    const dl = () => {
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([shown.map(r => new Date(r.t * 1000).toISOString() + '  ' + r.text).join('\n')], { type: 'text/plain' }));
      a.download = 's2s2_ground_station_log.txt'; a.click();
    };
    const older = () => api('/api/rawlog?n=2000' + (q ? '&q=' + encodeURIComponent(q) : '')).then(r => { S.raw = r; emit(); }).catch(() => {});
    return h`<div class="rawlog">
      <div class="row tools">${FILTERS.map(([k, l]) => h`<button key=${k} class=${'chipbtn ' + (f === k ? 'on' : '')} onClick=${() => setF(k)}>${l}</button>`)}
        <input class="search" placeholder="Search" value=${q} onInput=${e => setQ(e.target.value)} aria-label="Search the log" />
        <button class=${'sm ' + (hex ? 'on' : '')} onClick=${() => setHex(!hex)} data-tour="hexlog">${hex ? 'Hexadecimal' : 'Text'}</button>
        <button class="sm" onClick=${() => setPaused(!paused)}>${paused ? 'Resume' : 'Pause'}</button>
        ${!compact && h`<button class="sm" onClick=${older}>Load earlier</button><button class="sm" onClick=${dl}>Export</button>`}</div>
      <div class=${'logbox mono ' + (compact ? 'compact' : '')} ref=${box} role="log" aria-live="off">
        ${shown.length ? shown.map((r, i) => h`<div key=${r.t + ':' + i} class=${'ln ' + lineClass(r.text)}><span class="lt">${clock(r.t)}</span>${hex ? textHex(r.text) : r.text}</div>`) : h`<div class="hint">${s.raw.length ? 'No entry matches the filter.' : 'The ground station has not printed anything yet.'}</div>`}</div></div>`;
  }

  window.MCP = { Help, Card, PageHead, Led, Code, TEL, getPath, telValue, seriesOf, Tile, TelTile, HexDump, decodeText, EventCard, FileCard, ReplyView, RawLog, STATES, RAW_ONLY, jobEvents };
})();
