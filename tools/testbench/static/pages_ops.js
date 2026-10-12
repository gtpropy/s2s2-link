/* operations pages: Dashboard, Consoles (GS + Nucleo + merged timeline), Commands */
(function () {
  const { h, api, useStore, act, log, Chip, Verdict, Kv, Terminal, fmtT, push, emit, useState, useEffect, useMemo, useRef, state } = S2;

  const echo = (k, msg, cls) => { push(k, { t: Date.now() / 1000, cls, msg }); emit(); };

  /* ------------------------------------------------------------------ link bar (serial ports) */
  function LinkBar({ kind }) {
    const st = useStore().status;
    const l = st && st[kind];
    const [sel, setSel] = useState('');
    if (!l) return null;
    const ports = l.ports || [];
    const cur = sel || l.port || ports[0] || '';
    const label = kind === 'gs' ? 'Ground station' : 'Nucleo (satellite)';
    return h`<div class="row">
      <${Chip} cls=${l.connected ? 'ok' : 'bad'}>${label}<//>
      <select value=${cur} onChange=${e => setSel(e.target.value)} disabled=${l.connected}>
        ${ports.length ? ports.map(p => h`<option key=${p}>${p}</option>`) : h`<option>none found</option>`}
      </select>
      ${l.connected
        ? h`<button onClick=${() => act('/api/' + (kind === 'gs' ? 'gs' : 'obc') + '/disconnect', {})}>Disconnect</button>`
        : h`<button class="pri" disabled=${!ports.length} onClick=${() => act('/api/' + (kind === 'gs' ? 'gs' : 'obc') + '/connect', { port: cur }, label + ' connected')}>Connect</button>`}
    </div>`;
  }

  /* ------------------------------------------------------------------ GS console */
  const gsSend = async text => {
    echo('gs', '› ' + text, 'you');
    try { await api('/api/gs/send', { text }); } catch (e) { echo('gs', e.message, 'err'); }
  };

  function Palette({ items, onPick }) {
    const groups = [];
    items.forEach(it => { const g = groups.find(x => x.name === it.group); g ? g.items.push(it) : groups.push({ name: it.group || '', items: [it] }); });
    return h`<div class="pal">${groups.map(g => h`<${React.Fragment} key=${g.name}>
      ${g.name && h`<div class="g">${g.name}</div>`}
      ${g.items.map(it => h`<button key=${it.label || it.name} class=${'sm ' + (it.danger ? 'danger' : '')} title=${(it.help || '') + '  (shift-click: edit first)'}
        onClick=${e => onPick(it, e.shiftKey)}>${it.label || it.name}</button>`)}<//>`)}</div>`;
  }

  function CmdInput({ placeholder, onSubmit, setRef, hist }) {
    const [v, setV] = useState('');
    const [i, setI] = useState(-1);
    useEffect(() => { if (setRef) setRef(setV); }, []);
    const go = () => { if (!v.trim()) return; hist.current.unshift(v); onSubmit(v.trim()); setV(''); setI(-1); };
    const key = e => {
      if (e.key === 'Enter') go();
      else if (e.key === 'ArrowUp') { const n = Math.min(i + 1, hist.current.length - 1); setI(n); if (hist.current[n] !== undefined) setV(hist.current[n]); e.preventDefault(); }
      else if (e.key === 'ArrowDown') { const n = Math.max(i - 1, -1); setI(n); setV(n < 0 ? '' : hist.current[n]); e.preventDefault(); }
    };
    return h`<div class="row" style="margin-top:6px"><input class="grow mono" placeholder=${placeholder} value=${v} onInput=${e => setV(e.target.value)} onKeyDown=${key} /><button class="pri" onClick=${go}>Send</button></div>`;
  }

  function GsConsole() {
    const s = useStore();
    const setV = useRef(null), hist = useRef([]);
    const pick = async (it, edit) => { if (it.edit || edit) setV.current && setV.current(it.send); else gsSend(it.send); };
    return h`<div class="card">
      <h2>Ground station console <small class="hint">raw UART of CPU2 on the GS: whatever you type is the GS firmware's own command line</small></h2>
      <${LinkBar} kind="gs" />
      <${Palette} items=${s.palette ? s.palette.gs : []} onPick=${pick} />
      <${Terminal} lines=${s.gs} onClear=${() => { state.gs = []; emit(); }} />
      <${CmdInput} placeholder="HK1 · PING · DIGI K1ABC>APZS2S,ARISS:>hi · HEX 53 01 ... · HELP" onSubmit=${gsSend} setRef=${f => (setV.current = f)} hist=${hist} />
    </div>`;
  }

  /* ------------------------------------------------------------------ Nucleo console */
  async function obcRun(line) {
    echo('sat', '› ' + line, 'you');
    const m = line.match(/^(\S+)\s*(.*)$/);
    const name = m[1].toUpperCase(), rest = m[2];
    try {
      if (name === 'RAW') {
        const r = await api('/api/obc/raw', { hex: rest });
        echo('sat', `sent ${r.sent} bytes; answers: ${r.acks.length ? r.acks.map(a => `cmd ${a.cmd} seq ${a.seq} status 0x${a.status.toString(16)}`).join(', ') : 'none'}`, 'res');
        r.lines.forEach(l => echo('sat', l, ''));
        return;
      }
      const pal = (state.palette ? state.palette.obc : []).find(p => p.name === name.replace(/\s+/g, ' '));
      const args = {};
      const parts = rest.split(/\s+/).filter(Boolean);
      if (name === 'INJECT' || name === 'SETNAME') args.text = rest;
      else (pal && pal.args || []).forEach((a, i) => { if (parts[i] !== undefined) args[a] = parseInt(parts[i], parts[i].startsWith('0x') ? 16 : 10); });
      if (name === 'SAF' && parts[0] === 'clear') { const r = await api('/api/obc/cmd', { name: 'SAF clear', args: {} }); echo('sat', r.meaning, 'res'); return; }
      const r = await api('/api/obc/cmd', { name, args });
      echo('sat', `${name}: ${r.meaning}${r.status != null ? ' (status 0x' + r.status.toString(16).toUpperCase() + ')' : ''}`, r.status === 0 ? 'ok' : 'res');
      if (Array.isArray(r.data)) r.data.forEach(o => echo('sat', `  #${o.n} id=${o.id} len=${o.len} hops_left=${o.hops_left} fwd=${o.fwd_count} ttl=${o.ttl_s}s  ${o.text}`, 'res'));
      else if (r.data) echo('sat', '  ' + r.data, 'res');
    } catch (e) { echo('sat', e.message, 'err'); }
  }

  function NucleoConsole() {
    const s = useStore();
    const setV = useRef(null), hist = useRef([]);
    const pick = (it, edit) => {
      const t = it.name + ((it.args || []).length ? ' ' + (it.args || []).map(a => (it.default && it.default[a]) || (it.seq !== undefined && a === 'seq' ? it.seq : '')).join(' ') : '');
      if ((it.args && it.args.length) || edit) setV.current && setV.current(t.trim() + ' '); else obcRun(t);
    };
    return h`<div class="card">
      <h2>Nucleo console <small class="hint">CPU1 (NuttX) answers OBC frames on the ST-Link port; its text log (and CPU2 relayed logs) appears below</small></h2>
      <${LinkBar} kind="obc" />
      <${Palette} items=${s.palette ? s.palette.obc.map(o => ({ ...o, label: o.name, group: o.danger ? 'caution' : (o.args && o.args.length ? 'with arguments' : 'no arguments') })) : []} onPick=${pick} />
      <${Terminal} lines=${s.sat} onClear=${() => { state.sat = []; emit(); }} />
      <${CmdInput} placeholder="STATUS · INFO · SAF 0 · INJECT K1ABC>APZS2S,ARISS:>hi · READF 2 · RAW c5c5c5c5c5c5c5c5a7 ..." onSubmit=${obcRun} setRef=${f => (setV.current = f)} hist=${hist} />
    </div>`;
  }

  /* ------------------------------------------------------------------ digipeater quick flow (GS -> satellite -> repeat) */
  function DigiFlow() {
    const [txt, setTxt] = useState('K1ABC>APZS2S,ARISS:>hello via the ground station');
    const [recs, setRecs] = useState(null);
    const showStore = async () => {
      echo('sat', '› SAF 0', 'you');
      try { const r = await api('/api/digi/store', {}); setRecs(r); echo('sat', `${r.journal || 'no answer'} — ${r.stored} stored, ${r.bytes} B`, 'res'); }
      catch (e) { echo('sat', e.message, 'err'); }
    };
    return h`<div class="card">
      <h2>Digipeat from the ground station</h2>
      <div class="row">
        <input class="grow mono" value=${txt} onInput=${e => setTxt(e.target.value)} />
        <button class="pri" title="GS builds a UI frame and transmits it on 437.375 MHz; the satellite digipeats it on 435.000 MHz" onClick=${() => gsSend('DIGI ' + txt)}>GS → satellite (over the air)</button>
        <button title="hands the frame to the satellite's CPU2 through CPU1 (no radio uplink)" onClick=${() => obcRun('INJECT ' + txt)}>Inject through Nucleo</button>
        <button onClick=${showStore}>Show stored messages</button>
      </div>
      <p class="hint">Watch the GS terminal for the repeated frame and the Nucleo terminal for the digipeater answer. The satellite listens only between CW beacons (about 50 s of 90 s); resend if it is silent.</p>
      ${recs && h`<table><tr><th>#</th><th>id</th><th>len</th><th>hops left</th><th>forwarded</th><th>ttl</th><th>frame</th></tr>
        ${recs.objects.map(o => h`<tr key=${o.n}><td>${o.n}</td><td>${o.id}</td><td>${o.len}</td><td>${o.hops_left}</td><td>${o.fwd_count}</td><td>${o.ttl_s}s</td><td class="mono">${o.text}</td></tr>`)}</table>`}
    </div>`;
  }

  /* ------------------------------------------------------------------ merged timeline */
  function Timeline() {
    const s = useStore();
    const [src, setSrc] = useState({ gs: true, sat: true, rf: true, evt: true, bench: true });
    const lines = useMemo(() => {
      const out = [];
      if (src.gs) s.gs.forEach(l => out.push({ t: l.t, cls: 'gs', msg: '[GS]  ' + l.msg }));
      if (src.sat) s.sat.forEach(l => out.push({ t: l.t, cls: 'sat', msg: '[SAT] ' + l.msg }));
      if (src.rf) s.sdr.forEach(l => out.push({ t: l.t, cls: 'rf', msg: '[RF]  ' + l.msg }));
      if (src.evt) s.evt.forEach(e => out.push({ t: e.t, cls: e.kind === 'nack' ? 'nack' : e.kind === 'ack' ? 'ack' : 'warn', msg: '[EVT] ' + describe(e) }));
      if (src.bench) s.bench.forEach(l => out.push({ t: l.t, cls: l.cls, msg: '[BENCH] ' + l.msg }));
      return out.sort((a, b) => a.t - b.t).slice(-900);
    }, [s.tick, src]);
    return h`<div class="card"><h2>Timeline <small class="hint">everything on one clock: GS UART, Nucleo VCP, SDR/Direwolf, decoded events, bench log</small></h2>
      <div class="row">${Object.keys(src).map(k => h`<label key=${k}><input type="checkbox" checked=${src[k]} onChange=${e => setSrc({ ...src, [k]: e.target.checked })} /> ${k}</label>`)}</div>
      <${Terminal} lines=${lines} height=${300} /></div>`;
  }

  function describe(e) {
    switch (e.kind) {
      case 'ack': return `ACK ${e.what} (mission ${e.mission_name || e.mission})`;
      case 'nack': return `NACK 0x${(e.err || 0).toString(16)} ${e.name} (mission ${e.mission_name || e.mission})`;
      case 'beacon': return `CW beacon ${e.hex}  bat ${e.b1.bat_v} V ${e.b1.system}`;
      case 'cw': return 'CW ' + e.text;
      case 'hk': return 'housekeeping packet';
      case 'rssi': return `packet #${e.n} ${e.rssi} dBm SNR ${e.snr} ${e.bytes} B`;
      case 'hk': return `housekeeping seq ${e.seq}: battery ${e.bat_v} V, bat ${e.bat_t} °C, ${e.crc_ok ? 'footers ok' : 'footers BAD'}`;
      case 'frame': return `${e.src} frame: ${e.text}`;
      case 'command': return `command ${e.id}: ${e.text} (try ${e.attempt})`;
      default: return e.kind + ' ' + JSON.stringify(e).slice(0, 160);
    }
  }

  function Consoles() {
    return h`<div>
      <${DigiFlow} />
      <div class="cards" style="grid-template-columns:repeat(auto-fit,minmax(400px,1fr))"><${GsConsole} /><${NucleoConsole} /></div>
      <${Timeline} />
    </div>`;
  }

  /* ------------------------------------------------------------------ Commands (sheet commands with ACK / NACK verdicts) */
  const jobVerdict = job => {
    if (job.state === 'running') return 'running';
    const v = job.result && job.result.verdict;
    if (v === 'completed' || v === 'sent') return 'PASS';
    if (v === 'NACK') return 'NACK';
    return v && v.startsWith('accepted') ? 'PARTIAL' : 'FAIL';
  };

  function Commands() {
    const s = useStore();
    const [args, setArgs] = useState({});
    const [last, setLast] = useState({});
    const cmds = s.palette ? s.palette.commands : [];
    const groups = [...new Set(cmds.map(c => c.group))];
    const run = async c => {
      const a = {};
      (c.args || []).forEach(x => { a[x.name] = (args[c.id] || {})[x.name] ?? x.default; });
      if (c.critical && !confirm(`${c.label}: ${c.help}\nSend it?`)) return;
      try { const r = await api('/api/cmd', { id: c.id, args: a }); setLast({ ...last, [c.id]: r.job }); } catch (e) { log(e.message, 'err'); }
    };
    return h`<div>
      <p class="hint">Each command is sent through the ground station, repeated until the satellite answers, and judged on the ACK 0xAC / NACK 0xEE frames the GS decodes. The result lists every event it caused.</p>
      ${groups.map(g => h`<div class="card" key=${g}><h2>${g}</h2>
        ${cmds.filter(c => c.group === g).map(c => {
          const job = s.jobs[last[c.id]];
          return h`<div key=${c.id} style="padding:8px 0;border-top:1px solid var(--line)">
            <div class="row">
              <button class=${c.critical ? 'danger' : 'pri'} disabled=${job && job.state === 'running'} onClick=${() => run(c)}>${c.label}</button>
              ${(c.args || []).map(x => h`<input key=${x.name} class="mono" style="width:${x.name === 'text' ? 380 : 90}px" title=${x.label} placeholder=${x.label} value=${(args[c.id] || {})[x.name] ?? x.default}
                onInput=${e => setArgs({ ...args, [c.id]: { ...(args[c.id] || {}), [x.name]: e.target.value } })} />`)}
              <span class="mono hint">${c.gs}${c.opcode ? '  ·  opcode ' + c.opcode : ''}</span>
              ${job && h`<${Verdict} v=${jobVerdict(job)} />`}
            </div>
            <div class="hint">${c.help}</div>
            ${job && job.result && h`<div class="hint mono">${job.result.verdict}${job.result.seconds != null ? ` in ${job.result.seconds} s, ${job.result.attempts} try` : ''}${job.result.error ? ' — ' + job.result.error : ''}
              ${(job.result.events || []).slice(-8).map((e, i) => h`<div key=${i}>${fmtT(e.t)}  ${describe(e)}</div>`)}</div>`}
          </div>`;
        })}</div>`)}
    </div>`;
  }

  /* ------------------------------------------------------------------ Dashboard */
  function Dashboard({ go }) {
    const s = useStore();
    const st = s.status, live = s.live;
    const latest = (live && live.latest) || {};
    const counts = (live && live.counts) || {};
    const im = s.images[0];
    const mon = s.monitor || {};
    const lastBeacon = s.evt.slice().reverse().find(e => e.kind === 'beacon') || latest.beacon;
    const lastAck = s.evt.slice().reverse().find(e => e.kind === 'ack' || e.kind === 'nack') || latest.ack;
    const rssi = s.evt.slice().reverse().find(e => e.kind === 'rssi');
    return h`<div>
      <div class="row" style="margin-bottom:12px">
        ${[['Commands acked', counts.ack], ['NACKs', counts.nack], ['Beacons', counts.beacon], ['Housekeeping', counts.hk], ['Frames (SDR)', counts.frame]].map(([k, v]) =>
          h`<div class="stat" key=${k}><b>${v == null ? '–' : v}</b><span>${k}</span></div>`)}
        <div class="stat"><b>${rssi ? rssi.rssi + ' dBm' : '–'}</b><span>last RSSI</span></div>
        <div class="stat"><b>${s.running || 'idle'}</b><span>running test</span></div>
      </div>
      <div class="cards">
        <div class="card"><h2>Link</h2>
          <${LinkBar} kind="gs" /><div style="height:8px"></div><${LinkBar} kind="obc" />
          <div style="height:8px"></div>
          <div class="row"><${Chip} cls=${mon.on ? 'ok' : (st && st.sdr.present ? 'warn' : 'bad')}>${mon.on ? `SDR monitor ${mon.freq} MHz` : (st && st.sdr.present ? 'SDR present, monitor off' : 'no SDR')}<//>
            <button onClick=${() => act('/api/monitor', { on: !mon.on, freq: mon.freq, gain: mon.gain }, mon.on ? 'monitor off' : 'monitor on')}>${mon.on ? 'Stop monitor' : 'Start monitor'}</button></div>
          <p class="hint">The monitor decodes the downlink with Direwolf all the time; camera fragments go straight into the image store.</p>
        </div>
        <div class="card"><h2>Last answer from the satellite</h2>
          ${lastAck ? h`<${Kv} rows=${[['event', describe(lastAck)], ['when', fmtT(lastAck.t)]]} />` : h`<p class="hint">nothing yet — send HEALTH SAT</p>`}
          <div class="row" style="margin-top:8px"><button class="pri" onClick=${() => api('/api/cmd', { id: 'health', args: {} }).then(() => go('commands')).catch(e => log(e.message, 'err'))}>HEALTH SAT</button>
            <button onClick=${() => go('commands')}>All commands</button><button onClick=${() => go('consoles')}>Consoles</button></div>
        </div>
        <div class="card"><h2>CW beacon</h2>
          ${lastBeacon ? h`<${Kv} rows=${[['bytes', h`<span class="mono">${lastBeacon.hex}</span>`], ['system', lastBeacon.b1.system], ['battery', `${lastBeacon.b1.bat_v} V / ${lastBeacon.b1.bat_i} A`],
            ['solar', `${lastBeacon.b1.sol_total_v} (V) ${lastBeacon.b1.sol_total_i} (I)`], ['mission', lastBeacon.b1.mission], ['when', fmtT(lastBeacon.t)]]} />`
            : h`<p class="hint">no beacon decoded yet (the satellite keys one every 90 s)</p>`}
        </div>
        <div class="card"><h2>Newest image <small class="hint">built in the background while fragments arrive</small></h2>
          ${im ? h`<div class="big"><img src=${`/api/images/${im.id}/image?v=${im.got}`} /></div>
            <div class="bar" style="margin:6px 0"><i style=${'width:' + (100 * im.got / im.total) + '%'}></i></div>
            <${Kv} rows=${[['session', im.id], ['fragments', `${im.got} / ${im.total}`], ['state', im.complete ? h`<${Verdict} v=${im.matches_sent === false ? 'FAIL' : 'PASS'} /> complete${im.matches_sent ? ', identical to the file sent' : ''}` : 'receiving'], ['sources', JSON.stringify(im.sources)]]} />
            <button class="sm" onClick=${() => go('images')}>Open gallery</button>`
            : h`<p class="hint">no fragments received yet</p>`}
        </div>
      </div>
      <div class="card"><h2>Recent events</h2>
        <${Terminal} lines=${s.evt.filter(e => e.kind !== 'rssi' || true).map(e => ({ t: e.t, cls: e.kind === 'nack' ? 'nack' : e.kind === 'ack' ? 'ack' : '', msg: describe(e) }))} height=${220} />
      </div>
    </div>`;
  }

  Object.assign(S2, { Dashboard, Consoles, Commands, describe, LinkBar });
})();
