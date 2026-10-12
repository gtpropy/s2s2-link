(function () {
  const { h, api, S, useS, startLive, loadAll, poll, utc, age, Chip, useState, useEffect } = MC;
  const PAGES = [['overview', 'Overview', '◉'], ['attitude', 'Attitude', '⟲'], ['telemetry', 'Telemetry', '▤'], ['beacons', 'CW beacons', '≋'], ['commands', 'Commands', '➤'], ['imagery', 'Imagery', '▣'], ['admin', 'Administration', '⚙']];

  function Login({ onIn }) {
    const [u, setU] = useState('admin'), [p, setP] = useState(''), [err, setErr] = useState('');
    const go = () => api('/api/login', { user: u, password: p }).then(onIn).catch(e => setErr(e.message));
    return h`<div class="login"><div class="card"><div class="brand">S2S2 <i>MISSION CONTROL</i></div>
      <input placeholder="user" value=${u} onInput=${e => setU(e.target.value)} /><input type="password" placeholder="password" autofocus value=${p} onInput=${e => setP(e.target.value)} onKeyDown=${e => e.key === 'Enter' && go()} />
      ${err && h`<div class="err">${err}</div>`}<button class="pri" onClick=${go}>Log in</button></div></div>`;
  }

  function Shell({ me, onOut }) {
    const s = useS();
    const [page, setPage] = useState((location.hash || '#overview').slice(1));
    const [clock, setClock] = useState(Date.now());
    const [theme, setTheme] = useState(() => { try { return localStorage.getItem('s2s2.mc.theme') || ''; } catch (e) { return ''; } });
    useEffect(() => { const f = () => setPage((location.hash || '#overview').slice(1)); addEventListener('hashchange', f); const t = setInterval(() => setClock(Date.now()), 1000); const p = setInterval(poll, 3000); return () => { removeEventListener('hashchange', f); clearInterval(t); clearInterval(p); }; }, []);
    useEffect(() => { theme ? document.documentElement.setAttribute('data-theme', theme) : document.documentElement.removeAttribute('data-theme'); try { localStorage.setItem('s2s2.mc.theme', theme); } catch (e) { /* ignore */ } }, [theme]);
    const gs = s.snap && s.snap.gs;
    const lc = s.snap && s.snap.last_contact;
    const stale = lc && Date.now() / 1000 - lc > 300;
    const alerts = MCV.alertsOf(s).filter(a => a.level === 'bad').length;
    const P = { overview: MCV.Overview, attitude: MCV.Attitude, telemetry: MCV.Telemetry, beacons: MCV.Beacons, commands: MCV.Commands, imagery: MCV.Imagery, admin: MCV.Admin }[page] || MCV.Overview;
    return h`<div class="app"><div class="top"><span class="brand">S2S2 <i>MISSION CONTROL</i></span>
      <span class="clock" title="UTC">${new Date(clock).toISOString().slice(11, 19)}Z</span>
      <${Chip} cls=${gs && gs.connected ? 'ok' : 'bad'} title=${gs && gs.port}>Ground station ${gs && gs.connected ? 'online' : 'offline'}<//>
      <${Chip} cls=${!lc ? '' : stale ? 'warn' : 'ok'}>Last contact ${age(lc)}<//>
      ${alerts > 0 && h`<${Chip} cls="bad">${alerts} alert${alerts > 1 ? 's' : ''}<//>`}
      <span class="grow"></span><span class="hint">${me.user} (${me.role})</span>
      <button class="sm" onClick=${() => setTheme(theme === 'light' ? 'dark' : 'light')}>${theme === 'light' ? 'dark' : 'light'}</button>
      <button class="sm" onClick=${() => api('/api/logout', {}).then(onOut)}>log out</button></div>
      <nav class="side">${PAGES.map(([k, t, i], n) => h`<${React.Fragment} key=${k}>${n === 4 ? h`<div class="sep">Operate</div>` : n === 0 ? h`<div class="sep">Monitor</div>` : null}
        <button class=${page === k ? 'on' : ''} onClick=${() => { location.hash = k; }}><span>${i}</span>${t}</button><//>`)}</nav>
      <main class="main"><${UI.Boundary} resetKey=${page}><${P} me=${me} go=${p => { location.hash = p; }} /><//></main></div>`;
  }

  function App() {
    const [me, setMe] = useState(undefined);
    const enter = m => { setMe(m); loadAll().then(startLive).catch(() => {}); };
    useEffect(() => { api('/api/me').then(m => (m.user ? enter(m) : setMe(null))).catch(() => setMe(null)); const f = () => setMe(null); addEventListener('s2s2-auth', f); return () => removeEventListener('s2s2-auth', f); }, []);
    if (me === undefined) return null;
    if (!me) return h`<${Login} onIn=${enter} />`;
    return h`<${Shell} me=${me} onOut=${() => setMe(null)} />`;
  }
  ReactDOM.createRoot(document.getElementById('root')).render(h`<${App} />`);
})();
