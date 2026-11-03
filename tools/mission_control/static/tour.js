/* Guided, interactive tour. It highlights the real parts of the screen and, where it says "click", waits until the user really clicks them.
 * While it runs, S.practice is true: the command window sends nothing. */
(function () {
  const { h, S, useState, useEffect, useRef, emit } = MC;

  const STEPS = [
    { title: 'Interface tour', text: 'This tour covers the ground-station home screen and the satellite control screen. Where a step asks you to select something, the actual control is used. No command is transmitted while the tour runs.', go: 'home' },
    { sel: '[data-tour=drive]', title: 'Storage and Google Drive', text: 'Live storage figures: the size of the local data folder and, once Google Drive is configured, the space used and the size of the report folder.' },
    { sel: '[data-tour=reports]', title: 'Operational reports', text: 'One folder per day. Select a day to view its commands, overpasses, signal strength, mission data and raw logs here. Days older than seven days are downloaded from Google Drive when opened.' },
    { sel: '[data-tour=gosat]', click: true, title: 'Satellite control', text: 'Select \u201cGo to satellite control\u201d.' },
    { sel: '[data-tour=cw]', title: 'CW beacon', text: 'The satellite transmits a Morse-code beacon about every 90 seconds. The bar in the top menu, visible on every page, shows it being decoded character by character; \u201cbeacon details\u201d opens the decoded values and the raw bytes. The Fullscreen button gives the whole screen to this page; the menu button at the top left opens the sidebar over it.' },
    { sel: '[data-tour=state]', title: 'Satellite flags', text: 'Operating mode, active mission, time since reset and the age of the last health report, as small flags. Colour shows the state.' },
    { sel: '[data-tour=glance]', title: 'Link flags', text: 'Ground station connection, whether the satellite responds, the time since the last contact with its signal strength, line of contact (LOC), the next pass and the passes today. Hover a flag for details.' },
    { sel: '[data-tour=link]', title: 'Heartbeat', text: 'With the HB switch on, the ground station pings the satellite every 30 to 40 seconds. An answer means the satellite is responsive and in command mode. AUTO samples housekeeping and ADCS data during an overpass.' },
    { sel: '[data-tour=orbit]', title: 'Next pass', text: 'Countdown to acquisition (AOS) and loss (LOS) of signal with their exact times, the elevation and azimuth right now and the time since launch (once the launch time is entered in Settings).' },
    { sel: '[data-tour=map]', title: 'Position', text: 'The satellite moves along its track in real time. The green circle is the area that can see it, the dashed circle is the ground station\u2019s 5\u00b0 visibility limit and the shaded area is night. Pointing at a telemetry chart marks where the satellite was at that moment.' },
    { sel: '[data-tour=tel]', title: 'Telemetry', text: 'Interactive charts: point at a chart to read the value at that time and see the satellite\u2019s position when it was measured; select to pin it. The buttons choose the time window.' },
    { sel: '[data-tour=events]', title: 'RF RX log', text: 'Everything the ground station received from the satellite, as printed by the radio. The ground-station log below also shows what was sent.' },
    { sel: '[data-tour=cmds]', title: 'Commands', text: 'Every command is listed here; the groups open and close. Select one to open its own page.' },
    { sel: '.crow[data-cmd=health]', click: true, title: 'Open a command', text: 'Select \u201cHealth report\u201d. The command opens its own page where it is reviewed, transmitted and its response is displayed.' },
    { sel: '[data-tour=cmdform]', title: 'Command page', text: 'The page shows the command specification and the transmit control. No password is required.' },
    { sel: '[data-tour=sendgo]', ev: 's2s2-practice-sent', click: true, title: 'Transmit', text: 'Select the button. During the tour nothing is transmitted.' },
    { sel: '[data-tour=home]', click: true, title: 'Back to Home', text: 'Every page has a \u201cBack to Home\u201d link at the top. Select it to return to the ground-station overview.' },
    { sel: '[data-tour=nav]', title: 'Sections', text: 'Home, satellite control, commands, command history, telemetry, the 3D position view, the log, images, the operations guide and settings. This tour can be started again from the operations guide.' },
    { title: 'End of tour', text: 'An overpass notification appears 30 minutes before each pass and repeats every minute until it is confirmed with \u201cYes, I am ready\u201d.' },
  ];

  function Tour({ onEnd }) {
    const [i, setI] = useState(0);
    const [box, setBox] = useState(null);
    const step = STEPS[i];
    const adv = useRef(null);
    const next = () => setI(n => Math.min(STEPS.length, n + 1));
    useEffect(() => { S.practice = true; return () => { S.practice = false; }; }, []);
    useEffect(() => {
      if (step && step.go) location.hash = step.go;
      const place = () => {
        if (!step || !step.sel) return setBox(null);
        const el = document.querySelector(step.sel);
        if (!el) return setBox({ missing: true });
        const r = el.getBoundingClientRect();
        setBox({ x: r.left, y: r.top, w: r.width, h: r.height });
      };
      const el0 = step && step.sel && document.querySelector(step.sel);
      if (el0) el0.scrollIntoView({ block: 'center', behavior: 'smooth' });
      place();
      const t = setInterval(place, 250);
      return () => clearInterval(t);
    }, [i]);
    useEffect(() => {          // wait for the real click / event of "click" steps
      if (!step || !step.click) return;
      const done = () => { clearTimeout(adv.current); adv.current = setTimeout(next, 350); };
      if (step.ev) { addEventListener(step.ev, done); return () => removeEventListener(step.ev, done); }
      const f = e => { const el = document.querySelector(step.sel); if (el && el.contains(e.target)) done(); };
      document.addEventListener('click', f, true);
      return () => document.removeEventListener('click', f, true);
    }, [i]);
    useEffect(() => { if (i >= STEPS.length) onEnd(true); }, [i]);
    if (!step) return null;
    const vw = innerWidth, vh = innerHeight;
    const hasBox = box && !box.missing;
    const tipW = Math.min(380, vw - 24);
    let style = { left: Math.max(12, (vw - tipW) / 2), top: Math.max(12, vh / 2 - 100), width: tipW };
    if (hasBox) {
      const below = box.y + box.h + 12 + 190 < vh;
      style = { width: tipW, left: Math.min(Math.max(12, box.x + box.w / 2 - tipW / 2), vw - tipW - 12), top: below ? box.y + box.h + 12 : Math.max(12, box.y - 12 - 190) };
      if (!below && box.y < 200) style.top = Math.min(vh - 210, box.y + 16);
    }
    return h`<div class="tour" role="dialog" aria-label="Guided tour">
      ${hasBox ? h`<div class="spot" style=${`left:${box.x - 6}px;top:${box.y - 6}px;width:${box.w + 12}px;height:${box.h + 12}px`}></div>` : h`<div class="dim"></div>`}
      <div class="tip2" style=${Object.entries(style).map(([k, v]) => `${k}:${typeof v === 'number' ? v + 'px' : v}`).join(';')}>
        <small>Step ${i + 1} of ${STEPS.length}</small><b>${step.title}</b><p>${step.text}</p>
        ${box && box.missing && h`<p class="hint">The highlighted element is not visible on this page. Use Back or Next.</p>`}
        ${step.click && h`<p class="clickme">Select the highlighted control to continue.</p>`}
        <div class="row"><button class="sm" onClick=${() => onEnd(false)}>Exit tour</button><span class="grow"></span>${i > 0 && h`<button class="sm" onClick=${() => setI(n => n - 1)}>Back</button>`}
          <button class="pri sm" onClick=${next}>${i === STEPS.length - 1 ? 'Finish' : step.click ? 'Skip step' : 'Next'}</button></div></div></div>`;
  }
  window.Tour = Tour;
})();
