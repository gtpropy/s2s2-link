/* Dashboard: housekeeping and beacon data in dashboard form, next to the debug state of both boards */
(function () {
  const { h, api, useStore, log, Chip, Kv, fmtT, useState, useEffect, UI, describe } = S2;
  const { Metric, Spark, Cube, age, fx } = UI;
  const okEv = e => (e.kind === 'hk' ? !!e.mag : true);                   // old history rows (before the HK line was decoded) have no fields
  const lastOf = (a, k) => { for (let i = a.length - 1; i >= 0; i--) if (a[i].kind === k && okEv(a[i])) return a[i]; return null; };
  const col = (a, k, f, n = 120) => a.filter(e => e.kind === k && okEv(e)).slice(-n).map(f).filter(v => v != null && !Number.isNaN(v));
  const send = line => api('/api/gs/send', { text: line }).then(() => log('GS <- ' + line)).catch(e => log(e.message, 'err'));

  function Dashboard({ go }) {
    const s = useStore();
    const [hist, setHist] = useState(false);
    useEffect(() => { api('/api/history?kind=hk&limit=200').then(r => { r.forEach(e => { if (!s.evt.some(x => x.t === e.t && x.kind === e.kind)) s.evt.unshift(e); }); setHist(true); }).catch(() => {}); }, []);
    const hk = lastOf(s.evt, 'hk'), bc = lastOf(s.evt, 'beacon'), rs = s.evt.filter(e => e.kind === 'rssi').slice(-120);
    const mag = hk && hk.mag.some(v => v) ? hk.mag : (bc && bc.b2.magneto.some(v => v) ? bc.b2.magneto : null);
    const st = s.status, live = s.live || {};
    const counts = live.counts || {};
    const prof = Object.values(s.prof)[0];
    return h`<div>
      <div class="row" style="margin-bottom:12px">
        <${Chip} cls=${st && st.gs.connected ? 'ok' : 'bad'}>Ground station UART<//><${Chip} cls=${st && st.obc.connected ? 'ok' : 'bad'}>Nucleo VCP<//>
        <${Chip} cls=${prof ? 'ok' : ''}>Profiler ${prof ? 'attached' : 'detached'}<//><${Chip} cls=${s.running ? 'run' : 'ok'}>${s.running ? 'test: ' + s.running : 'no test running'}<//>
        <span class="grow"></span><button class="pri" onClick=${() => send('HK1')}>Request HK1</button><button onClick=${() => send('HK2')}>HK2</button><button onClick=${() => send('PING')}>Ping</button>
        <button onClick=${() => go('consoles')}>Consoles</button><button onClick=${() => go('profiler')}>Profiler</button></div>
      <div class="cards">
        <div class="card"><h2>Power</h2><div class="metrics"><${Metric} label="Battery" value=${hk && hk.bat_v} unit="V" /><${Metric} label="Solar" value=${hk && hk.sol_v} unit="V" /><${Metric} label="Bus" value=${hk && hk.bus_v} unit="V" />
          <${Metric} label="Battery I" value=${hk && hk.bat_i} unit="A" /><${Metric} label="Bus I" value=${hk && hk.bus_i} unit="A" /><${Metric} label="Unreg I" value=${hk && hk.unreg_i} unit="A" />
          <${Metric} label="3.3 V I" value=${hk && hk.i_3v3} unit="A" /><${Metric} label="5 V I" value=${hk && hk.i_5v} unit="A" /></div>
          <${Spark} data=${col(s.evt, 'hk', e => e.bat_v)} /><div class="hint">battery voltage per HK packet</div></div>
        <div class="card"><h2>Thermal</h2><div class="metrics"><${Metric} label="Battery" value=${hk && hk.bat_t} unit="°C" d=${1} /><${Metric} label="Antenna" value=${hk && hk.ant_t} unit="°C" d=${1} /><${Metric} label="Power board" value=${hk && hk.bpb_t} unit="°C" d=${1} /></div>
          <${Spark} data=${col(s.evt, 'hk', e => e.bat_t)} color="var(--warn)" />
          <h2 style="margin-top:12px">Solar panels</h2><div class="metrics">${['SP1', 'SP2', 'SP3', 'SP4', 'SP5'].map(k => h`<${Metric} key=${k} label=${k} value=${hk && hk.panels_v[k]} unit="V" />`)}</div></div>
        <div class="card"><h2>Attitude (magnetometer)</h2><${Cube} mag=${mag} size=${230} />
          ${hk && h`<div class="metrics" style="margin-top:8px"><${Metric} label="Gyro X" value=${hk.gyro[0]} unit="°/s" /><${Metric} label="Gyro Y" value=${hk.gyro[1]} unit="°/s" /><${Metric} label="Gyro Z" value=${hk.gyro[2]} unit="°/s" /></div>`}
          <div class="hint">source: ${hk && hk.mag.some(v => v) ? 'HK packet' : bc ? 'CW beacon' : 'none yet'}</div></div>
        <div class="card"><h2>Radio link</h2><div class="metrics"><${Metric} label="RSSI" value=${rs.length ? rs[rs.length - 1].rssi : null} unit="dBm" d=${0} /><${Metric} label="SNR" value=${rs.length ? rs[rs.length - 1].snr : null} unit="dB" d=${0} />
          <${Metric} label="HK packets" value=${counts.hk || 0} d=${0} /><${Metric} label="ACK / NACK" value=${(counts.ack || 0) + ' / ' + (counts.nack || 0)} /></div>
          <${Spark} data=${rs.map(e => e.rssi)} color="var(--cyan)" /></div>
        <div class="card"><h2>CW beacon</h2>${bc ? h`<${Kv} rows=${[['bytes', h`<span class="mono">${bc.hex}</span>`], ['as beacon 1', `${bc.b1.system}, battery ${bc.b1.bat_v} V ${bc.b1.bat_i} A, solar ${bc.b1.sol_total_v}/${bc.b1.sol_total_i}`],
          ['as beacon 2', `gyro ${bc.b2.gyro.join(',')} · mag ${bc.b2.magneto.join(',')} · bat ${bc.b2.bat_temp_c} °C`], ['received', fmtT(bc.t) + ' (' + age(bc.t) + ' ago)']]} />` : h`<div class="hint">none decoded yet</div>`}</div>
        <div class="card"><h2>Housekeeping packet</h2>${hk ? h`<${Kv} rows=${[['sequence', hk.seq], ['footers', hk.crc_ok ? 'OK' : 'BAD'], ['RSSI', hk.rssi + ' dBm'], ['flags', hk.flag1 + ' / ' + hk.flag2], ['received', fmtT(hk.t) + ' (' + age(hk.t) + ' ago)']]} /><div class="mono hint" style="word-break:break-all;margin-top:6px">${hk.hex}</div>` : h`<div class="hint">no packet yet: press Request HK1 (the satellite answers between its CW beacons)</div>`}</div>
        ${prof && h`<div class="card"><h2>Boards (profiler)</h2>${prof.cores.map(c => h`<div key=${c.core} style="margin:4px 0"><b>${c.core}</b> busy ${c.busy_pct}% · top: ${c.top.slice(0, 3).map(t => t.fn + ' ' + t.pct + '%').join(', ')}</div>`)}</div>`}
      </div>
    </div>`;
  }
  Object.assign(S2, { Dashboard });
})();
