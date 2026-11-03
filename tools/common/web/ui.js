/* shared widgets of both web tools: h() with style/class fixes, Metric, Spark, Cube (attitude), time formatting */
(function () {
  const parseStyle = s => { const o = {}; String(s).split(';').forEach(d => { const i = d.indexOf(':'); if (i > 0) o[d.slice(0, i).trim().replace(/-([a-z])/g, (_, c) => c.toUpperCase())] = d.slice(i + 1).trim(); }); return o; };
  const h = htm.bind((type, props, ...kids) => {
    if (props && (typeof props.style === 'string' || props.class !== undefined)) {
      props = { ...props };
      if (typeof props.style === 'string') props.style = parseStyle(props.style);
      if (props.class !== undefined) { props.className = props.class; delete props.class; }
    }
    return React.createElement(type, props, ...kids);
  });
  const { useState, useEffect, useRef, useMemo } = React;

  /* ---- format */
  const utc = t => (t ? new Date(t * 1000).toISOString().slice(11, 19) + 'Z' : '–');
  const utcd = t => (t ? new Date(t * 1000).toISOString().slice(0, 19).replace('T', ' ') + 'Z' : '–');
  const age = t => { if (!t) return '–'; const s = Math.max(0, Date.now() / 1000 - t); return s < 90 ? Math.round(s) + ' s' : s < 5400 ? Math.round(s / 60) + ' min' : s < 172800 ? (s / 3600).toFixed(1) + ' h' : Math.round(s / 86400) + ' days'; };
  const fx = (v, d = 2) => (v == null || Number.isNaN(v) ? '–' : Number(v).toFixed(d));

  /* ---- widgets */
  const Pill = ({ s, children }) => h`<span class=${'pill ' + s}>${children || s}</span>`;
  const Chip = ({ cls, children, title }) => h`<span class="chip" title=${title || ''}><i class=${'dot ' + (cls || '')}></i>${children}</span>`;
  const Metric = ({ label, value, unit, d = 2, lim }) => {
    let c = '';
    if (lim && value != null) { if ((lim.lo != null && value < lim.lo) || (lim.hi != null && value > lim.hi)) c = 'bad'; }
    return h`<div class=${'metric ' + c}><small>${label}</small><span class="big" style="font-size:22px">${typeof value === 'number' ? fx(value, d) : (value ?? '–')}<span class="unit">${unit || ''}</span></span></div>`;
  };
  function Spark({ data, color = 'var(--acc)', lo, hi, h: hh = 46 }) {
    if (!data || data.length < 2) return h`<svg class="spark" viewBox="0 0 200 46"><text x="4" y="26" fill="var(--dim)" font-size="11">no data yet</text></svg>`;
    const mn = Math.min(...data), mx = Math.max(...data), sp = mx - mn || 1;
    const pts = data.map((v, i) => `${(i / (data.length - 1)) * 200},${42 - ((v - mn) / sp) * 38}`).join(' ');
    return h`<svg class="spark" viewBox="0 0 200 46" preserveAspectRatio="none"><polyline points=${pts} fill="none" stroke=${color} stroke-width="1.6" vector-effect="non-scaling-stroke" />
      ${mx !== mn && h`<text x="2" y="10" fill="var(--dim)" font-size="9">${fx(mx)}</text>`}${mx !== mn && h`<text x="2" y="45" fill="var(--dim)" font-size="9">${fx(mn)}</text>`}</svg>`;
  }

  /* ---- attitude: orientation of the body relative to the measured magnetic field (the only reference a magnetometer gives) */
  function attitude(mag) {
    const n = Math.hypot(...mag);
    if (!n) return null;
    const ez = mag.map(v => v / n);
    let x = [1, 0, 0]; const dx = ez[0];
    if (Math.abs(dx) > 0.95) x = [0, 1, 0];
    const dot = x[0] * ez[0] + x[1] * ez[1] + x[2] * ez[2];
    let ex = x.map((v, i) => v - dot * ez[i]); const ne = Math.hypot(...ex); ex = ex.map(v => v / ne);
    const ey = [ez[1] * ex[2] - ez[2] * ex[1], ez[2] * ex[0] - ez[0] * ex[2], ez[0] * ex[1] - ez[1] * ex[0]];
    // R: world<-body (rows ex, ey, ez; right handed). CSS axes are left handed (x right, y down, z at the viewer), so the cube is built
    // in P-converted coordinates and the displayed transform is T = P R P with P = [[1,0,0],[0,0,-1],[0,-1,0]] (world z up, world y away).
    const R = [ex, ey, ez], P = [[1, 0, 0], [0, 0, -1], [0, -1, 0]];
    const mul = (A, B) => A.map(r => [0, 1, 2].map(j => r[0] * B[0][j] + r[1] * B[1][j] + r[2] * B[2][j]));
    const T = mul(mul(P, R), P);
    const m3 = [T[0][0], T[1][0], T[2][0], 0, T[0][1], T[1][1], T[2][1], 0, T[0][2], T[1][2], T[2][2], 0, 0, 0, 0, 1];
    const ang = ez.map(v => Math.acos(Math.max(-1, Math.min(1, v))) * 180 / Math.PI);
    return { matrix: 'matrix3d(' + m3.join(',') + ')', angles: ang, magnitude: n, unit: ez };
  }
  function Cube({ mag, size = 300 }) {
    const a = mag ? attitude(mag) : null;
    const faces = [['+X', 'rotateY(90deg)'], ['-X', 'rotateY(-90deg)'], ['+Y', 'rotateY(180deg)'], ['-Y', ''], ['+Z', 'rotateX(90deg)'], ['-Z', 'rotateX(-90deg)']];
    if (!a) return h`<div class="stage" style=${'height:' + size + 'px'}><div class="hint">no magnetometer data yet</div></div>`;
    return h`<div class="stage" style=${'height:' + size + 'px'}>
      <div class="scene" style="transform:rotateX(-18deg) rotateY(28deg)">
        <div class="axis"><div class="bfield"></div></div>
        <div class="cube" style=${'transform:' + (a ? a.matrix : 'none')}>
          ${faces.map(([l, t]) => h`<div class="face" key=${l} style=${'transform:' + t + ' translateZ(50px)'}>${l}</div>`)}
        </div></div>
    </div>`;
  }

  /* a page that throws must not take the whole app down */
  class Boundary extends React.Component {
    constructor(p) { super(p); this.state = { err: null }; }
    static getDerivedStateFromError(err) { return { err }; }
    componentDidUpdate(prev) { if (prev.resetKey !== this.props.resetKey && this.state.err) this.setState({ err: null }); }
    render() { return this.state.err ? h`<div class="card" style="border-color:var(--bad)"><b style="color:var(--bad)">This page failed: ${String(this.state.err.message || this.state.err)}</b><div class="hint">Reload the page; the other pages keep working.</div></div>` : this.props.children; }
  }
  window.UI = { Boundary, memo: React.memo, h, utc, utcd, age, fx, Pill, Chip, Metric, Spark, Cube, attitude };
})();
