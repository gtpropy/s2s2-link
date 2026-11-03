/* World map (canvas, equirectangular, true 2:1 proportions): Natural Earth land (1:50m), country borders, graticule, day / night, ground track
 * with the part in contact with the ground station highlighted, the ground station and its 5 degree limit, the footprint and the satellite.
 * The satellite is propagated for every frame (about 30 per second). The static layer is drawn once per size. Geography data: static/geo.json
 * (built by build_geo.py from Natural Earth, public domain). */
(function () {
  const { h, useRef, useEffect } = MC;
  const D2R = Math.PI / 180, RE = 6371;

  /* ---------------------------------------------------------------- geography */
  let geo = null, geoLoading = false, geoCbs = [];
  function loadGeo(cb) {
    if (geo) return cb();
    geoCbs.push(cb);
    if (geoLoading) return;
    geoLoading = true;
    fetch('/geo.json').then(r => r.json()).then(j => {
      for (const c of j.countries.concat(j.seas)) c.box = c.p.map(poly => { let a = 1e9, b = 1e9, c2 = -1e9, d = -1e9; for (let i = 0; i < poly[0].length; i += 2) { a = Math.min(a, poly[0][i]); c2 = Math.max(c2, poly[0][i]); b = Math.min(b, poly[0][i + 1]); d = Math.max(d, poly[0][i + 1]); } return [a, b, c2, d]; });
      geo = j; geoLoading = false; geoCbs.splice(0).forEach(f => f());
    }).catch(() => { geoLoading = false; });
  }
  /* ray casting on a flat ring [x, y, x, y, ...] */
  const inRing = (r, x, y) => { let c = false; for (let i = 0, j = r.length - 2; i < r.length; j = i, i += 2) { if ((r[i + 1] > y) !== (r[j + 1] > y) && x < (r[j] - r[i]) * (y - r[i + 1]) / (r[j + 1] - r[i + 1]) + r[i]) c = !c; } return c; };
  const inPoly = (poly, x, y) => { if (!inRing(poly[0], x, y)) return false; for (let k = 1; k < poly.length; k++) if (inRing(poly[k], x, y)) return false; return true; };
  const landAt = (lon, lat) => (geo ? geo.land.some(p => inPoly(p, lon, lat)) : null);
  function hit(list, lon, lat) {                                 // the smallest polygon that contains the point (a sea inside an ocean wins)
    let best = null, area = 1e9;
    for (const c of list) c.p.forEach((poly, i) => { const b = c.box[i]; if (lon < b[0] || lon > b[2] || lat < b[1] || lat > b[3]) return; const a = (b[2] - b[0]) * (b[3] - b[1]); if (a < area && inPoly(poly, lon, lat)) { best = c; area = a; } });
    return best;
  }
  /* "over Kazakhstan", "over the North Pacific Ocean", "near Italy" (coast: the country outline is coarser than the land outline) */
  function placeAt(lon, lat) {
    if (!geo) return null;
    const c = hit(geo.countries, lon, lat);
    if (c) return 'over ' + c.n;
    if (landAt(lon, lat)) {
      let best = null, d2 = 9;
      for (const k of geo.countries) for (const poly of k.p) for (let i = 0; i < poly[0].length; i += 2) { const d = (poly[0][i] - lon) ** 2 + (poly[0][i + 1] - lat) ** 2; if (d < d2) { d2 = d; best = k; } }
      return best ? 'near ' + best.n : 'over land';
    }
    const s = hit(geo.seas, lon, lat);
    return s ? 'over the ' + s.n : 'over the ocean';
  }
  window.Geo = { load: loadGeo, placeAt, landAt };
  window.placeAt = placeAt; window.landAt = landAt;
  loadGeo(() => {});

  const cssMemo = { t: 0, el: null, v: {} };
  const css = (el, n, d) => {                                    // computed styles force a layout: read them at most once a second
    const t = Date.now();
    if (cssMemo.el !== el || t - cssMemo.t > 1000) { cssMemo.el = el; cssMemo.t = t; cssMemo.v = {}; }
    if (!(n in cssMemo.v)) cssMemo.v[n] = getComputedStyle(el).getPropertyValue(n).trim() || d;
    return cssMemo.v[n];
  };
  /* the largest 2:1 rectangle that fits, centred */
  const frame = (w, hgt) => { const mw = Math.min(w, hgt * 2), mh = mw / 2; return { ox: (w - mw) / 2, oy: (hgt - mh) / 2, mw, mh }; };

  /* sea, graticule, land, borders: redrawn only when the size or the theme changes */
  function baseLayer(cv, w, hgt, dpr) {
    const b = document.createElement('canvas');
    b.width = Math.round(w * dpr); b.height = Math.round(hgt * dpr);
    const g = b.getContext('2d');
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    const { ox, oy, mw, mh } = frame(w, hgt);
    const X = lon => ox + ((lon + 180) / 360) * mw, Y = lat => oy + ((90 - lat) / 180) * mh;
    g.fillStyle = css(cv, '--sea', '#0b1426'); g.fillRect(ox, oy, mw, mh);
    g.save(); g.beginPath(); g.rect(ox, oy, mw, mh); g.clip();
    g.strokeStyle = css(cv, '--grid', 'rgba(255,255,255,.06)'); g.lineWidth = 1; g.beginPath();
    for (let lo = -180; lo <= 180; lo += 15) { g.moveTo(X(lo), oy); g.lineTo(X(lo), oy + mh); }
    for (let la = -75; la <= 75; la += 15) { g.moveTo(ox, Y(la)); g.lineTo(ox + mw, Y(la)); }
    g.stroke();
    g.setLineDash([5, 5]); g.strokeStyle = css(cv, '--coast', '#38598a'); g.globalAlpha = 0.55; g.beginPath();
    for (const la of [0, 23.44, -23.44, 66.56, -66.56]) { g.moveTo(ox, Y(la)); g.lineTo(ox + mw, Y(la)); }
    g.stroke(); g.setLineDash([]); g.globalAlpha = 1;
    if (geo) {
      g.fillStyle = css(cv, '--land', '#1b3354'); g.strokeStyle = css(cv, '--coast', '#38598a'); g.lineWidth = 0.7;
      for (const poly of geo.land) {
        g.beginPath();
        for (const r of poly) { for (let i = 0; i < r.length; i += 2) (i ? g.lineTo(X(r[i]), Y(r[i + 1])) : g.moveTo(X(r[i]), Y(r[i + 1]))); g.closePath(); }
        g.fill('evenodd'); g.stroke();
      }
      g.globalAlpha = 0.6; g.lineWidth = 0.6; g.beginPath();
      for (const l of geo.borders) for (let i = 0; i < l.length; i += 2) (i ? g.lineTo(X(l[i]), Y(l[i + 1])) : g.moveTo(X(l[i]), Y(l[i + 1])));
      g.stroke(); g.globalAlpha = 1;
    }
    g.restore();
    g.fillStyle = css(cv, '--dim', '#8d9ab6'); g.font = '10px system-ui,sans-serif';
    for (let lo = -150; lo <= 150; lo += 30) g.fillText(Math.abs(lo) + '°' + (lo < 0 ? 'W' : lo > 0 ? 'E' : ''), X(lo) + 3, oy + mh - 4);
    for (let la = -60; la <= 60; la += 30) if (la) g.fillText(Math.abs(la) + '°' + (la > 0 ? 'N' : 'S'), ox + 3, Y(la) - 3);
    g.strokeStyle = css(cv, '--line', '#243049'); g.lineWidth = 1; g.strokeRect(ox + .5, oy + .5, mw - 1, mh - 1);
    return b;
  }

  const cache = { base: null, key: '', track: null, trackAt: 0 };
  const view = { ox: 0, oy: 0, mw: 1, mh: 1 };

  function draw(cv, big) {
    const dpr = window.devicePixelRatio || 1;
    const w = cv.clientWidth, hgt = cv.clientHeight;
    if (!w || !hgt) return;
    if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(hgt * dpr)) { cv.width = Math.round(w * dpr); cv.height = Math.round(hgt * dpr); }
    const key = [w, hgt, dpr, !!geo, css(cv, '--sea', ''), css(cv, '--land', '')].join();
    if (!cache.base || cache.key !== key) { cache.base = baseLayer(cv, w, hgt, dpr); cache.key = key; }
    const g = cv.getContext('2d');
    g.setTransform(1, 0, 0, 1, 0, 0);
    g.clearRect(0, 0, cv.width, cv.height);
    g.drawImage(cache.base, 0, 0);
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    const { ox, oy, mw, mh } = frame(w, hgt);
    Object.assign(view, { ox, oy, mw, mh });
    const X = lon => ox + ((lon + 180) / 360) * mw, Y = lat => oy + ((90 - lat) / 180) * mh;
    const O = Orbit, st = O.st, now = new Date();
    const warn = css(cv, '--warn', '#f7b84b'), ok = css(cv, '--ok', '#38d39f'), fg = css(cv, '--fg', '#fff');
    g.save(); g.beginPath(); g.rect(ox, oy, mw, mh); g.clip();
    g.font = '600 11px system-ui,sans-serif';
    if (!O.ok()) {
      g.fillStyle = warn; g.fillRect(X(st.lon) - 3.5, Y(st.lat) - 3.5, 7, 7);
      g.restore();
      g.fillStyle = fg; g.font = '600 14px system-ui,sans-serif'; g.textAlign = 'center';
      g.fillText('No orbit data yet. Enter the NORAD number under Settings › Satellite orbit.', w / 2, hgt / 2); g.textAlign = 'left';
      return;
    }
    // night side (terminator from the Sun's declination)
    const sun = O.sunSub(now), dec = sun.lat * D2R || 1e-6;
    g.fillStyle = 'rgba(0,0,0,.34)'; g.beginPath();
    const edge = dec > 0 ? oy + mh : oy;
    g.moveTo(ox, edge);
    for (let x = 0; x <= mw; x += 3) { const lon = (x / mw) * 360 - 180; g.lineTo(ox + x, Y(Math.atan(-Math.cos((lon - sun.lon) * D2R) / Math.tan(dec)) / D2R)); }
    g.lineTo(ox + mw, edge); g.closePath(); g.fill();
    g.fillStyle = warn; g.beginPath(); g.arc(X(sun.lon), Y(sun.lat), 4, 0, 7); g.fill();
    g.strokeStyle = warn; g.lineWidth = 1; g.beginPath();
    for (let a = 0; a < 8; a++) { g.moveTo(X(sun.lon) + Math.cos(a * Math.PI / 4) * 6.5, Y(sun.lat) + Math.sin(a * Math.PI / 4) * 6.5); g.lineTo(X(sun.lon) + Math.cos(a * Math.PI / 4) * 9, Y(sun.lat) + Math.sin(a * Math.PI / 4) * 9); }
    g.stroke();
    // ground track: flown solid, predicted dotted, the parts in contact with the ground station thick (recomputed every 5 s)
    if (!cache.track || now - cache.trackAt > 5000) {
      cache.track = O.track(now, big ? 60 : 50, big ? 180 : 100, 10).map(seg => seg.map(p => [p[0], p[1], p[2], O.look(new Date(+now + p[2] * 1000)).el > 0]));
      cache.trackAt = +now;
    }
    const nowT = (+now - cache.trackAt) / 1000;
    for (const seg of cache.track) {
      for (let i = 1; i < seg.length; i++) {
        const a = seg[i - 1], b = seg[i], past = b[2] - nowT < 0, inC = a[3] && b[3];
        g.strokeStyle = past ? ok : css(cv, '--trackfut', '#56d3f5'); g.lineWidth = inC ? 3 : 1.5; g.globalAlpha = inC ? 1 : 0.85;
        g.setLineDash(past || inC ? [] : [2, 4]);
        g.beginPath(); g.moveTo(X(a[0]), Y(a[1])); g.lineTo(X(b[0]), Y(b[1])); g.stroke();
      }
    }
    g.setLineDash([]); g.globalAlpha = 1;
    // ground station and its 5 degree limit
    const p = O.at(now);
    g.strokeStyle = warn; g.fillStyle = warn; g.lineWidth = 1;
    if (p) {
      const el = 5 * D2R, lam = Math.acos(RE * Math.cos(el) / (RE + p.alt)) - el;
      g.setLineDash([4, 4]); g.beginPath();
      let pen = false, prev = null;
      for (let b = 0; b <= 360; b += 3) {
        const br = b * D2R, la = st.lat * D2R, lo = st.lon * D2R;
        const la2 = Math.asin(Math.sin(la) * Math.cos(lam) + Math.cos(la) * Math.sin(lam) * Math.cos(br));
        const lo2 = lo + Math.atan2(Math.sin(br) * Math.sin(lam) * Math.cos(la), Math.cos(lam) - Math.sin(la) * Math.sin(la2));
        const x = ((lo2 / D2R + 540) % 360) - 180;
        if (!pen || (prev !== null && Math.abs(x - prev) > 180)) { g.moveTo(X(x), Y(la2 / D2R)); pen = true; } else g.lineTo(X(x), Y(la2 / D2R));
        prev = x;
      }
      g.stroke(); g.setLineDash([]);
    }
    g.fillRect(X(st.lon) - 3.5, Y(st.lat) - 3.5, 7, 7);
    g.fillText('GS', X(st.lon) + 8, Y(st.lat) + 4);
    if (!p) { g.restore(); return; }
    // footprint (area in line of sight of the satellite)
    g.strokeStyle = ok; g.fillStyle = 'rgba(52,201,151,.10)'; g.lineWidth = 1.4;
    for (const seg of O.footprint(p.lat, p.lon, p.alt)) {
      g.beginPath(); seg.forEach(([lo, la], i) => (i ? g.lineTo(X(lo), Y(la)) : g.moveTo(X(lo), Y(la)))); g.stroke();
      if (seg.length > 80) g.fill();
    }
    // satellite: crosshair marker, heading, soft pulse
    const x = X(p.lon), y = Y(p.lat), pn = O.at(new Date(+now + 10000));
    let ang = 0;
    if (pn) { let dl = pn.lon - p.lon; if (dl > 180) dl -= 360; if (dl < -180) dl += 360; ang = Math.atan2(-(pn.lat - p.lat), dl); }
    const t = (+now / 1400) % 1;
    g.strokeStyle = ok; g.globalAlpha = 1 - t; g.lineWidth = 1.5; g.beginPath(); g.arc(x, y, 8 + t * 18, 0, 7); g.stroke(); g.globalAlpha = 1;
    g.strokeStyle = fg; g.lineWidth = 1.3; g.beginPath(); g.arc(x, y, 7, 0, 7);
    for (const [dx, dy] of [[1, 0], [-1, 0], [0, 1], [0, -1]]) { g.moveTo(x + dx * 7, y + dy * 7); g.lineTo(x + dx * 12, y + dy * 12); }
    g.stroke();
    g.fillStyle = fg; g.beginPath(); g.arc(x, y, 2.6, 0, 7); g.fill();
    g.save(); g.translate(x, y); g.rotate(ang); g.fillStyle = ok; g.beginPath(); g.moveTo(21, 0); g.lineTo(15, -4); g.lineTo(15, 4); g.closePath(); g.fill(); g.restore();
    // position at the moment pointed at in a telemetry chart
    if (O.probe && O.probe.t != null) {
      const q = O.at(new Date(O.probe.t * 1000));
      if (q) {
        const px = X(q.lon), py = Y(q.lat);
        g.strokeStyle = warn; g.fillStyle = warn; g.lineWidth = 2; g.beginPath(); g.arc(px, py, 7, 0, 7); g.stroke();
        g.beginPath(); g.arc(px, py, 2.5, 0, 7); g.fill();
        g.font = '600 11px system-ui,sans-serif';
        g.fillText(new Date(O.probe.t * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }), px + 11, py - 9);
      }
    }
    g.restore();
    const name = ((O.tle && O.tle.name) || 'S2S2').replace(/\s*\(.*\)/, '');
    g.fillStyle = fg; g.font = '700 12px system-ui,sans-serif'; g.fillText(name, Math.min(x + 16, ox + mw - 70), Math.max(oy + 14, y - 8));
  }

  /* onCursor([lon, lat]) while the pointer is over the map, onCursor(null) when it leaves */
  function WorldMap({ big, height, onCursor }) {
    const ref = useRef(null);
    useEffect(() => {
      let on = true, raf = 0, last = 0;
      const frame2 = ts => {
        raf = requestAnimationFrame(frame2);
        if (!on || !ref.current || ts - last < 33 || document.hidden) return;       // about 30 frames per second
        last = ts;
        draw(ref.current, big);
      };
      loadGeo(() => { cache.key = ''; });
      raf = requestAnimationFrame(frame2);
      return () => { on = false; cancelAnimationFrame(raf); };
    }, [big]);
    const move = e => {
      if (!onCursor) return;
      const r = e.currentTarget.getBoundingClientRect(), x = e.clientX - r.left, y = e.clientY - r.top;
      const lon = (x - view.ox) / view.mw * 360 - 180, lat = 90 - (y - view.oy) / view.mh * 180;
      onCursor(lon >= -180 && lon <= 180 && lat >= -90 && lat <= 90 ? [lon, lat] : null);
    };
    return h`<canvas ref=${ref} class="worldmap" style=${height ? 'height:' + height : (big ? 'height:min(62vh,560px)' : '')} onMouseMove=${move} onMouseLeave=${() => onCursor && onCursor(null)} aria-label="World map with the satellite's position"></canvas>`;
  }
  window.WorldMap = WorldMap;
})();
