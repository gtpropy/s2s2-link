/* DEBUG: section: CUBUS debug (why does the satellite not receive / hand off a command?) and CUBUS pins (who owns which pin). Everything reads or acts only on a button press. */
(function () {
  const { h, api, useStore, log, Chip, Verdict, Kv, Terminal, useState, useEffect, useRef, UI } = S2;
  const stopTest = () => api('/api/test/stop', {}).catch(e => log(e.message, 'err'));

  const ICON = { ok: ['✔', 'var(--ok)'], info: ['ℹ', 'var(--acc)'], warn: ['⚠', 'var(--warn)'], bad: ['✘', 'var(--bad)'] };
  const hex = (v, n = 8) => (v == null ? '–' : '0x' + (v >>> 0).toString(16).toUpperCase().padStart(n, '0'));
  const dd = v => (v == null ? '–' : v);

  const Finding = ({ f }) => {
    const [ic, col] = ICON[f.level] || ICON.info;
    return h`<div style="display:grid;grid-template-columns:26px 1fr;gap:8px;padding:8px 0;border-top:1px solid var(--line)">
      <span style=${'font-size:18px;color:' + col}>${ic}</span>
      <div><b>${f.title}</b> <small class="hint">${f.stage || ''}</small><div class="hint" style="margin:2px 0 0;font-size:13px;color:var(--fg)">${f.text}</div></div></div>`;
  };

  /* receive path as a funnel: every stage a packet has to pass */
  function Funnel({ rx }) {
    if (!rx || !rx.valid) return h`<p class="hint">no receive counters (flash a current build)</p>`;
    const rows = [['radio delivered', rx.rx_done, ''], ['decoded (G3RUH + HDLC)', rx.decode_ok, rx.decode_fail ? rx.decode_fail + ' failed' : ''],
      ['accepted as a command', rx.accepted, [rx.validate_fail && rx.validate_fail + ' bad FCS', rx.auth_fail && rx.auth_fail + ' AUTH', rx.not_tc && rx.not_tc + ' not 0x53', rx.own_echo && rx.own_echo + ' echo', rx.digi && rx.digi + ' digipeater'].filter(Boolean).join(' · ')],
      ['written to the OBC ring', rx.ring_ok, rx.ring_fail ? rx.ring_fail + ' NOT written' : '']];
    const mx = Math.max(1, ...rows.map(r => r[1]));
    return h`<div>${rows.map(([t, n, note], i) => h`<div key=${i} style="margin:5px 0"><div class="row"><span style="min-width:190px">${t}</span><b>${n}</b><span class="hint">${note}</span></div>
      <div class="bar"><i style=${'width:' + (100 * n / mx) + '%'}></i></div></div>`)}
      ${(rx.rx_timeout || rx.rx_error || rx.bad_len || rx.incomplete) ? h`<p class="hint">radio: ${rx.rx_timeout} RX timeouts · ${rx.rx_error} RX errors · ${rx.bad_len} bad lengths · ${rx.incomplete} incomplete commands</p>` : null}
      <p class="hint">last accepted command: <span class="mono">${rx.last_cmd || '–'}</span></p></div>`;
  }

  const pin = (label, p) => [label, p ? `${p.pin}: ${p.output ? (p.high ? 'high' : 'low') : 'not an output'}` : '–'];

  function CubusDebug() {
    const s = useStore();
    const st = s.status;
    const [info, setInfo] = useState(null);
    const [rep, setRep] = useState(null);
    const [err, setErr] = useState('');
    const [busy, setBusy] = useState(false);
    const [live, setLive] = useState(false);
    const [o, setO] = useState({ command: 'HK1', seconds: 100, every_s: 8 });
    const [port, setPort] = useState('');
    const [pins, setPins] = useState(null);
    const busyRef = useRef(false);
    const loadInfo = () => api('/api/cubus/status').then(setInfo).catch(e => setErr(e.message));
    useEffect(() => { loadInfo(); }, []);
    const read = () => {
      if (busyRef.current) return Promise.resolve();
      busyRef.current = true; setBusy(true); setErr('');
      return api('/api/cubus/snapshot', {}).then(r => { setRep(r); loadInfo(); }).catch(e => setErr(e.message)).finally(() => { busyRef.current = false; setBusy(false); });
    };
    useEffect(() => { if (!live) return undefined; read(); const t = setInterval(read, 3000); return () => clearInterval(t); }, [live]);
    const prof = (rep && rep.profile) || (info && info.profile);
    const running = s.running === 'cubusrx';
    const res = s.results.cubusrx;
    const snap = rep && rep.snapshot;
    const obc = st && st.obc;
    const ports = (obc && obc.ports) || [];
    const curPort = port || (obc && obc.port) || ports[0] || '';
    const runTest = () => api('/api/test/start', { name: 'cubusrx', ...o }).catch(e => setErr(e.message));
    return h`<div>
      <div class="card"><h2>CUBUS debug <small class="hint">find out WHERE an uplink command is lost: radio, decode, validation or the hand-off to the OBC</small></h2>
        ${prof && h`<${Kv} rows=${[
          ['profile in .s2s2.config', h`<b>${prof.cubus ? 'RSMS CUBUS V.3 (CUBUS=T)' : 'Nucleo / custom front end (CUBUS=F)'}</b> · expects image id ${hex(prof.expect_id)} (${prof.expect_name})`],
          ['last satellite build', h`<span class="mono">${prof.build_opts || 'none yet'}</span> ${prof.build_opts && (prof.build_is_cubus === prof.cubus ? h`<${Chip} cls="ok">matches<//>` : h`<${Chip} cls="warn">build and profile differ: rebuild<//>`)}`],
          ['SWD (ST-Link of the satellite board)', info && info.swd_open ? 'connection open' : 'closed (opens on the first snapshot, closes after 2 idle minutes, when the profiler starts or when you flash)'],
          ['bench app link', prof.cubus ? 'not available on CUBUS: CPU1 runs the OBC firmware. Tests that need it are blocked (list below).' : 'the Nucleo VCP link works as before']]} />`}
        ${err && h`<p class="hint" style="color:var(--bad)">${err}</p>`}
        <div class="row" style="margin-top:8px">
          <button class="pri" disabled=${busy || !!s.running} onClick=${read}>${busy ? 'reading …' : 'Read the satellite now (SWD)'}</button>
          <label><input type="checkbox" checked=${live} disabled=${!!s.running} onChange=${e => setLive(e.target.checked)} /> live, every 3 s</label>
          <button class="sm" onClick=${() => api('/api/cubus/swd/close', {}).then(loadInfo)}>Release the probe</button>
          <span class="hint">Reads memory only. Nothing is written to the board.</span></div></div>

      <div class="card"><h2>Why is no command received?</h2>
        ${!rep ? h`<p class="hint">Press "Read the satellite now". The page reads CPU2's state, the receive counters, the RF switch and PA pins, and the OBC command ring, and explains what it finds.</p>`
        : h`<div>${rep.findings.map((f, i) => h`<${Finding} key=${i} f=${f} />`)}</div>`}</div>

      <div class="cards">
        <div class="card"><h2>Receive path <small class="hint">counters of g_satRx since boot</small></h2><${Funnel} rx=${snap && snap.rx} /></div>
        <div class="card"><h2>State</h2>${snap ? h`<${Kv} rows=${[
          ['image', `${hex(snap.board_id)} ${snap.board_id != null ? '' : '(no g_boardId)'}`],
          ['CPU2', snap.alive == null ? 'unknown' : snap.alive ? 'running' : 'NOT running'],
          ['state machine', snap.state.name], ['uptime', dd(snap.state.uptime_s) + ' s'], ['since beacon start', dd(snap.state.since_beacon_s) + ' s'],
          ['command session / DC-DC hold', `${snap.state.cmd_service_active} / ${snap.state.dcdc_hold}`], ['last RSSI / SNR', `${dd(snap.radio.rssi)} dBm / ${dd(snap.radio.snr)} dB`],
          ['TX busy / rx_done flag', `${dd(snap.radio.tx_busy)} / ${dd(snap.radio.rx_done_flag)}`],
          ['self-tests', `integrity ${hex(snap.selftests.seal)} · digipeater ${hex(snap.selftests.digi)}`]]} />` : h`<p class="hint">–</p>`}</div>
        <div class="card"><h2>RF switch and PA pins</h2>${snap && snap.gpio ? h`<${Kv} rows=${[
          ['RF switch (FE_CTRL1 / 2)', h`<b>${snap.gpio.rf_switch}</b> (PC4 ${+snap.gpio.fe_ctrl1.high}, PC5 ${+snap.gpio.fe_ctrl2.high}); receive needs PC4 1, PC5 0`],
          pin('3.3 V PA supply', snap.gpio.pa_3v3), pin('5 V PA supply', snap.gpio.pa_5v), pin('bus 5 V rail', snap.gpio.rail_5v)]} />` : h`<p class="hint">–</p>`}</div>
        <div class="card"><h2>OBC side (shared RAM)</h2>${snap ? h`<${Kv} rows=${[
          ['command ring (M0+ → M4)', snap.ring_rx ? `magic ${hex(snap.ring_rx.magic)} ${snap.ring_rx.magic === 0x52585242 ? '(set by the M4)' : '(NOT set: the M4 never initialised it)'} · write ${snap.ring_rx.write}, read ${snap.ring_rx.read}, pending ${snap.ring_rx.pending}` : '–'],
          ['telemetry ring (M4 → M0+)', snap.ring_tx ? `magic ${hex(snap.ring_tx.magic)} ${snap.ring_tx.magic === 0x54585242 ? '(set by the M4)' : '(not set)'}` : '–'],
          ['flash ring (M4 → M0+)', snap.ring_flash ? `magic ${hex(snap.ring_flash.magic)} · write ${snap.ring_flash.write}, read ${snap.ring_flash.read}` : '–'],
          ['OBC block', snap.obc ? (snap.obc.magic === 0x4F424331 ? `bench app block: heartbeat ${snap.obc.cpu2_beat}` : 'no bench-app block (normal on CUBUS)') : '–']]} />` : h`<p class="hint">–</p>`}</div>
      </div>

      <div class="card"><h2>Receive test <small class="hint">sends a command from the ground station for a while and compares the satellite's counters before and after</small></h2>
        <div class="row"><label>command</label><input class="mono" style="width:110px" value=${o.command} onInput=${e => setO({ ...o, command: e.target.value })} />
          <label>seconds</label><input type="number" style="width:80px" value=${o.seconds} onInput=${e => setO({ ...o, seconds: +e.target.value })} />
          <label>every s</label><input type="number" style="width:70px" value=${o.every_s} onInput=${e => setO({ ...o, every_s: +e.target.value })} />
          <button class="pri" disabled=${!!s.running || !(st && st.gs.connected)} onClick=${runTest}>Run</button>${running && h`<button onClick=${stopTest}>Stop</button>`}
          <${Verdict} v=${res && res.verdict} /></div>
        <p class="hint">Needs the ground station UART (Consoles page) and the satellite's ST-Link. One satellite cycle is 90 s and it only listens about 50 s of it, so use at least 100 s.
          Pass = commands were accepted and handed to the OBC; the rows say at which stage they stopped if not.</p>
        ${res && res.rows ? h`<${S2.RowsView} r=${res} />` : null}</div>

      <div class="cards">
        <div class="card"><h2>Satellite debug UART</h2>
          <p class="hint">${prof && prof.cubus ? 'PA9 / PA10 on the ACCESS connector (DEBUG_TX_COM/OBC, DEBUG_RX_COM/OBC) through a USB serial adapter.' : 'The Nucleo VCP.'} Text only; the stage counts above also come from the log lines.</p>
          <div class="row"><${Chip} cls=${obc && obc.connected ? 'ok' : 'bad'}>debug UART<//>
            <select value=${curPort} onChange=${e => setPort(e.target.value)} disabled=${obc && obc.connected}>${ports.length ? ports.map(p => h`<option key=${p}>${p}</option>`) : h`<option>none found</option>`}</select>
            ${obc && obc.connected ? h`<button onClick=${() => api('/api/obc/disconnect', {}).then(() => S2.refresh(false))}>Disconnect</button>`
              : h`<button class="pri" disabled=${!ports.length} onClick=${() => api('/api/obc/connect', { port: curPort }).then(() => S2.refresh(false)).catch(e => setErr(e.message))}>Connect</button>`}</div>
          ${rep && rep.uart ? h`<${Kv} rows=${Object.entries(rep.uart.counts).filter(([, v]) => v).map(([k, v]) => [k.replace(/_/g, ' '), v])} />` : null}
          <${Terminal} lines=${s.sat} height=${260} /></div>
        <div class="card"><h2>Pin ownership check</h2>
          <p class="hint">Static check of the satellite sources and a CUBUS image build: only PA0, PC2..PC5 and PA9/PA10 may be touched (the kill switch, SPI chip selects, watchdog and power enables belong to CPU1). Takes about a minute; no hardware.</p>
          <div class="row"><button disabled=${pins === 'running'} onClick=${() => { setPins('running'); api('/api/cubus/pins', {}).then(setPins).catch(e => setPins({ ok: false, output: e.message })); }}>Run the check</button>
            ${pins && pins !== 'running' && h`<${Verdict} v=${pins.ok ? 'PASS' : 'FAIL'} />`}${pins === 'running' && h`<span class="hint">building a CUBUS image …</span>`}</div>
          ${pins && pins !== 'running' && h`<pre style="max-height:200px;overflow:auto">${pins.output}</pre>`}</div>
        ${info && prof && prof.cubus && h`<div class="card"><h2>Blocked with the CUBUS profile</h2><p class="hint">These tests need the s2s2obc bench app on CPU1 (Nucleo only). They refuse to start:</p>
          <table>${Object.entries(info.blocked).map(([k, v]) => h`<tr key=${k}><td class="mono">${k}</td><td class="hint">${v}</td></tr>`)}</table>
          <p class="hint">Still available: ground-station tests (cmdconf, nackmatrix, beacontime, winmap, soak, link), SWD tests (fwcheck, bootcfg, membudget, profcap) and the profiler.</p></div>`}
      </div>
    </div>`;
  }

  /* who owns which pin of the STM32WL55 on the CUBUS board (sheet 4 of the schematic); live state of the CPU2 pins from the last snapshot */
  const PINS = [
    ['PA0', 'EN_5V_DC/DC&OCP', 'CPU2 (radio)', 'enable of the bus 5 V DC/DC and its 2 A over-current switch; raised for GMSK'],
    ['PC2', 'AMP_3V3EN', 'CPU2 (radio)', '3.3 V into the PA supply (CW beacon)'], ['PC3', 'AMP_5VEN', 'CPU2 (radio)', '5 V into the PA supply (GMSK); never together with PC2'],
    ['PC4', 'FE_CTRL1', 'CPU2 (radio)', 'RF switch VEN: RX = 1'], ['PC5', 'FE_CTRL2', 'CPU2 (radio)', 'RF switch VCTL: TX = 1'],
    ['PA9', 'DEBUG_TX_COM/OBC', 'CPU2 (radio)', 'USART1 debug console, ACCESS connector'], ['PA10', 'DEBUG_RX_COM/OBC', 'CPU2 (radio)', 'USART1 debug console'],
    ['PB0', 'VDD_TCXO', 'radio', 'not connected on CUBUS: the TCXO is powered from COM_3V3'],
    ['PA1', 'ADC2_CS', 'CPU1', 'chip select of ADC 2'], ['PA2 / PA3', 'OBC_TX_2 / OBC_RX_2', 'CPU1', 'LPUART1 to the OBC connector'], ['PA4..PA7', 'SMSN_FM_CS / SCK / MISO / MOSI', 'CPU1', 'shared mission flash (SPI1)'],
    ['PA8', 'EN_UNREG', 'CPU1', 'unregulated rail enable'], ['PA11', 'EN_GLOBAL_RESET', 'CPU1', 'global reset enable'], ['PA12', 'EN_3V3_MSN_DCDC&OCP', 'CPU1', 'mission 3.3 V rail'],
    ['PA13 / PA14', 'SWDIO / SWCLK', 'debug', 'SWD of this board'], ['PA15', 'BURNER_SIGNAL', 'CPU1', 'antenna burner'],
    ['PB1..PB5', 'KILL_SWITCH2 +/-/STAT, KILL_SWITCH_EN, KILL_SWITCH1 +', 'CPU1', 'kill switch'], ['PB6', 'KILL_SWITCH1_-', 'CPU1', 'kill switch coil line (never a UART pin on this board)'],
    ['PB7', 'KILL_SWITCH1_STAT', 'CPU1', 'kill switch status'], ['PB8', 'WD_WDI_OBC', 'CPU1', 'watchdog input; nobody in the reference feeds it'],
    ['PB9', 'CS_MPU', 'CPU1', 'gyro chip select (a LED on the Nucleo)'], ['PB10', 'ADC_SCK', 'CPU1', 'SPI2 clock'], ['PB11', 'CS_MAG', 'CPU1', 'magnetometer chip select (a LED on the Nucleo)'],
    ['PB12', 'ADC1_CS', 'CPU1', 'chip select of ADC 1'], ['PB13', 'KILL_SWITCH_ON', 'CPU1', 'kill switch'], ['PB14 / PB15', 'ADC_MISO / ADC_MOSI', 'CPU1', 'SPI2 data (PB15 is a LED on the Nucleo)'],
    ['PC0 / PC1', 'OBC_RX_1 / OBC_TX_1', 'CPU1', 'LPUART1'], ['PC6 / PC13', 'GPIO2 / GPIO1', 'CPU1', 'general purpose'],
  ];

  function CubusPins() {
    const [rep, setRep] = useState(null);
    const [err, setErr] = useState('');
    const snap = rep && rep.snapshot;
    const live = k => { const g = snap && snap.gpio; if (!g) return ''; const m = { PA0: g.rail_5v, PC2: g.pa_3v3, PC3: g.pa_5v, PC4: g.fe_ctrl1, PC5: g.fe_ctrl2 }[k]; return m ? (m.output ? (m.high ? 'high' : 'low') : 'not output') : ''; };
    return h`<div>
      <div class="card"><h2>CUBUS pins <small class="hint">RSMS CUBUS V.3, sheet 4 "COM MCU". The radio image may touch the CPU2 rows only.</small></h2>
        <div class="row"><button disabled=${false} onClick=${() => api('/api/cubus/snapshot', {}).then(setRep).catch(e => setErr(e.message))}>Read the CPU2 pins now (SWD)</button>
          ${err && h`<span class="hint" style="color:var(--bad)">${err}</span>`}${snap && snap.gpio && h`<${Chip} cls="ok">RF switch ${snap.gpio.rf_switch}<//>`}</div>
        <table style="margin-top:8px"><tr><th>pin</th><th>net</th><th>owner</th><th>function</th><th>live</th></tr>
          ${PINS.map(([p, n, o, f]) => h`<tr key=${p} style=${o.startsWith('CPU2') ? 'background:var(--panel2)' : ''}><td class="mono">${p}</td><td class="mono" style="font-size:12px">${n}</td><td>${o}</td><td class="hint">${f}</td><td>${live(p)}</td></tr>`)}</table>
        <p class="hint">The assignment was read from the schematic PDF's text layer (every net label sits at the position of its pin name). Rules: docs/CUBUS.md; the automatic check is "Pin ownership check" on the CUBUS debug page.</p></div></div>`;
  }

  Object.assign(S2, { CubusDebug, CubusPins });
})();
