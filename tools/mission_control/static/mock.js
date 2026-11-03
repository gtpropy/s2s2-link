/* MOCK MODE -- test data for trying the screens without a satellite.
 *
 * Turned on and off ONLY by a key sequence (see docs/MISSION_CONTROL.md). While it is on:
 *   - a bright banner and a "MOCK" watermark are on every screen, the browser tab says [MOCK];
 *   - the real data is put aside and comes back unchanged when mock mode ends;
 *   - nothing is sent to the ground station and nothing is written on the server (commands, settings and accounts are answered here);
 *   - the generated values are never stored, exported or used for alerts anywhere else.
 */
(function () {
  const { S, emit } = MC;
  const SAMPLE_TLE = { name: 'SAMPLE ORBIT (mock)', source: 'built-in sample', norad: 25544, error: '',
    line1: '1 25544U 98067A   26300.82187419  .00005456  00000+0  10681-3 0  9999', line2: '2 25544  51.6336   3.7485 0007015 317.3678  42.6766 15.49080514592319', fetched: 0 };
  const FIELDS = ['snap', 'hk', 'beacon', 'evt', 'raw', 'jobs', 'images', 'gslink', 'orbit', 'link'];
  let saved = null, timers = [], seq = 0, n = 0, jobId = 9000000;
  const rnd = (a, b) => a + Math.random() * (b - a);
  const now = () => Date.now() / 1000;
  /* slowly varying, orbit-shaped test values: charge when the satellite is sunlit (computed from the orbit), discharge in eclipse; one revolution is about 93 minutes */
  function model(t) {
    const lit = window.Orbit && Orbit.ok() ? Orbit.sunlit(new Date(t * 1000)) !== false : true;
    const ph = t / 5580 * 2 * Math.PI, n = () => (Math.random() - 0.5);
    return { lit, bat_v: 3.88 + 0.11 * Math.sin(ph - 0.9) + 0.01 * n(), sol_v: lit ? 4.35 + 0.06 * n() : 0.04 + 0.02 * Math.abs(n()), bat_i: lit ? 0.17 + 0.02 * n() : -0.12 + 0.01 * n(),
      bat_t: 21 + 3 * Math.sin(ph - 1.4) + 0.1 * n(), rssi: Math.round(-90 + 5 * Math.sin(t / 240) + 3 * n()) };
  }
  const hex2 = b => b.toString(16).padStart(2, '0').toUpperCase();
  const s8 = x => (x > 127 ? x - 256 : x);

  /* same reading of the 8 beacon bytes as tools/common/events.py decode_beacon */
  function decodeBeacon(b) {
    const SYS = { 0: '?', 1: 'safe', 2: 'low power', 3: 'normal' }, MIS = { 0: 'reserved', 1: 'camera', 2: 'ADCS', 3: 'EPDM' };
    const b1 = { system: SYS[b[0] >> 6], mission: MIS[(b[0] >> 4) & 3], hours_since_reset: b[0] & 15, sol_total_i: b[1], sol_total_v: (b[2] << 8) | b[3], bat_v: ((b[4] << 8) | b[5]) / 100, bat_i: (((b[6] << 8) | b[7]) << 16 >> 16) / 100 };
    const b2 = { gyro: [s8(b[0]), s8(b[1]), s8(b[2])], magneto: [s8(b[3]), s8(b[4]), s8(b[5])],
      solar_cells: Object.fromEntries(['+X', '+Y', '-Z', '+Y2', '+Z'].map((k, i) => [k, !!(b[6] & (1 << i))])), bat_temp_c: s8(b[7]) };
    return { b1, b2 };
  }
  function add(ev) {
    const e = { t: now(), mock: true, ...ev };
    S.evt = S.evt.concat(e).slice(-400);
    if (e.kind === 'hk') S.hk = S.hk.concat(e).slice(-20000);
    if (e.kind === 'beacon') S.beacon = S.beacon.concat(e).slice(-20000);
    S.snap.last_contact = e.t;
    return e;
  }
  function raw(text) { S.raw = S.raw.concat({ t: now(), text, mock: true }).slice(-1500); }

  /* the two beacons as the real COM firmware builds them (satellite_main.c Cw_Beacon1_Hex / Cw_Beacon2_Hex), sent alternately 1, 2, 1, 2:
   *  B1: sys|mission|hours, SOL_TOTAL_I, SOL_TOTAL_V (0.01 V), BAT_V (0.01 V), BAT_I (0.01 A)   - the OBC's values, one Li-ion cell (about 3.4-4.2 V)
   *  B2: gyro x,y,z, magnetometer x,y,z, solar-cell bits (all 0: the M4 delivers nothing for them), BAT_TEMP (whole degrees C) */
  function beacon() {
    let bytes;
    if (n % 2 === 0) {
      const M = model(now()), bv = Math.round(M.bat_v * 100), sv = Math.round(M.sol_v * 100), bi = Math.round(M.bat_i * 100) & 0xFFFF;
      bytes = [(3 << 6) | (0 << 4) | Math.min(15, Math.floor(n / 240)), Math.round(rnd(20, 60)), sv >> 8, sv & 255, bv >> 8, bv & 255, bi >> 8, bi & 255];
    } else {
      bytes = [0, 0, 0, 0, 0, 0, 0, Math.round(model(now()).bat_t)];
    }
    const hex = bytes.map(hex2).join('');
    // the decoder prints one line per tone / letter while the beacon is being received, then the finished text
    const text = 'S2S2 9N2SJ ' + hex.match(/.{4}/g).join(' ');   // same layout as the firmware: %02X%02X %02X%02X %02X%02X %02X%02X
    raw('{"type":"CWCAR","ms":500,"rssi":-95}');
    [...text].forEach((ch, i) => timers.push(setTimeout(() => { if (!S.mock) return; raw('{"type":"CWSIG","on":1,"rssi":-95}'); raw(`{"type":"CWCHAR","ch":"${ch}"}`); emit(); }, 150 * (i + 1))));
    timers.push(setTimeout(() => {
      if (!S.mock) return;
      add({ kind: 'cw', text }); add({ kind: 'beacon', hex, ...decodeBeacon(bytes) });
      raw('[CW DECODED] ' + text); raw(`{"type":"CWHK","hex":"${hex}"}`); emit();
    }, 150 * (text.length + 2)));
    n++;
  }
  function rssi() {
    const r = model(now()).rssi;
    add({ kind: 'rssi', n: ++seq, rssi: r, snr: Math.round(rnd(2, 12)), bytes: 200 });
    raw(`[GS RX] Packet #${seq} | RSSI=${r} dBm | SNR=${Math.round(rnd(2, 12))} dB | 200 bytes`);
  }
  /* housekeeping packet: values in the ranges of the real board (one Li-ion cell, small currents, thermistors near room temperature) */
  function hkEvent(tt) {
    const T = tt || now(), M = model(T), w = T / 60;
    const hk = { kind: 'hk', bat_v: M.bat_v, sol_v: M.sol_v, bus_v: M.bat_v - 0.02 + 0.005 * Math.random(), panels_v: { SP1: M.sol_v, SP2: M.sol_v * 0.95, SP3: 0.1, SP4: M.sol_v * 0.9, SP5: 0.2 },
      ant_t: 14 + 6 * Math.sin(w / 80), bat_t: M.bat_t, bpb_t: 23 + 2 * Math.sin(w / 90), unreg_i: M.lit ? 0.31 : 0.05, i_3v3: 0.12, i_5v: 0.05, bat_i: M.bat_i, bus_i: 0.33, flag1: 0, flag2: 0,
      gyro: [rnd(-0.5, 0.5), rnd(-0.5, 0.5), rnd(-0.5, 0.5)], mag: [22 + 3 * Math.sin(w / 15), -8 + 2 * Math.cos(w / 15), 35 + Math.sin(w / 30)], seq: ++seq, crc_ok: true, rssi: M.rssi, hex: Array.from({ length: 128 }, () => hex2(Math.floor(Math.random() * 256))).join('') };
    const added = add(tt ? { ...hk, t: tt } : hk);
    if (!tt) {
      raw('[GS RX] *** HK TELEMETRY PACKET DETECTED (128 bytes) ***');
      raw(`{"type":"HK","seq":${hk.seq},"hex":"${hk.hex.slice(0, 32)}…"}`);
    }
    return added;
  }

  /* image download in mock mode: a generated test pattern revealed fragment by fragment (it only exercises the live view; it is not an image from the satellite) */
  function mockImage() {
    const id = 'MOCK-' + Date.now().toString(36), total = 120, t0 = now(), missing = Array.from({ length: total }, (_, i) => i + 1);
    const cv = document.createElement('canvas'); cv.width = 320; cv.height = 240;
    let got = 0;
    const render = () => {
      const g = cv.getContext('2d'), hh = Math.round(240 * got / total);
      g.fillStyle = '#222'; g.fillRect(0, 0, 320, 240);
      g.save(); g.beginPath(); g.rect(0, 0, 320, hh); g.clip();
      const gr = g.createLinearGradient(0, 0, 320, 240); gr.addColorStop(0, '#3a6ea5'); gr.addColorStop(1, '#d08a3c'); g.fillStyle = gr; g.fillRect(0, 0, 320, 240);
      g.fillStyle = '#fff'; g.font = 'bold 22px sans-serif'; g.fillText('MOCK TEST PATTERN', 20, 120); g.restore();
      return cv.toDataURL('image/jpeg', 0.7);
    };
    const step = () => {
      if (!S.mock) return;
      got++; missing.shift();
      const im = { id, total, got, complete: got >= total, missing: missing.slice(), dups: 0, t_first: t0, t_last: now(), seconds: Math.round((now() - t0) * 10) / 10, matches_sent: null, mock: true, src: render() };
      S.images = [im].concat(S.images.filter(x => x.id !== id)); emit();
      if (got < total) timers.push(setTimeout(step, 400));
    };
    timers.push(setTimeout(step, 1500));
  }

  /* a command: queued -> sending -> accepted -> data -> completed, each step visible in the same job object the real server publishes */
  const REPLY = { // what the mock satellite answers per command
    health: 'hk', hk2: 'hk', ping: 'ping', dummycam: 'ack', hkdl: 'hk', adcs: 'ack', epdm: 'ack', adcs_file: 'file', epdm_file: 'file', satlog: 'file', flags: 'file', rsvtable: 'nofile', cam_run: 'ack', cam_dl: 'ack', digi: 'sent', antenna: 'ack' };
  function runJob(cat, line) {
    const id = ++jobId, t0 = now();
    const job = { id, t: t0, user: 'mock', cmd: cat.id, label: cat.label, simple: cat.simple, icon: cat.icon, opcode: cat.opcode, line, state: 'queued', detail: { events: [], raw: [] }, wait_s: cat.wait_s, mock: true };
    const upd = (state, extra = {}) => { job.state = state; Object.assign(job.detail, extra); job.detail.raw = S.raw.filter(r => r.t >= t0 - 0.5); S.jobs = { ...S.jobs, [id]: { ...job, detail: { ...job.detail } } }; if (state !== 'queued' && state !== 'sending' && state !== 'accepted') job.t_end = now(); emit(); };
    const at = (ms, f) => timers.push(setTimeout(() => { if (S.mock) { f(); emit(); } }, ms));
    S.jobs = { ...S.jobs, [id]: job };
    const mis = { health: 1, hk2: 1, hkdl: 1, ping: 255, dummycam: 5, adcs: 4, epdm: 8, adcs_file: 4, epdm_file: 8, satlog: 10, cam_run: 5, cam_dl: 5, digi: 9, antenna: 255 }[cat.id] || 255;
    if (cat.id === 'cam_dl') mockImage();
    raw(`>>> [UPLINK] ${line}`);
    at(500, () => { raw('[GS TX] command sent'); upd('sending', { attempt: 1 }); });
    if (REPLY[cat.id] === 'sent') { at(1500, () => upd('sent', { note: 'frame transmitted (mock)' })); return id; }
    at(2600, () => { const a = add({ kind: 'ack', what: 'accepted', mission: mis, mission_name: ({ 1: 'housekeeping', 4: 'ADCS', 5: 'camera', 8: 'EPDM', 10: 'satellite log', 255: 'general' })[mis], text: 'ACK accepted' }); raw(`{"type":"ACK","msg":"ACK Accepted mission=0x0${mis.toString(16)}"}`); upd('accepted', { attempt: 1, events: [a] }); });
    if (REPLY[cat.id] === 'nofile') {                               // what the real satellite answers when the OBC has no such file: NACK 05 requested data missing
      at(3600, () => { const e = add({ kind: 'nack', err: 5, name: 'requested data missing', mission: mis, mission_name: 'general', text: 'NACK data missing' }); raw('{"type":"NACK","msg":"NACK err=0x05 mission=0xff"}'); upd('nack', { events: [e], error: 'NACK 0x05 requested data missing' }); });
      return id;
    }
    at(4200, () => {
      const evs = [];
      if (REPLY[cat.id] === 'hk') evs.push(hkEvent());
      if (REPLY[cat.id] === 'ping') { evs.push(add({ kind: 'ascii', text: 'ACK: PING OK (OBC_ONLINE)' })); raw('[GS RX] ASCII response: "ACK: PING OK (OBC_ONLINE)"'); }
      if (REPLY[cat.id] === 'file') {          // placeholder text only: the real file contents are not known to this program
        const txt = new TextEncoder().encode(`MOCK FILE ${cat.id}\nThis is placeholder text from mock mode.\nIt is not a measurement.\n`);
        const pad = new Uint8Array(256); pad.set(txt);
        [0, 1].forEach(k => evs.push(add({ kind: 'file', pkt: k + 1, total: 2, addr: k * 128, len: 128, status: 0, hex: Array.from(pad.slice(k * 128, k * 128 + 128)).map(hex2).join('') })));
      }
      const c = add({ kind: 'ack', what: 'completed', mission: mis, mission_name: ({ 1: 'housekeeping', 5: 'camera', 8: 'EPDM', 255: 'general' })[mis], text: 'ACK Completed' });
      raw(`{"type":"ACK","msg":"ACK Completed mission=0x0${mis.toString(16)}"}`);
      upd('completed', { events: job.detail.events.concat(evs, c) });
    });
    return id;
  }

  /* mock heartbeat: answered every ~35 s with a believable RSSI, so the link panel can be tried */
  function mockLink() {
    const t = now();
    return { enabled: true, autosample: true, responsive: true, command_mode: true, last_t: t, last_ok_t: t, rtt: 1.2, rssi: -90, snr: 8, success: 100, sent: 5, next_due: t + 35, busy: false, in_pass: false, gs: true,
      hist: Array.from({ length: 12 }, (_, i) => ({ t: t - (12 - i) * 35, ok: true })), pass_reason: '', sample_basis: 'mock' };
  }
  function mockBeat() { const t = now(), r = Math.round(rnd(-96, -82)); S.link = { ...S.link, last_t: t, last_ok_t: t, rssi: r, snr: Math.round(rnd(4, 11)), next_due: t + 35, hist: S.link.hist.concat({ t, ok: true }).slice(-30) }; add({ kind: 'rssi', n: ++seq, rssi: r, snr: S.link.snr, bytes: 200 }); raw('[GS RX] ASCII response: "ACK: PING OK (OBC_ONLINE)"'); }
  const MOCK = {
    on() {
      if (S.mock) return;
      saved = {}; FIELDS.forEach(k => { saved[k] = S[k]; });
      S.mock = true;
      S.hk = []; S.beacon = []; S.evt = []; S.raw = []; S.jobs = {}; S.images = []; S.gslink = null; S.link = mockLink();
      S.snap = { t: now(), gs: { connected: true, port: 'MOCK (no real radio)', ports: [], error: '', last_byte_age: 0 }, last_contact: null, latest: {}, rssi: [], counters: {}, images: [], queue: [], limits: {} };
      const real = saved.orbit;
      S.orbit = { tle: (real && real.tle) || SAMPLE_TLE, station: (real && real.station) || { name: 'Ground station (mock)', lat: 27.7172, lon: 85.324, alt: 1.4 }, station_is_default: true, launch: Date.now() / 1000 - 12.4 * 86400 };
      Orbit.set(S.orbit.tle, S.orbit.station);
      document.body.classList.add('mock'); document.title = '[MOCK] ' + document.title.replace(/^\[MOCK\] /, '');
      raw('MOCK MODE ON: everything on screen is generated test data');
      for (let k = 1440; k > 0; k--) hkEvent(now() - k * 60);          // a day of history for the charts (generated)
      beacon(); rssi(); hkEvent();
      timers.push(setInterval(() => S.mock && (mockBeat(), emit()), 35000), setInterval(() => S.mock && (rssi(), emit()), 3000), setInterval(() => S.mock && (beacon(), emit()), 15000), setInterval(() => S.mock && (hkEvent(), emit()), 45000), setInterval(() => S.mock && (hkEvent(now()), emit()), 3000));       // quiet live stream so the charts move
      emit();
    },
    off() {
      if (!S.mock) return;
      timers.forEach(t => { clearTimeout(t); clearInterval(t); }); timers = [];
      S.mock = false; FIELDS.forEach(k => { S[k] = saved[k]; }); saved = null;
      document.body.classList.remove('mock'); document.title = document.title.replace(/^\[MOCK\] /, '');
      if (S.orbit && window.Orbit) Orbit.set(S.orbit.tle, S.orbit.station);
      emit();
      MC.loadAll().catch(() => {});
    },
    toggle() { S.mock ? MOCK.off() : MOCK.on(); },
    /* writes are answered here, reads go to the real server (catalog, me, orbit) */
    async api(p, body, realApi) {
      if (body === undefined || p === '/api/logout' || p === '/api/login') return realApi(p, body);
      if (p === '/api/command') {
        const c = S.catalog.find(x => x.id === body.id);
        if (!c) throw new Error('unknown command');
        await new Promise(r => setTimeout(r, 200));
        const line = (c.gs || c.id).replace('{count}', (body.args && body.args.count) || 5).replace('{text}', (body.args && body.args.text) || '');
        return { job: runJob(c, line) };
      }
      if (p === '/api/command/cancel') { const j = S.jobs[body.id]; if (j) { j.state = 'cancelled'; emit(); } return {}; }
      throw new Error('Mock mode: this change is not saved anywhere.');
    },
  };
  window.MOCK = MOCK;
})();
