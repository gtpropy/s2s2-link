/* Orbit maths in the browser (satellite.js SGP4): where the satellite is now, its ground track and when it passes over the station. */
(function () {
  const D2R = Math.PI / 180, R2D = 180 / Math.PI, RE = 6371;
  const O = { rec: null, tle: null, st: { lat: 27.7172, lon: 85.324, alt: 1.4 }, ok() { return !!this.rec; } };
  let passCache = {};

  O.set = function (tle, station) {
    if (station) O.st = { name: station.name, lat: station.lat, lon: station.lon, alt: station.alt || 0 };
    passCache = {};
    O.tle = tle || null;
    try { O.rec = tle && tle.line1 ? satellite.twoline2satrec(tle.line1, tle.line2) : null; } catch (e) { O.rec = null; }
  };

  /* position of the satellite at a date: lat/lon (deg), altitude (km), speed (km/s), ecf for look angles */
  O.at = function (date) {
    if (!O.rec) return null;
    const pv = satellite.propagate(O.rec, date);
    if (!pv.position || typeof pv.position.x !== 'number') return null;
    const gmst = satellite.gstime(date);
    const g = satellite.eciToGeodetic(pv.position, gmst);
    const v = pv.velocity;
    return { lat: satellite.degreesLat(g.latitude), lon: satellite.degreesLong(g.longitude), alt: g.height, speed: Math.hypot(v.x, v.y, v.z), ecf: satellite.eciToEcf(pv.position, gmst) };
  };

  /* elevation / azimuth / range seen from the station */
  O.look = function (date) {
    const p = O.at(date);
    if (!p) return null;
    const obs = { latitude: O.st.lat * D2R, longitude: O.st.lon * D2R, height: O.st.alt || 0 };
    const la = satellite.ecfToLookAngles(obs, p.ecf);
    return { el: la.elevation * R2D, az: la.azimuth * R2D, range: la.rangeSat, pos: p };
  };

  /* ground track from -back to +fwd minutes, split where the line crosses the date line */
  O.track = function (date, back = 45, fwd = 90, stepS = 30) {
    const segs = [];
    let cur = [], prev = null;
    for (let s = -back * 60; s <= fwd * 60; s += stepS) {
      const p = O.at(new Date(date.getTime() + s * 1000));
      if (!p) continue;
      if (prev !== null && Math.abs(p.lon - prev) > 180) { segs.push(cur); cur = []; }
      cur.push([p.lon, p.lat, s]);
      prev = p.lon;
    }
    if (cur.length) segs.push(cur);
    return segs;
  };

  /* circle on the ground that can see the satellite (horizon), as polylines split at the date line */
  O.footprint = function (lat, lon, altKm) {
    const lam = Math.acos(RE / (RE + altKm));
    const la = lat * D2R, lo = lon * D2R;
    const segs = [];
    let cur = [], prev = null;
    for (let b = 0; b <= 360; b += 4) {
      const br = b * D2R;
      const la2 = Math.asin(Math.sin(la) * Math.cos(lam) + Math.cos(la) * Math.sin(lam) * Math.cos(br));
      const lo2 = lo + Math.atan2(Math.sin(br) * Math.sin(lam) * Math.cos(la), Math.cos(lam) - Math.sin(la) * Math.sin(la2));
      let x = ((lo2 * R2D + 540) % 360) - 180;
      if (prev !== null && Math.abs(x - prev) > 180) { segs.push(cur); cur = []; }
      cur.push([x, la2 * R2D]);
      prev = x;
    }
    segs.push(cur);
    return segs;
  };

  /* moment (unix s) when the satellite crosses the horizon between two sample times, refined by bisection to about 0.2 s */
  const horizon = (a, b) => {
    let lo = a, hi = b;
    const up0 = O.look(new Date(lo)).el > 0;
    for (let i = 0; i < 7; i++) { const m = (lo + hi) / 2; if ((O.look(new Date(m)).el > 0) === up0) lo = m; else hi = m; }
    return (lo + hi) / 2000;
  };
  /* passes above minEl degrees between from and from+hours; AOS / LOS are the exact horizon crossings; cached per minute */
  O.passes = function (from, hours = 24, minEl = 5) {
    if (!O.rec) return [];
    const key = Math.floor(from.getTime() / 60000) + ':' + hours + ':' + minEl;
    if (passCache[key]) return passCache[key];
    const out = [];
    let cur = null, prevT = null;
    const step = 20 * 1000, end = from.getTime() + hours * 3600 * 1000;
    for (let t = from.getTime(); t <= end; t += step) {
      const l = O.look(new Date(t));
      if (!l) continue;
      if (l.el > 0) {
        if (!cur) cur = { aos: prevT === null ? t / 1000 : horizon(prevT, t), maxEl: l.el, maxT: t / 1000, azIn: l.az };
        if (l.el > cur.maxEl) { cur.maxEl = l.el; cur.maxT = t / 1000; }
        cur.azOut = l.az; cur.los = t / 1000;
      } else if (cur) { cur.los = horizon(prevT, t); if (cur.maxEl >= minEl) out.push(cur); cur = null; }
      prevT = t;
    }
    if (cur && cur.maxEl >= minEl) out.push(cur);
    if (Object.keys(passCache).length > 6) passCache = {};
    passCache[key] = out;
    return out;
  };
  /* the pass in progress or the next one (searches from 25 minutes back so that the true AOS of a running pass is known) */
  O.nextPass = function (nowS) {
    return O.passes(new Date(Math.floor((nowS * 1000 - 25 * 60000) / 60000) * 60000), 26).find(p => p.los > nowS) || null;
  };

  /* passes whose start falls on today's date (local time) */
  O.passesToday = function (now = new Date()) {
    const d0 = new Date(now); d0.setHours(0, 0, 0, 0);
    const all = O.passes(d0, 24);
    return all;
  };

  /* TLE epoch (unix s) and how many days a time lies from it */
  O.tleEpoch = function () {
    if (!O.tle || !O.tle.line1) return null;
    const yy = parseInt(O.tle.line1.slice(18, 20), 10), day = parseFloat(O.tle.line1.slice(20, 32));
    return (Date.UTC(yy < 57 ? 2000 + yy : 1900 + yy, 0, 1) + (day - 1) * 86400000) / 1000;
  };
  O.tleEpochOffsetDays = t => { const e = O.tleEpoch(); return e == null ? null : (t - e) / 86400; };
  O.compass = az => ['N', 'NE', 'E', 'SE', 'S', 'SW', 'W', 'NW'][Math.round(az / 45) % 8];
  /* age of the TLE in days (epoch is in line 1, columns 19-32) */
  O.tleAgeDays = function () {
    if (!O.tle || !O.tle.line1) return null;
    const yy = parseInt(O.tle.line1.slice(18, 20), 10), day = parseFloat(O.tle.line1.slice(20, 32));
    const year = yy < 57 ? 2000 + yy : 1900 + yy;
    const ep = Date.UTC(year, 0, 1) + (day - 1) * 86400000;
    return (Date.now() - ep) / 86400000;
  };

  /* ---- inertial (ECI, km) state, Sun direction and a geomagnetic dipole model: the inputs of the 3D view and of the attitude estimate ---- */
  O.eci = function (date) {
    if (!O.rec) return null;
    const pv = satellite.propagate(O.rec, date);
    if (!pv.position || typeof pv.position.x !== 'number') return null;
    return { r: [pv.position.x, pv.position.y, pv.position.z], v: [pv.velocity.x, pv.velocity.y, pv.velocity.z], gmst: satellite.gstime(date) };
  };
  O.sunEci = function (date) {                      // low-precision solar position (about 0.01 deg), unit vector
    const n = date.getTime() / 86400000 + 2440587.5 - 2451545.0;
    const L = (280.460 + 0.9856474 * n) * D2R, g = (357.528 + 0.9856003 * n) * D2R;
    const lam = L + (1.915 * Math.sin(g) + 0.020 * Math.sin(2 * g)) * D2R, eps = (23.439 - 0.0000004 * n) * D2R;
    return [Math.cos(lam), Math.cos(eps) * Math.sin(lam), Math.sin(eps) * Math.sin(lam)];
  };
  const dot = (a, b) => a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  const cross = (a, b) => [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]];
  const nrm = a => { const l = Math.hypot(a[0], a[1], a[2]) || 1; return [a[0] / l, a[1] / l, a[2] / l]; };
  /* centred-dipole field at an ECI position (IGRF-13 dipole: geomagnetic north pole 80.65 N, 72.68 W; B0 = 29.4 uT): returns {dir (unit, ECI), uT} */
  O.dipoleField = function (r, gmst) {
    const rm = Math.hypot(r[0], r[1], r[2]), rh = [r[0] / rm, r[1] / rm, r[2] / rm];
    const lat = 80.65 * D2R, lon = -72.68 * D2R;
    const m = [-Math.cos(lat) * Math.cos(lon), -Math.cos(lat) * Math.sin(lon), -Math.sin(lat)];     // moment points to the geomagnetic SOUTH pole
    // rotate the ECEF dipole axis to ECI
    const c = Math.cos(gmst), s = Math.sin(gmst), me = [m[0] * c - m[1] * s, m[0] * s + m[1] * c, m[2]];
    const k = 29.4 * Math.pow(RE / rm, 3), d = dot(me, rh);
    const B = [k * (3 * d * rh[0] - me[0]), k * (3 * d * rh[1] - me[1]), k * (3 * d * rh[2] - me[2])];
    return { dir: nrm(B), uT: Math.hypot(B[0], B[1], B[2]) };
  };
  /* Attitude estimate from ONE vector observation (magnetometer, body frame) and the modelled field (ECI). One vector fixes two of the three rotation angles:
   * start from the nominal nadir-pointing frame (z = nadir, y = -orbit normal, x = completes) and apply the smallest rotation that aligns the measured field with the model.
   * Returns R (body -> ECI, rows) and the angle that rotation needed; rotation about the field direction is NOT observable. */
  O.attitude = function (date, magBody) {
    const st = O.eci(date);
    if (!st || !magBody) return null;
    const bm = Math.hypot(magBody[0], magBody[1], magBody[2]);
    if (!bm) return null;
    const zb = nrm(st.r).map(v => -v), yb = nrm(cross(st.r, st.v)).map(v => -v), xb = cross(yb, zb);
    const R0 = [[xb[0], yb[0], zb[0]], [xb[1], yb[1], zb[1]], [xb[2], yb[2], zb[2]]];       // columns = body axes in ECI
    const mulv = (R, v) => [R[0][0] * v[0] + R[0][1] * v[1] + R[0][2] * v[2], R[1][0] * v[0] + R[1][1] * v[1] + R[1][2] * v[2], R[2][0] * v[0] + R[2][1] * v[1] + R[2][2] * v[2]];
    const bw = nrm(mulv(R0, nrm(magBody))), Bm = O.dipoleField(st.r, st.gmst);
    let ax = cross(bw, Bm.dir); const sn = Math.hypot(ax[0], ax[1], ax[2]), cs = Math.max(-1, Math.min(1, dot(bw, Bm.dir)));
    let R = R0;
    if (sn > 1e-9) {
      ax = [ax[0] / sn, ax[1] / sn, ax[2] / sn];
      const K = [[0, -ax[2], ax[1]], [ax[2], 0, -ax[0]], [-ax[1], ax[0], 0]], I = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
      const Q = I.map((row, i) => row.map((_, j) => I[i][j] + sn * K[i][j] + (1 - cs) * (K[i][0] * K[0][j] + K[i][1] * K[1][j] + K[i][2] * K[2][j])));       // Rodrigues
      R = Q.map((row, i) => [0, 1, 2].map(j => row[0] * R0[0][j] + row[1] * R0[1][j] + row[2] * R0[2][j]));
    }
    return { R, offsetDeg: Math.atan2(sn, cs) * R2D, fieldUT: Bm.uT, nadir: zb, st };
  };

  /* sub-solar point (deg) for the day / night shading of the map */
  O.sunSub = function (date) {
    const s = O.sunEci(date), g = satellite.gstime(date);
    let lon = (Math.atan2(s[1], s[0]) - g) * R2D;
    lon = ((lon + 540) % 360) - 180;
    return { lat: Math.asin(s[2]) * R2D, lon };
  };
  /* is the satellite in sunlight (outside the cylindrical Earth shadow)? */
  O.sunlit = function (date) {
    const e = O.eci(date);
    if (!e) return null;
    const sun = O.sunEci(date), along = dot(e.r, sun);
    return !(along < 0 && Math.hypot(e.r[0] - along * sun[0], e.r[1] - along * sun[1], e.r[2] - along * sun[2]) < RE);
  };
  /* orbit figures from the TLE for the orbit panel: altitude, inclination, period, speed, beta angle, eclipse time of one revolution, RAAN, eccentricity */
  O.stats = function (date, withEclipse) {
    const st = O.eci(date), p = O.at(date);
    if (!st || !p) return null;
    const rec = O.rec, sun = O.sunEci(date);
    const beta = Math.asin(dot(sun, nrm(cross(st.r, st.v)))) * R2D;
    const period = 2 * Math.PI / rec.no;                                       // rec.no is rad/min
    let eclipse = null;
    if (withEclipse) {
      const stepS = 30, n = Math.round(period * 60 / stepS);
      let k = 0;
      for (let i = 0; i < n; i++) {
        const e = O.eci(new Date(date.getTime() + i * stepS * 1000));
        if (!e) continue;
        const along = dot(e.r, sun);
        if (along < 0 && Math.hypot(e.r[0] - along * sun[0], e.r[1] - along * sun[1], e.r[2] - along * sun[2]) < RE) k++;       // inside the cylindrical Earth shadow
      }
      eclipse = k * stepS / 60;
    }
    return { alt: p.alt, lat: p.lat, lon: p.lon, speed: p.speed, incl: rec.inclo * R2D, period, beta, eclipse, raan: rec.nodeo * R2D, ecc: rec.ecco };
  };
  window.Orbit = O;
})();
