/* Profiler: live "task manager" of the two boards over SWD (profiler.py) */
(function () {
  const { h, api, useStore, act, log, Chip, Verdict, Kv, fmtBytes, useState, useEffect, useMemo, state, UI } = S2;
  const { Spark } = UI;
  const hex = (n, w = 8) => '0x' + n.toString(16).toUpperCase().padStart(w, '0');

  const heatColor = (heat, used) => {
    const l = 14 + used / 255 * 14;                               // filled RAM is a little lighter than empty
    if (!heat) return `hsl(215 30% ${l}%)`;
    return `hsl(${Math.max(0, 55 - heat / 255 * 55 * 1.5)} 90% ${35 + heat / 255 * 20}%)`;
  };
  const OWN = { data: '#4da3ff', bss: '#38d39f', heap: '#f7b84b', stack: '#c58bff', ipc: '#ff7a90', cpu1: '#8794b8', free: 'transparent' };

  function ownerMap(layout, which) {
    // owner of every 256-byte block of 0x20000000..0x2000FFFF for the CPU2 image
    const L = layout.find(c => c.core.startsWith('CPU2'));
    const own = new Array(256).fill('free');
    for (let b = 0; b < 256; b++) {
      const a = 0x20000000 + b * 256;
      if (a < 0x20008000) own[b] = 'cpu1';
      else if (a < 0x20009000) own[b] = 'ipc';
    }
    if (L) {
      const mark = (name, kind) => { const s = L.sections.find(x => x.name === name); if (s) for (let a = s.addr & ~255; a < s.addr + s.size; a += 256) { const b = (a - 0x20000000) >> 8; if (b >= 0 && b < 256) own[b] = kind; } };
      mark('.data', 'data'); mark('.bss', 'bss'); mark('._user_heap_stack', 'heap');
      if (L.estack && L.ebss) { const hs = L.sections.find(x => x.name === '._user_heap_stack'); const from = hs ? hs.addr + hs.size : L.ebss; for (let a = from & ~255; a < L.estack; a += 256) { const b = (a - 0x20000000) >> 8; if (b >= 0 && b < 256 && own[b] === 'free') own[b] = 'stack'; } }
    }
    return own;
  }

  function MemMap({ st, layout, syms }) {
    const [tip, setTip] = useState(null);
    const own = useMemo(() => ownerMap(layout), [layout]);
    const all = useMemo(() => (syms ? [].concat(...Object.values(syms)).sort((a, b) => a[0] - b[0]) : []), [syms]);
    const tipFor = b => {
      const a = 0x20000000 + b * 256;
      const inside = all.filter(([ad, sz]) => ad < a + 256 && ad + sz > a).slice(0, 6).map(x => x[2] + ' (' + x[1] + ' B)').join(', ');
      return `${hex(a)}–${hex(a + 255)} · ${own[b]} · changed ${Math.round(st.heat[b] / 255 * 100)} % · ${Math.round(st.used[b] / 255 * 100)} % non-zero${inside ? ' · ' + inside : ''}`;
    };
    return h`<div><div style="display:grid;grid-template-columns:repeat(32,1fr);gap:2px;max-width:760px">
      ${st.heat.map((v, b) => h`<i key=${b} title=${tipFor(b)} onMouseEnter=${() => setTip(tipFor(b))} style=${`aspect-ratio:1;border-radius:2px;background:${heatColor(v, st.used[b])};border:1.5px solid ${OWN[own[b]]}`}></i>`)}</div>
      <div class="hint mono" style="min-height:2.6em;margin-top:6px">${tip || 'hover a block (256 B each; 64 KB of SRAM)'}</div>
      <div class="row hint">${Object.entries(OWN).filter(([k]) => k !== 'free').map(([k, c]) => h`<span key=${k}><i style=${`display:inline-block;width:10px;height:10px;border:2px solid ${c};border-radius:2px;margin-right:4px`}></i>${{ data: '.data', bss: '.bss', heap: 'heap reserve', stack: 'stack', ipc: 'IPC mailboxes (shared)', cpu1: 'CPU1 SRAM1 (NuttX)' }[k]}</span>`)}
        <span>fill = changed in the last seconds (red = hot)</span></div></div>`;
  }

  function FnBars({ top }) {
    return h`<div>${top.map(t => h`<div key=${t.fn} style="display:flex;align-items:center;gap:8px;margin:2px 0"><div class="mono" style="width:190px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap" title=${t.fn}>${t.fn}</div>
      <div class="bar grow"><i style=${'width:' + t.pct + '%'}></i></div><div style="width:52px;text-align:right">${t.pct}%</div></div>`)}</div>`;
  }

  function Usage({ label, used, total, note }) {
    const pct = total ? 100 * used / total : 0;
    return h`<div style="margin:6px 0"><div class="row"><b>${label}</b><span class="grow"></span><span>${used.toLocaleString()} / ${total ? total.toLocaleString() : '?'} B ${total ? '(' + pct.toFixed(1) + ' %)' : ''}</span></div>
      <div class="bar"><i style=${'width:' + Math.min(100, pct) + '%;background:' + (pct > 90 ? 'var(--bad)' : pct > 75 ? 'var(--warn)' : 'var(--acc)')}></i></div>${note ? h`<div class="hint">${note}</div>` : ''}</div>`;
  }

  function Watches({ target, st, cores }) {
    const [core, setCore] = useState(cores[0]);
    const [q, setQ] = useState('');
    const [res, setRes] = useState([]);
    useEffect(() => { if (q.length < 2) { setRes([]); return; } const t = setTimeout(() => api(`/api/prof/symbols?target=${target}&core=${encodeURIComponent(core)}&q=${encodeURIComponent(q)}`).then(setRes).catch(() => {}), 200); return () => clearTimeout(t); }, [q, core, target]);
    const names = Object.keys(st.watch);
    return h`<div><div class="row"><select value=${core} onChange=${e => setCore(e.target.value)}>${cores.map(c => h`<option key=${c}>${c}</option>`)}</select>
      <input class="mono grow" placeholder="search a RAM symbol (app_state, g_saf, rx_frame_buffer …)" value=${q} onInput=${e => setQ(e.target.value)} /></div>
      ${res.length ? h`<div class="row" style="margin:6px 0">${res.slice(0, 14).map(r => h`<button class="sm" key=${r.name} title=${hex(r.addr) + ' ' + r.size + ' B'} onClick=${() => api('/api/prof/watch', { target, core, name: r.name }).then(() => setQ('')).catch(e => log(e.message, 'err'))}>+ ${r.name}</button>`)}</div>` : ''}
      <table><tr><th>symbol</th><th>address</th><th>value</th><th>history</th><th></th></tr>
      ${names.map(n => { const w = st.watch[n]; const v = w.value; return h`<tr key=${n}><td class="mono">${n}</td><td class="mono">${hex(w.addr)}</td>
        <td class="mono">${v ? (v.length === 1 ? `${v[0]} (0x${v[0].toString(16)})` : v.map(x => x.toString(16).padStart(w.size >= 4 ? 8 : 2, '0')).join(' ')) : '…'}${w.error ? ' ' + w.error : ''}</td>
        <td style="width:200px">${w.hist.length > 1 ? h`<${Spark} data=${w.hist} />` : ''}</td><td><button class="sm" onClick=${() => api('/api/prof/unwatch', { target, name: n })}>x</button></td></tr>`; })}</table></div>`;
  }

  function Peek({ target }) {
    const [addr, setAddr] = useState('0x20008F00'), [len, setLen] = useState(64), [out, setOut] = useState('');
    const go = () => api('/api/prof/peek', { target, addr, len }).then(r => { const b = r.hex.match(/../g) || []; const lines = []; for (let i = 0; i < b.length; i += 16) { const ch = b.slice(i, i + 16); lines.push(hex(r.addr + i) + '  ' + ch.join(' ').padEnd(47) + '  ' + ch.map(x => { const c = parseInt(x, 16); return c >= 32 && c < 127 ? String.fromCharCode(c) : '.'; }).join('')); } setOut(lines.join('\n')); }).catch(e => setOut(e.message));
    return h`<div><div class="row"><input class="mono" value=${addr} onInput=${e => setAddr(e.target.value)} style="width:140px" /><input type="number" value=${len} onInput=${e => setLen(+e.target.value)} style="width:80px" /><button onClick=${go}>Read (read-only)</button>
      <span class="hint">0x20008000 SAF mailbox · 0x20008840 flash ring · 0x20008F00 OBC handshake · 0x08000000 flash</span></div><pre class="mono" style="max-height:260px">${out}</pre></div>`;
  }

  function Profiler() {
    const s = useStore();
    const [target, setTarget] = useState('sat');
    const [info, setInfo] = useState(null);
    const [syms, setSyms] = useState(null);
    const [err, setErr] = useState('');
    const load = () => api('/api/prof').then(setInfo).catch(e => setErr(e.message));
    useEffect(() => { load(); }, []);
    useEffect(() => { api('/api/prof/ramsyms?target=' + target).then(setSyms).catch(() => {}); }, [target]);
    const st = s.prof[target];
    const running = !!st || (info && info.running.includes(target));
    const layout = info && info.layout[target];
    const toggle = () => api(running ? '/api/prof/stop' : '/api/prof/start', { target }).then(() => { setErr(''); setTimeout(load, 800); }).catch(e => setErr(e.message));
    const cores = (layout || []).map(c => c.core);
    const hist = s.profHist[target] || {};
    const stack = (core, action) => api('/api/prof/stack', { target, core, action }).then(() => setErr('')).catch(e => setErr(e.message));
    return h`<div>
      <div class="card"><div class="row">
        <div class="tabs" style="margin:0"><button class=${target === 'sat' ? 'on' : ''} onClick=${() => setTarget('sat')}>Satellite Nucleo</button><button class=${target === 'gs' ? 'on' : ''} onClick=${() => setTarget('gs')}>Ground station board</button></div>
        <button class=${running ? '' : 'pri'} onClick=${toggle}>${running ? 'Detach' : 'Attach over SWD'}</button>
        ${st && h`<${Chip} cls=${st.error ? 'warn' : 'ok'}>${st.rate} PC samples/s · SRAM dump ${st.mem_ms} ms<//>`}
        <span class="hint">probe ${info ? info.probes[target] : ''} · attaches without reset; the cores keep running. Detach before flashing.</span></div>
        ${(err || (st && st.error)) && h`<div class="err" style="margin-top:6px;color:var(--bad)">${err || st.error}</div>`}</div>
      ${!running && h`<div class="card hint">Not attached. The memory layout below comes from the ELF files; the live views need an attached ST-Link.</div>`}
      ${st && h`<div class="cards" style="grid-template-columns:repeat(auto-fit,minmax(420px,1fr))">${st.cores.map(c => h`<div class="card" key=${c.core}><h2>${c.core}</h2>
        <div class="row"><div class="stat"><b>${c.busy_pct}%</b><span>busy</span></div><div class="stat"><b>${c.sleep_pct}%</b><span>idle (WFI / idle task)</span></div><div class="stat"><b>${c.samples}</b><span>PC samples</span></div></div>
        <${Spark} data=${hist[c.core] || []} color="var(--ok)" />
        <h3>Where the CPU time goes (last 10 s)</h3><${FnBars} top=${c.top} />
        <h3>Since attach</h3><div class="hint mono" style="max-height:130px;overflow:auto">${c.lifetime.slice(0, 12).map(x => x.fn + ' ' + x.pct + '%').join('  ·  ')}</div>
        ${!c.elf && h`<div class="hint">no ELF for this core: addresses are not resolved to names</div>`}</div>`)}</div>`}
      ${st && h`<div class="card"><h2>SRAM map (0x20000000 – 0x2000FFFF)</h2><${MemMap} st=${st} layout=${layout || []} syms=${syms} /></div>`}
      ${layout && h`<div class="cards">${layout.map(L => { const fl = L.regions && L.regions.FLASH, rm = L.regions && L.regions.RAM; const hs = L.sections.find(x => x.name === '._user_heap_stack'); const stk = st && (st.cores.find(c => c.core === L.core) || {}).stack;
        return h`<div class="card" key=${L.core}><h2>${L.core} memory <small class="hint">${L.path}${L.mtime ? ', built ' + new Date(L.mtime * 1000).toLocaleString() : ''}</small></h2>
          ${!L.elf ? h`<div class="hint">ELF not found: build first.</div>` : h`
          <${Usage} label="Flash (image)" used=${L.flash_used} total=${fl ? fl[1] : 0} />
          <${Usage} label="RAM (.data + .bss + heap reserve)" used=${L.ram_used} total=${rm ? rm[1] : 0} />
          ${L.estack && h`<${Usage} label="Stack high-water mark" used=${stk && stk.used != null ? stk.used : 0} total=${stk ? stk.top - stk.lo : (hs ? L.estack - (hs.addr + hs.size) : 0)} note=${stk ? (stk.used != null ? `deepest word at ${hex(stk.deepest)}, ${stk.free} B never touched since arming` : 'armed, press Read') : 'arm the watermark first'} />
            <div class="row"><button class="sm" disabled=${!st} title="halts the core for a few ms, reads SP, paints the unused stack" onClick=${() => stack(L.core, 'arm')}>Arm watermark (halts core ~5 ms)</button><button class="sm" disabled=${!st || !stk} onClick=${() => stack(L.core, 'read')}>Read watermark</button>${stk && h`<span class="hint">SP at arming ${hex(stk.sp)}</span>`}</div>`}
          <h3>Sections</h3><table><tr><th>section</th><th>address</th><th>size</th></tr>${L.sections.map(x => h`<tr key=${x.name}><td class="mono">${x.name}</td><td class="mono">${hex(x.addr)}</td><td>${x.size.toLocaleString()} B</td></tr>`)}</table>
          <h3>Largest RAM objects</h3><table>${L.top_ram.slice(0, 10).map(x => h`<tr key=${x.name}><td class="mono">${x.name}</td><td class="mono">${hex(x.addr)}</td><td>${x.size} B</td></tr>`)}</table>`}</div>`; })}</div>`}
      ${st && h`<div class="card"><h2>Watch variables</h2><${Watches} target=${target} st=${st} cores=${cores} /></div>
        <div class="card"><h2>Memory peek</h2><${Peek} target=${target} /></div>`}
    </div>`;
  }

  Object.assign(S2, { Profiler });
})();
