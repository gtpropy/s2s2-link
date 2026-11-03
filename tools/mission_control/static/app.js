(function () {
  const { h, api, S, useS, useNow, startLive, loadAll, poll, emit, satState, useState, useEffect } = MC;
  const NAV = [['home', 'Home'], ['dashboard', 'Satellite control'], ['commands', 'Commands'], ['history', 'Command history'], ['telemetry', 'Telemetry'], ['location', 'Position and attitude'], ['logs', 'Ground station log'], ['pictures', 'Images'], ['help', 'Operations guide'], ['settings', 'Settings']];
  const store = (k, v) => { try { localStorage.setItem(k, v); } catch (e) { /* storage blocked: nothing is remembered */ } };
  const recall = k => { try { return localStorage.getItem(k) || ''; } catch (e) { return ''; } };
  const KONAMI = ['ArrowUp', 'ArrowUp', 'ArrowDown', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'ArrowLeft', 'ArrowRight', 'b', 'a'];
  const route = () => { const [p, ...a] = (location.hash || '#home').slice(1).split('/'); const args = a.map(x => { try { return decodeURIComponent(x); } catch (e) { return x; } }); return { page: p || 'home', arg: args.join('/'), args }; };

  function Login({ onIn }) {
    const [u, setU] = useState('admin'), [p, setP] = useState(''), [err, setErr] = useState('');
    const go = () => api('/api/login', { user: u, password: p }).then(onIn).catch(e => setErr(e.message));
    return h`<div class="login"><div class="card"><div class="cbody"><div class="brand">S2S2 <i>MISSION CONTROL</i></div><p class="hint">Sign in to continue.</p>
      <label class="field">User name<input value=${u} onInput=${e => setU(e.target.value)} autocomplete="username" /></label>
      <label class="field">Password<input type="password" autofocus value=${p} onInput=${e => setP(e.target.value)} onKeyDown=${e => e.key === 'Enter' && go()} autocomplete="current-password" /></label>
      ${err && h`<div class="note bad">${err}</div>`}<button class="pri big" onClick=${go}>Sign in</button></div></div></div>`;
  }

  function Shell({ me, onOut }) {
    const s = useS();
    const now = useNow(1);
    const [rt, setRt] = useState(route());
    const [tour, setTour] = useState(false);
    const [offer, setOffer] = useState(() => !recall('s2s2.mc.toured'));
    const [theme, setTheme] = useState(() => recall('s2s2.mc.theme') || 'dark');
    const [size, setSize] = useState(() => recall('s2s2.mc.size'));
    useEffect(() => { const f = () => { S.modal = null; setNavOpen(false); setRt(route()); window.scrollTo(0, 0); const m = document.querySelector('.main'); if (m) m.scrollTop = 0; }; addEventListener('hashchange', f); const p = setInterval(poll, 3000); return () => { removeEventListener('hashchange', f); clearInterval(p); }; }, []);
    useEffect(() => { document.documentElement.setAttribute('data-theme', theme); store('s2s2.mc.theme', theme); }, [theme]);
    useEffect(() => { size ? document.documentElement.setAttribute('data-size', size) : document.documentElement.removeAttribute('data-size'); store('s2s2.mc.size', size); }, [size]);
    useEffect(() => {          // key sequence for mock mode
      let buf = [];
      const f = e => {
        if (/INPUT|TEXTAREA|SELECT/.test((e.target && e.target.tagName) || '')) return;
        buf = buf.concat(e.key.length === 1 ? e.key.toLowerCase() : e.key).slice(-KONAMI.length);
        if (buf.join() === KONAMI.join()) { buf = []; MOCK.toggle(); }
      };
      addEventListener('keydown', f);
      return () => removeEventListener('keydown', f);
    }, []);
    useEffect(() => {          // every link to a command opens it in a pop-up over the current page; the page #cmd/<id> still exists for a direct link
      const click = e => {
        if (e.defaultPrevented || e.button || e.metaKey || e.ctrlKey || e.shiftKey) return;
        const a = e.target.closest && e.target.closest('a[href^="#cmd/"]');
        if (!a) return;
        e.preventDefault(); S.modal = decodeURIComponent(a.getAttribute('href').slice(5)); emit();
      };
      const key = e => { if (e.key === 'Escape' && S.modal) { S.modal = null; emit(); } };
      const practice = () => setTimeout(() => { if (S.modal) { S.modal = null; emit(); } }, 1600);
      document.addEventListener('click', click); addEventListener('keydown', key); addEventListener('s2s2-practice-sent', practice);
      return () => { document.removeEventListener('click', click); removeEventListener('keydown', key); removeEventListener('s2s2-practice-sent', practice); };
    }, []);
    const closeModal = () => { S.modal = null; emit(); };
    const endTour = done => { setTour(false); setOffer(false); store('s2s2.mc.toured', '1'); if (done) location.hash = 'home'; };
    const startTour = () => { setOffer(false); location.hash = 'home'; setTimeout(() => setTour(true), 150); };
    const st = satState(s);
    const { page, arg, args } = rt;
    const [navOpen, setNavOpen] = useState(false);
    const active = { cmd: 'commands', job: 'history', report: 'home' }[page] || page;
    const P = {
      home: () => h`<${MCV2.HomePage} />`, report: () => h`<${MCV2.ReportPage} args=${args} />`, dashboard: () => h`<${MCV.Dashboard} />`, telemetry: () => h`<${MCV2.TelemetryPage} />`, location: () => h`<${MCV.LocationPage} me=${me} />`, commands: () => h`<${MCV.CommandsPage} />`,
      cmd: () => h`<${MCV.CommandPage} id=${arg} />`, history: () => h`<${MCV.HistoryPage} />`, job: () => h`<${MCV.JobPage} id=${arg} />`, logs: () => h`<${MCV.LogsPage} />`,
      pictures: () => h`<${MCV.PicturesPage} />`, help: () => h`<${MCV.HelpPage} startTour=${startTour} />`, settings: () => h`<${MCV.SettingsPage} me=${me} size=${size} setSize=${setSize} theme=${theme} setTheme=${setTheme} />`,
    }[page] || (() => h`<${MCV2.HomePage} />`);
    return h`<div class=${'app ' + (page === 'dashboard' ? 'dashpage ' + (navOpen ? 'navopen' : '') : '')}>
      ${s.mock && h`<div class="mockbar" role="alert"><b>MOCK MODE</b> All displayed data is generated test data. Nothing is transmitted or stored. <button class="sm" onClick=${() => MOCK.off()}>Exit mock mode</button></div>`}
      <header class="top">${page === 'dashboard' && h`<button class="menubtn" aria-label="Open the menu" aria-expanded=${navOpen} onClick=${() => setNavOpen(!navOpen)} data-tour="menubtn">\u2630</button>`}<a class="brand" href="#home">S2S2 <i>MISSION CONTROL</i></a>
        <span class=${'status ' + st.level} title=${st.text}><${MCP.Led} s=${st.level} />${st.title}</span><${MCV.CwBar} /><span class="grow"></span>
        <span class="clock" title="Coordinated Universal Time">${new Date(now * 1000).toISOString().slice(11, 19)} UTC</span>
        <button class="sm" onClick=${startTour}>Tour</button><span class="who">${me.user}</span><button class="sm" onClick=${() => api('/api/logout', {}).then(onOut)}>Sign out</button></header>
      ${navOpen && h`<div class="navback" onClick=${() => setNavOpen(false)}></div>`}<nav class="side" data-tour="nav" aria-label="Sections"><div class="sidehead"><b>Menu</b><button class="sm" onClick=${() => setNavOpen(false)}>Close \u2715</button></div>${NAV.map(([k, t]) => h`<a key=${k} href=${'#' + k} class=${active === k ? 'on' : ''} aria-current=${active === k ? 'page' : null} onClick=${k === 'dashboard' ? () => MCV.goFull() : null}>${t}</a>`)}</nav>
      <main class=${'main ' + (page === 'dashboard' ? 'dashmain' : '')}>${offer && h`<div class="note offer"><b>First visit.</b> A short interface tour is available. <button class="pri sm" onClick=${startTour}>Start tour</button> <button class="sm" onClick=${() => { setOffer(false); store('s2s2.mc.toured', '1'); }}>Dismiss</button></div>`}
        <${UI.Boundary} resetKey=${page + arg}>${P()}<//></main>
      ${s.modal && h`<div class="popbg" onClick=${closeModal}><div class="pop cmdpop" role="dialog" aria-label="Command" onClick=${e => e.stopPropagation()}><${MCV.CommandPage} id=${s.modal} modal=${true} onClose=${closeModal} key=${s.modal} /></div></div>`}
      <${MCV2.OverpassAlert} />
      ${tour && h`<${Tour} onEnd=${endTour} />`}
    </div>`;
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
