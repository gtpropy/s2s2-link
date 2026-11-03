/* 3D view: the Earth (NASA Blue Marble texture, Sun-lit, rotated by Greenwich sidereal time), the satellite at its propagated position, its orbit, the coverage circle,
 * the ground station and the satellite body with its estimated attitude. Scene units: 1 = Earth radius. ECI (x, y, z) maps to scene (x, z, -y). */
(function () {
  const { h, useRef, useEffect, useState, S } = MC;
  const RE = 6371, D2R = Math.PI / 180;
  const toScene = (e, k) => new THREE.Vector3(e[0] * k, e[2] * k, -e[1] * k);
  /* point on the unit sphere in the Earth's own frame, matching the way SphereGeometry maps the equirectangular texture */
  function onEarth(lat, lon, r) {
    const phi = (lon + 180) * D2R, th = (90 - lat) * D2R;
    return new THREE.Vector3(-r * Math.cos(phi) * Math.sin(th), r * Math.cos(th), r * Math.sin(phi) * Math.sin(th));
  }
  const glow = (color) => {
    const c = document.createElement('canvas'); c.width = c.height = 64;
    const g = c.getContext('2d'), gr = g.createRadialGradient(32, 32, 2, 32, 32, 32);
    gr.addColorStop(0, color); gr.addColorStop(1, 'rgba(0,0,0,0)');
    g.fillStyle = gr; g.fillRect(0, 0, 64, 64);
    return new THREE.CanvasTexture(c);
  };

  function build(canvas, ctl, onInfo, onFail) {
    let renderer;
    try {
      if (!window.THREE) throw new Error('3D library not loaded');
      renderer = new THREE.WebGLRenderer({ canvas, antialias: true });
    } catch (e) { onFail(String(e.message || e)); return () => {}; }
    renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    const scene = new THREE.Scene();
    scene.background = new THREE.Color(0x02050c);
    const camera = new THREE.PerspectiveCamera(38, 1, 0.003, 400);

    // lighting: the Sun is a directional light, the night side keeps a faint fill
    const sun = new THREE.DirectionalLight(0xffffff, 1.25);
    scene.add(sun, new THREE.AmbientLight(0x2a3a5c, 0.55));

    // Earth
    const earthGroup = new THREE.Group();
    const tex = new THREE.TextureLoader().load('/earth_2048.jpg');
    tex.anisotropy = renderer.capabilities.getMaxAnisotropy();
    const earth = new THREE.Mesh(new THREE.SphereGeometry(1, 128, 96), new THREE.MeshPhongMaterial({ map: tex, shininess: 8, specular: 0x182030 }));
    earthGroup.add(earth);
    const grat = [];
    for (let la = -60; la <= 60; la += 30) { for (let lo = -180; lo < 180; lo += 4) grat.push(onEarth(la, lo, 1.001), onEarth(la, lo + 4, 1.001)); }
    for (let lo = -180; lo < 180; lo += 30) { for (let la = -88; la < 88; la += 4) grat.push(onEarth(la, lo, 1.001), onEarth(la + 4, lo, 1.001)); }
    earthGroup.add(new THREE.LineSegments(new THREE.BufferGeometry().setFromPoints(grat), new THREE.LineBasicMaterial({ color: 0xffffff, transparent: true, opacity: 0.12 })));
    const station = new THREE.Mesh(new THREE.SphereGeometry(0.012, 16, 12), new THREE.MeshBasicMaterial({ color: 0xf7b84b }));
    earthGroup.add(station);
    const footLine = new THREE.LineLoop(new THREE.BufferGeometry(), new THREE.LineBasicMaterial({ color: 0x38d39f }));
    earthGroup.add(footLine);
    scene.add(earthGroup);
    const atmo = new THREE.Mesh(new THREE.SphereGeometry(1.035, 64, 48), new THREE.ShaderMaterial({
      vertexShader: 'varying vec3 vN; void main(){ vN = normalize(normalMatrix * normal); gl_Position = projectionMatrix * modelViewMatrix * vec4(position,1.0); }',
      fragmentShader: 'varying vec3 vN; void main(){ float i = pow(0.72 - dot(vN, vec3(0.0,0.0,1.0)), 3.5); gl_FragColor = vec4(0.35,0.62,1.0,1.0) * i; }',
      side: THREE.BackSide, blending: THREE.AdditiveBlending, transparent: true, depthWrite: false }));
    scene.add(atmo);
    const stars = []; for (let i = 0; i < 1800; i++) { const u = Math.random() * 2 - 1, t = Math.random() * 6.283, s = Math.sqrt(1 - u * u); stars.push(120 * s * Math.cos(t), 120 * u, 120 * s * Math.sin(t)); }
    scene.add(new THREE.Points(new THREE.BufferGeometry().setAttribute('position', new THREE.Float32BufferAttribute(stars, 3)), new THREE.PointsMaterial({ size: 0.35, color: 0xcfd8ee, sizeAttenuation: true })));

    // orbit (inertial path, past and future) and the line to the sub-satellite point
    const pastLine = new THREE.Line(new THREE.BufferGeometry(), new THREE.LineBasicMaterial({ color: 0x4da3ff, transparent: true, opacity: 0.45 }));
    const futLine = new THREE.Line(new THREE.BufferGeometry(), new THREE.LineBasicMaterial({ color: 0x4fd8ff }));
    const nadirLine = new THREE.Line(new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(), new THREE.Vector3()]), new THREE.LineBasicMaterial({ color: 0xffffff, transparent: true, opacity: 0.35 }));
    scene.add(pastLine, futLine, nadirLine);

    // satellite: body, solar wings, antenna and body axes (exaggerated size, about 320 km across)
    const sat = new THREE.Group(), model = new THREE.Group();
    const body = new THREE.Mesh(new THREE.BoxGeometry(0.5, 0.5, 0.8), new THREE.MeshStandardMaterial({ color: 0xb9c2d4, metalness: 0.6, roughness: 0.4 }));
    const wingMat = new THREE.MeshStandardMaterial({ color: 0x1f4fa8, metalness: 0.3, roughness: 0.5, emissive: 0x0a1c44 });
    const w1 = new THREE.Mesh(new THREE.BoxGeometry(0.03, 1.1, 0.7), wingMat), w2 = w1.clone();
    w1.position.y = 0.85; w2.position.y = -0.85;
    const ant = new THREE.Line(new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(0, 0, 0.4), new THREE.Vector3(0, 0, 1.2)]), new THREE.LineBasicMaterial({ color: 0xffd37a }));
    model.add(body, w1, w2, ant);
    const axes = new THREE.Group();
    [[1, 0, 0, 0xff5c6c], [0, 1, 0, 0x38d39f], [0, 0, 1, 0x4da3ff]].forEach(([x, y, z, c]) => axes.add(new THREE.ArrowHelper(new THREE.Vector3(x, y, z), new THREE.Vector3(), 1.6, c, 0.3, 0.16)));
    model.add(axes);
    model.scale.setScalar(0.045);
    const halo = new THREE.Sprite(new THREE.SpriteMaterial({ map: glow('rgba(79,216,255,0.95)'), depthTest: false, transparent: true }));
    halo.scale.setScalar(0.1);
    sat.add(model, halo);
    scene.add(sat);

    // camera state
    const cam = { mode: 'earth', dist: 2.7, dAz: 0, dEl: 0.25, saz: 0.8, sel: 0.5, sdist: 0.32, az: 0, el: 0.3 };
    ctl.cam = cam;
    function size() {
      const w = canvas.clientWidth, hh = canvas.clientHeight;
      if (!w || !hh) return;
      renderer.setSize(w, hh, false);
      camera.aspect = w / hh; camera.updateProjectionMatrix();
    }
    const ro = new ResizeObserver(size); ro.observe(canvas); size();

    // pointer control: drag = rotate, wheel / pinch = zoom
    const ptrs = new Map(); let pinch = 0;
    const zoom = f => { if (cam.mode === 'sat') cam.sdist = Math.min(3, Math.max(0.08, cam.sdist * f)); else cam.dist = Math.min(14, Math.max(1.35, cam.dist * f)); };
    canvas.addEventListener('pointerdown', e => { canvas.setPointerCapture(e.pointerId); ptrs.set(e.pointerId, [e.clientX, e.clientY]); });
    canvas.addEventListener('pointerup', e => { ptrs.delete(e.pointerId); pinch = 0; });
    canvas.addEventListener('pointermove', e => {
      if (!ptrs.has(e.pointerId)) return;
      const p = ptrs.get(e.pointerId), dx = e.clientX - p[0], dy = e.clientY - p[1];
      ptrs.set(e.pointerId, [e.clientX, e.clientY]);
      if (ptrs.size === 2) { const a = [...ptrs.values()], d = Math.hypot(a[0][0] - a[1][0], a[0][1] - a[1][1]); if (pinch) zoom(pinch / d); pinch = d; return; }
      if (cam.mode === 'sat') { cam.saz -= dx * 0.008; cam.sel = Math.max(-1.5, Math.min(1.5, cam.sel + dy * 0.008)); }
      else { cam.dAz -= dx * 0.006; cam.dEl = Math.max(-1.3, Math.min(1.3, cam.dEl + dy * 0.006)); }
    });
    canvas.addEventListener('wheel', e => { e.preventDefault(); zoom(Math.exp(e.deltaY * 0.0012)); }, { passive: false });

    let raf = 0, lastInfo = 0, lastOrbit = 0, orbitKey = '', alive = true;
    const T = (R) => { const m = new THREE.Matrix4(); // body -> scene: scene = T * ECI, T = [[1,0,0],[0,0,1],[0,-1,0]]
      m.set(R[0][0], R[0][1], R[0][2], 0, R[2][0], R[2][1], R[2][2], 0, -R[1][0], -R[1][1], -R[1][2], 0, 0, 0, 0, 1); return m; };

    function frame(nowMs) {
      if (!alive) return;
      raf = requestAnimationFrame(frame);
      const date = new Date(), O = Orbit;
      const st = O.ok() ? O.eci(date) : null;
      const gmst = st ? st.gmst : satellite.gstime(date);
      earthGroup.rotation.y = gmst;
      const sd = O.sunEci(date); sun.position.set(sd[0] * 60, sd[2] * 60, -sd[1] * 60);
      station.position.copy(onEarth(O.st.lat, O.st.lon, 1.004));
      if (st) {
        const P = toScene(st.r, 1 / RE);
        sat.position.copy(P);
        const geo = O.at(date);
        // orbit lines every 20 s of wall time
        const key = Math.floor(nowMs / 20000);
        if (key !== orbitKey) {
          orbitKey = key;
          const past = [], fut = [];
          for (let s = -50 * 60; s <= 100 * 60; s += 30) { const e = O.eci(new Date(date.getTime() + s * 1000)); if (!e) continue; (s <= 0 ? past : fut).push(toScene(e.r, 1 / RE)); }
          if (past.length) fut.unshift(past[past.length - 1]);
          pastLine.geometry.setFromPoints(past); futLine.geometry.setFromPoints(fut);
        }
        nadirLine.geometry.setFromPoints([P, P.clone().normalize().multiplyScalar(1.002)]);
        if (geo) {
          const pts = []; O.footprint(geo.lat, geo.lon, geo.alt).forEach(sg => sg.forEach(([lo, la]) => pts.push(onEarth(la, lo, 1.004))));
          footLine.geometry.setFromPoints(pts);
        }
        // attitude: estimate from the newest housekeeping magnetometer sample (uT) when there is one
        const im = MC.imu(S), mag = im && im.unit.m === 'µT' ? im.mag : null;
        const att = mag ? O.attitude(date, mag) : null;
        let R;
        if (att) R = att.R;
        else { const z = [-st.r[0], -st.r[1], -st.r[2]], zn = Math.hypot(...z), zz = z.map(v => v / zn);
          const nrmv = [st.r[1] * st.v[2] - st.r[2] * st.v[1], st.r[2] * st.v[0] - st.r[0] * st.v[2], st.r[0] * st.v[1] - st.r[1] * st.v[0]], nn = Math.hypot(...nrmv), yy = nrmv.map(v => -v / nn);
          const xx = [yy[1] * zz[2] - yy[2] * zz[1], yy[2] * zz[0] - yy[0] * zz[2], yy[0] * zz[1] - yy[1] * zz[0]];
          R = [[xx[0], yy[0], zz[0]], [xx[1], yy[1], zz[1]], [xx[2], yy[2], zz[2]]]; }
        model.setRotationFromMatrix(T(R));
        axes.visible = ctl.axes;
        // camera
        const target = new THREE.Vector3(), dist = cam.mode === 'sat' ? cam.sdist : cam.dist;
        let az, el;
        if (cam.mode === 'sat') { target.copy(P); az = cam.saz; el = cam.sel; }
        else if (cam.mode === 'earth') { const d = P.clone().normalize(); az = Math.atan2(d.x, d.z) + cam.dAz; el = Math.asin(d.y) * 0.6 + cam.dEl - 0.25; }
        else { az = cam.dAz + 0.6; el = cam.dEl; }
        camera.position.set(target.x + dist * Math.cos(el) * Math.sin(az), target.y + dist * Math.sin(el), target.z + dist * Math.cos(el) * Math.cos(az));
        camera.lookAt(target);
        pastLine.visible = futLine.visible = ctl.orbit;
        halo.scale.setScalar(Math.max(0.006, camera.position.distanceTo(P) * 0.028));
        if (nowMs - lastInfo > 500) {
          lastInfo = nowMs;
          const lk = O.look(date), r = Math.hypot(...st.r), sdot = st.r[0] * sd[0] + st.r[1] * sd[1] + st.r[2] * sd[2];
          const perp = Math.hypot(st.r[0] - sdot * sd[0], st.r[1] - sdot * sd[1], st.r[2] - sdot * sd[2]);
          onInfo({ lat: geo.lat, lon: geo.lon, alt: geo.alt, speed: geo.speed, el: lk && lk.el, az: lk && lk.az, range: lk && lk.range, eclipse: sdot < 0 && perp < RE, r,
            att: att ? { offsetDeg: att.offsetDeg, fieldUT: att.fieldUT, src: im.src, t: im.t } : null });
        }
      } else {
        sat.visible = false; camera.position.set(0, 0.8, cam.dist); camera.lookAt(0, 0, 0);
        if (nowMs - lastInfo > 500) { lastInfo = nowMs; onInfo(null); }
      }
      if (st) sat.visible = true;
      renderer.render(scene, camera);
    }
    raf = requestAnimationFrame(frame);
    return () => { alive = false; cancelAnimationFrame(raf); ro.disconnect(); renderer.dispose(); };
  }

  /* props: mode ('earth' | 'sat' | 'free'), orbit (bool), axes (bool), onInfo(info|null) */
  function Globe3D({ mode, orbit, axes, onInfo, onFail }) {
    const ref = useRef(null), ctl = useRef({ mode, orbit, axes, cam: null });
    ctl.current.orbit = orbit; ctl.current.axes = axes;
    useEffect(() => { if (ctl.current.cam) { ctl.current.cam.mode = mode; if (mode === 'earth') ctl.current.cam.dAz = 0; } }, [mode]);
    useEffect(() => {
      const stop = build(ref.current, ctl.current, i => onInfo(i), m => onFail && onFail(m));
      if (ctl.current.cam) ctl.current.cam.mode = mode;
      return stop;
    }, []);
    return h`<canvas ref=${ref} class="globe" aria-label="Three-dimensional view of the Earth with the satellite"></canvas>`;
  }
  window.Globe3D = Globe3D;
})();
