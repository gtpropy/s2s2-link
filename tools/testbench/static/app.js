(function () {
  const { h, useStore, useState, useEffect, api, Chip, refresh, connectSSE, state } = S2;
  const PAGES = [
    ['dashboard', 'Dashboard', '◉', () => S2.Dashboard],
    ['consoles', 'Consoles', '⌨', () => S2.Consoles],
    ['profiler', 'Profiler', '▥', () => S2.Profiler],
    ['alerts', 'Alerts', '⚑', () => S2.Alerts],
    ['commands', 'Command tester', '➤', () => S2.Commands],
    ['digipeater', 'Digipeater', '⇄', () => S2.Digipeater],
    ['images', 'Images', '▣', () => S2.Images],
    ['radio', 'Radio / SDR', '≋', () => S2.Radio],
    ['tests', 'Tests / E2E', '✔', () => S2.Tests],
    ['scenarios', 'Scenarios', '☷', () => S2.Scenarios],
    ['trends', 'Trends / compare', '↗', () => S2.Trends],
    ['logs', 'Logs', '☰', () => S2.Logs],
    ['reports', 'Reports / shots', '▤', () => S2.LogPage],
    ['config', 'Configuration', '⚙', () => S2.Config],
    ['build', 'Build / flash', '⬇', () => S2.Build],
    ['cubusdebug', 'CUBUS debug', '🔍', () => S2.CubusDebug],
    ['cubuspins', 'CUBUS pins', '⌖', () => S2.CubusPins],
  ];
  const SEP = { dashboard: 'Observe', commands: 'Test', trends: 'History', config: 'System', cubusdebug: 'DEBUG:' };

  function App() {
    const s = useStore();
    const [page, setPage] = useState((location.hash || '#dashboard').slice(1));
    const [theme, setTheme] = useState(() => { try { return localStorage.getItem('s2s2.theme') || ''; } catch (e) { return ''; } });
    useEffect(() => { const f = () => setPage((location.hash || '#dashboard').slice(1)); addEventListener('hashchange', f); return () => removeEventListener('hashchange', f); }, []);
    useEffect(() => { if (theme) document.documentElement.setAttribute('data-theme', theme); else document.documentElement.removeAttribute('data-theme'); try { localStorage.setItem('s2s2.theme', theme); } catch (e) { /* ignore */ } }, [theme]);
    const [shotMsg, setShotMsg] = useState('');
    const go = p => { location.hash = p; };
    const st = s.status;
    const cur = PAGES.find(p => p[0] === page) || PAGES[0];
    const Page = cur[3]();
    return h`<div class="app">
      <div class="top"><span class="brand">S2S2 <small>Testbench</small></span>
        <${Chip} cls=${st && st.gs.connected ? 'ok' : 'bad'} title=${st && st.gs.port}>GS<//>
        <${Chip} cls=${st && st.obc.connected ? 'ok' : 'bad'} title=${st && st.obc.port}>${st && st.cubus ? 'Sat UART' : 'Nucleo'}<//>
        ${st && st.cubus && h`<${Chip} cls="warn" title="the satellite is built for the RSMS CUBUS V.3 flight board (CUBUS=T)">CUBUS<//>`}
        <${Chip} cls=${s.monitor && s.monitor.on ? 'ok' : (st && st.sdr.present ? 'warn' : 'bad')}>SDR<//>
        <${Chip} cls=${s.running ? 'run' : 'ok'}>${s.running ? 'running: ' + s.running : 'idle'}<//>
        ${s.alerts.some(a => !a.acked) && h`<span class="chip" style="cursor:pointer;border-color:var(--bad)" onClick=${() => go('alerts')}><i class="dot bad"></i>${s.alerts.filter(a => !a.acked).length} alert(s)</span>`}
        ${s.running && h`<button class="sm danger" onClick=${() => api('/api/test/stop', {})}>Stop</button>`}
        <span class="grow"></span>
        <span class="hint">${shotMsg || (st ? 'git ' + st.git : '')}</span>
        <button class="sm" title="screenshot of the page you are looking at, saved for the next report" onClick=${() => { setShotMsg('taking screenshot …'); api('/api/shot', { route: page, note: 'quick' }).then(r => setShotMsg('saved ' + r.file)).catch(e => setShotMsg(e.message)); setTimeout(() => setShotMsg(''), 9000); }}>📷 screenshot</button>
        <button class="sm" onClick=${() => setTheme(theme === 'light' ? 'dark' : 'light')}>${theme === 'light' ? 'dark' : 'light'}</button>
      </div>
      <nav class="side">${PAGES.map(([k, t, i]) => h`<${React.Fragment} key=${k}>${SEP[k] ? h`<div class="sep">${SEP[k]}</div>` : null}<button class=${page === k || (!PAGES.some(p => p[0] === page) && k === 'dashboard') ? 'on' : ''} onClick=${() => go(k)}><span>${i}</span>${t}</button><//>`)}</nav>
      <main class="main"><${UI.Boundary} resetKey=${page}>${h`<${Page} go=${go} />`}<//></main>
    </div>`;
  }

  connectSSE();
  api('/api/commands').then(p => { state.palette = p; state.palette.commands = p.commands; S2.emit(); }).catch(() => {});
  api('/api/alerts').then(r => { state.alerts = r.alerts; S2.emit(); }).catch(() => {});
  refresh(true);
  setInterval(() => refresh(false), 5000);
  ReactDOM.createRoot(document.getElementById('root')).render(h`<${App} />`);
})();
