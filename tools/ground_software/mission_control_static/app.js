// S2S2 Mission Control -- frontend logic.
// Talks to mission_control_api.py's REST + WebSocket API only. No demod/
// decode happens here -- the GS board already did that; this renders
// what the API already parsed/persisted (gs_protocol.py).

const API = "";
let CURRENT_USER = null;

// =====================================================================
// Known enums (mirrors CPU2/core/Inc/s2s2_commands.h)
// =====================================================================
const CMD_NAMES = {
  0x00: "CMD_NONE", 0x01: "CMD_REQUEST_BURST", 0x02: "CMD_HEARTBEAT",
  0x03: "CMD_ABORT_MISSION", 0x10: "CMD_RUN_CAMERA", 0x11: "CMD_DOWNLOAD_FILE",
  0x12: "CMD_GET_HEALTH", 0x13: "CMD_GET_HOUSEKEEPING", 0x14: "CMD_GET_BEACON1",
  0x15: "CMD_GET_BEACON2", 0x16: "CMD_GET_ADCS", 0x17: "CMD_DIGIPEATER",
  0x18: "CMD_GET_SAT_LOG", 0x19: "CMD_REQUEST_RETRANSMIT",
};
const SUBSYS_NAMES = { 0x00: "(none)", 0x01: "OBC", 0x02: "COM", 0x03: "ADCS", 0x04: "CAM", 0x05: "EPDM" };
const DATA_ID_NAMES = {
  0x00: "(none)", 0x01: "HOUSEKEEPING", 0x02: "BEACON1", 0x03: "BEACON2", 0x04: "ADCS",
  0x05: "CAMERA_RGB", 0x06: "CAMERA_NIR", 0x07: "NDWI", 0x08: "EPDM",
  0x09: "DIGIPEATER", 0x0A: "SAT_LOG", 0xFF: "GENERAL",
};
const CLI_KEYWORDS = [
  "COMMAND", "HEARTBEAT", "HEALTH", "HOUSEKEEPING", "BEACON1", "BEACON2",
  "ADCS", "CAMERA", "DOWNLOAD", "SATLOG", "DIGIPEATER", "RETRANSMIT", "ABORT",
];
let CRITICAL_OPCODES = new Set();

const BEACON1_PLACEHOLDER = new Set();
const BEACON2_PLACEHOLDER = new Set();

// =====================================================================
// fetch wrapper -- redirects to login on 401 instead of every caller
// having to check for it
// =====================================================================
async function api(path, opts = {}) {
  opts.headers = Object.assign({ "Content-Type": "application/json" }, opts.headers || {});
  const res = await fetch(`${API}${path}`, opts);
  if (res.status === 401) {
    showLogin();
    throw new Error("not authenticated");
  }
  return res;
}

async function apiJson(path, opts = {}) {
  const res = await api(path, opts);
  const body = await res.json();
  if (!res.ok) throw new Error(body.detail || res.statusText);
  return body;
}

// =====================================================================
// Auth / login
// =====================================================================

function showLogin() {
  const el = document.getElementById("login-overlay");
  el.hidden = false;
  el.style.display = "flex";
}
function hideLogin() {
  const el = document.getElementById("login-overlay");
  el.hidden = true;
  el.style.display = "none";
}

async function checkAuth() {
  try {
    const res = await fetch(`${API}/api/auth/me`);
    if (res.status === 401) { showLogin(); return false; }
    CURRENT_USER = await res.json();
    hideLogin();
    document.getElementById("whoami").textContent = `${CURRENT_USER.username} (${CURRENT_USER.role})`;
    document.getElementById("admin-tab-btn").hidden = CURRENT_USER.role !== "admin";
    return true;
  } catch (e) {
    showLogin();
    return false;
  }
}

async function doLogin() {
  const username = document.getElementById("login-username").value.trim();
  const password = document.getElementById("login-password").value;
  const errEl = document.getElementById("login-error");
  errEl.textContent = "";
  try {
    const res = await fetch(`${API}/api/auth/login`, {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ username, password }),
    });
    if (!res.ok) {
      const body = await res.json();
      errEl.textContent = body.detail || "login failed";
      return;
    }
    await afterLogin();
  } catch (e) {
    errEl.textContent = String(e);
  }
}

async function doLogout() {
  await fetch(`${API}/api/auth/logout`, { method: "POST" });
  CURRENT_USER = null;
  showLogin();
}

// =====================================================================
// Three.js cubesat model -- hand-rolled drag-to-orbit / scroll-to-zoom.
// Orientation stays neutral (see index.html's attitude-note) -- real
// interactive 3D view, just not driven by a fabricated attitude solution.
// =====================================================================
let scene, camera, renderer, satGroup;
let orbitYaw = 0.6, orbitPitch = 0.35, orbitDist = 6;
let dragging = false, lastX = 0, lastY = 0;

function initThree() {
  const container = document.getElementById("three-container");
  scene = new THREE.Scene();
  camera = new THREE.PerspectiveCamera(45, container.clientWidth / container.clientHeight, 0.1, 100);
  renderer = new THREE.WebGLRenderer({ antialias: true });
  renderer.setSize(container.clientWidth, container.clientHeight);
  container.appendChild(renderer.domElement);

  scene.add(new THREE.AmbientLight(0x445544, 1.2));
  const dir = new THREE.DirectionalLight(0x99ffcc, 1.4);
  dir.position.set(3, 4, 5);
  scene.add(dir);

  satGroup = new THREE.Group();

  const bodyMat = new THREE.MeshStandardMaterial({ color: 0x1c4a37, metalness: 0.3, roughness: 0.6 });
  const body = new THREE.Mesh(new THREE.BoxGeometry(1, 1, 1.4), bodyMat);
  satGroup.add(body);

  const panelMat = new THREE.MeshStandardMaterial({ color: 0x0b2f6e, metalness: 0.1, roughness: 0.4 });
  const panelGeo = new THREE.BoxGeometry(2.2, 0.6, 0.04);
  const panelL = new THREE.Mesh(panelGeo, panelMat);
  panelL.position.set(-1.6, 0, 0);
  satGroup.add(panelL);
  const panelR = new THREE.Mesh(panelGeo, panelMat);
  panelR.position.set(1.6, 0, 0);
  satGroup.add(panelR);

  const antMat = new THREE.MeshStandardMaterial({ color: 0x888888, metalness: 0.8, roughness: 0.3 });
  const antenna = new THREE.Mesh(new THREE.CylinderGeometry(0.02, 0.02, 1.2, 8), antMat);
  antenna.rotation.x = Math.PI / 2;
  antenna.position.set(0, 0.5, -0.8);
  satGroup.add(antenna);

  const wireGeo = new THREE.EdgesGeometry(new THREE.BoxGeometry(1, 1, 1.4));
  const wire = new THREE.LineSegments(wireGeo, new THREE.LineBasicMaterial({ color: 0x39ff9c }));
  satGroup.add(wire);

  scene.add(satGroup);
  updateCamera();

  const dom = renderer.domElement;
  dom.addEventListener("mousedown", (e) => { dragging = true; lastX = e.clientX; lastY = e.clientY; });
  window.addEventListener("mouseup", () => { dragging = false; });
  window.addEventListener("mousemove", (e) => {
    if (!dragging) return;
    orbitYaw += (e.clientX - lastX) * 0.01;
    orbitPitch = Math.max(-1.4, Math.min(1.4, orbitPitch + (e.clientY - lastY) * 0.01));
    lastX = e.clientX; lastY = e.clientY;
    updateCamera();
  });
  dom.addEventListener("wheel", (e) => {
    orbitDist = Math.max(3, Math.min(15, orbitDist + e.deltaY * 0.01));
    updateCamera();
    e.preventDefault();
  }, { passive: false });

  animate();
}

function updateCamera() {
  camera.position.x = orbitDist * Math.cos(orbitPitch) * Math.sin(orbitYaw);
  camera.position.y = orbitDist * Math.sin(orbitPitch);
  camera.position.z = orbitDist * Math.cos(orbitPitch) * Math.cos(orbitYaw);
  camera.lookAt(0, 0, 0);
}

function animate() {
  requestAnimationFrame(animate);
  renderer.render(scene, camera);
}

// =====================================================================
// Chart / field rendering helpers
// =====================================================================

function renderFieldGrid(containerId, values, placeholderSet) {
  const el = document.getElementById(containerId);
  el.innerHTML = "";
  if (!values) {
    el.innerHTML = '<div style="color:var(--fg-dim)">no telemetry received yet</div>';
    return;
  }
  for (const [name, val] of Object.entries(values)) {
    const isPlaceholder = placeholderSet.has(name);
    const nameEl = document.createElement("div");
    nameEl.className = "fname";
    nameEl.textContent = name;
    const valEl = document.createElement("div");
    valEl.className = "fval";
    valEl.textContent = val;
    const badgeEl = document.createElement("div");
    badgeEl.innerHTML = isPlaceholder ? '<span class="badge na">N/A</span>' : '<span class="badge real">real</span>';
    el.appendChild(nameEl); el.appendChild(valEl); el.appendChild(badgeEl);
  }
}

function drawLineChart(svgId, seriesList) {
  // seriesList: [{data: [numbers], color: "#..."}]
  const svg = document.getElementById(svgId);
  if (!svg) return;
  svg.innerHTML = "";
  const allVals = seriesList.flatMap(s => s.data);
  if (!allVals.length) return;
  const w = svg.clientWidth || 300, h = svg.clientHeight || 90;
  const min = Math.min(...allVals), max = Math.max(...allVals);
  const range = (max - min) || 1;
  for (const s of seriesList) {
    if (!s.data.length) continue;
    const pts = s.data.map((v, i) => {
      const x = (i / Math.max(1, s.data.length - 1)) * (w - 4) + 2;
      const y = h - 2 - ((v - min) / range) * (h - 4);
      return `${x},${y}`;
    }).join(" ");
    const poly = document.createElementNS("http://www.w3.org/2000/svg", "polyline");
    poly.setAttribute("points", pts);
    poly.setAttribute("fill", "none");
    poly.setAttribute("stroke", s.color);
    poly.setAttribute("stroke-width", "1.5");
    svg.appendChild(poly);
  }
}

function renderAckBars(rows) {
  const el = document.getElementById("ack-bars");
  el.innerHTML = "";
  if (!rows.length) { el.innerHTML = '<div style="color:var(--fg-dim)">no ACKs yet</div>'; return; }
  const max = Math.max(...rows.map(r => r.count));
  for (const r of rows) {
    const row = document.createElement("div");
    row.className = "ack-row";
    row.innerHTML = `
      <div class="ack-label">${r.status_name || "0x" + (r.status ?? 0).toString(16)}</div>
      <div class="ack-bar-track"><div class="ack-bar-fill" style="width:${(r.count / max) * 100}%"></div></div>
      <div class="ack-count">${r.count}</div>`;
    el.appendChild(row);
  }
}

function logEvent(ev, targetId = "event-log") {
  const log = document.getElementById(targetId);
  if (!log) return;
  const div = document.createElement("div");
  div.className = `line ${ev.kind}`;
  const t = new Date(ev.ts * 1000).toLocaleTimeString();
  div.textContent = `[${t}] ${ev.kind}: ${ev.raw_line || JSON.stringify(ev)}`;
  log.appendChild(div);
  log.scrollTop = log.scrollHeight;
  while (log.children.length > 500) log.removeChild(log.firstChild);
}

// =====================================================================
// Overview tab data
// =====================================================================

async function refreshPorts() {
  const ports = await apiJson("/api/serial/ports");
  const sel = document.getElementById("port-select");
  sel.innerHTML = "";
  for (const p of ports) {
    const opt = document.createElement("option");
    opt.value = p.device; opt.textContent = `${p.device} -- ${p.description}`;
    sel.appendChild(opt);
  }
}

async function refreshStatus() {
  const s = await apiJson("/api/status");
  const dot = document.getElementById("status-dot");
  dot.classList.toggle("connected", s.connected);
  document.getElementById("status-text").textContent = s.connected ? `connected (${s.port} @ ${s.baud})` : "disconnected";
  document.getElementById("connect-btn").textContent = s.connected ? "Disconnect" : "Connect";
  document.getElementById("stat-ok").textContent = s.session_stats.ok;
  document.getElementById("stat-bad").textContent = s.session_stats.bad;
  document.getElementById("stat-misaddr").textContent = s.session_stats.misaddressed;
  document.getElementById("last-contact").textContent = s.seconds_since_contact != null
    ? `last contact: ${s.seconds_since_contact.toFixed(0)}s ago` : "last contact: --";
}

async function refreshTelemetry() {
  const t = await apiJson("/api/telemetry/latest");
  for (const f of t.beacon1_placeholder_fields) BEACON1_PLACEHOLDER.add(f);
  for (const f of t.beacon2_placeholder_fields) BEACON2_PLACEHOLDER.add(f);
  renderFieldGrid("beacon1-fields", t.beacon1, BEACON1_PLACEHOLDER);
  renderFieldGrid("beacon2-fields", t.beacon2, BEACON2_PLACEHOLDER);
}

async function refreshMissions() {
  const rows = await apiJson("/api/missions");
  const tbody = document.querySelector("#mission-table tbody");
  tbody.innerHTML = "";
  for (const r of rows) {
    const tr = document.createElement("tr");
    const t = new Date(r.ts * 1000).toLocaleTimeString();
    tr.innerHTML = `<td>${t}</td><td>${CMD_NAMES[r.opcode] || "0x" + r.opcode.toString(16)}</td>
                     <td>${SUBSYS_NAMES[r.subsystem] ?? r.subsystem}</td>
                     <td>${DATA_ID_NAMES[r.data_id] ?? r.data_id}</td><td>${r.result}</td>
                     <td>${r.sent_by || ""}</td>`;
    tbody.appendChild(tr);
  }
}

// =====================================================================
// Analytics tab data
// =====================================================================

async function refreshAnalytics() {
  const lq = await apiJson("/api/stats/link_quality");
  drawLineChart("chart-rssi", [{ data: lq.map(r => r.rssi), color: "#22d3ee" }]);
  drawLineChart("chart-snr", [{ data: lq.map(r => r.snr), color: "#ffb020" }]);
  drawLineChart("chart-rs", [{ data: lq.map(r => r.rs_corrected), color: "#39ff9c" }]);

  const stats = await apiJson("/api/analytics/session_stats_history");
  drawLineChart("chart-stats", [
    { data: stats.map(r => r.ok_count), color: "#39ff9c" },
    { data: stats.map(r => r.bad_count), color: "#ff4d5e" },
    { data: stats.map(r => r.misaddressed_count), color: "#ffb020" },
  ]);

  const ack = await apiJson("/api/analytics/ack_status_distribution");
  renderAckBars(ack);

  const cmdHour = await apiJson("/api/analytics/commands_per_hour");
  drawLineChart("chart-cmdhour", [{ data: cmdHour.map(r => r.count), color: "#22d3ee" }]);

  const uptime = await apiJson("/api/analytics/uptime");
  const el = document.getElementById("uptime-readout");
  if (uptime.length) {
    const last = uptime[uptime.length - 1];
    el.textContent = `day ${last.tim_day}, hour ${last.tim_hour}`;
  } else {
    el.textContent = "no beacon1 telemetry yet";
  }
}

// =====================================================================
// Satellite tab data
// =====================================================================

async function refreshSatPorts() {
  const ports = await apiJson("/api/serial/ports");
  const sel = document.getElementById("sat-port-select");
  sel.innerHTML = "";
  for (const p of ports) {
    const opt = document.createElement("option");
    opt.value = p.device; opt.textContent = `${p.device} -- ${p.description}`;
    sel.appendChild(opt);
  }
}

async function refreshSatStatus() {
  const s = await apiJson("/api/satellite/status");
  document.getElementById("sat-connect-btn").textContent = s.connected ? "Disconnect" : "Connect";
  document.getElementById("digi-relayed").textContent = s.digi_relayed;
  document.getElementById("digi-duplicate").textContent = s.digi_duplicate;
  for (const box of document.querySelectorAll(".phase-box")) {
    box.classList.toggle("active", box.dataset.phase === s.cycle_phase);
  }
}

// =====================================================================
// Admin tab data
// =====================================================================

async function refreshUsers() {
  const rows = await apiJson("/api/users");
  const tbody = document.querySelector("#user-table tbody");
  tbody.innerHTML = "";
  for (const u of rows) {
    const opcodes = JSON.parse(u.allowed_opcodes || "[]").map(o => "0x" + o.toString(16)).join(", ");
    const tr = document.createElement("tr");
    tr.innerHTML = `<td>${u.username}</td><td>${u.role}</td><td>${u.role === "admin" ? "(all)" : opcodes}</td>
                     <td><button data-uid="${u.id}" class="del-user-btn">Delete</button></td>`;
    tbody.appendChild(tr);
  }
  for (const btn of document.querySelectorAll(".del-user-btn")) {
    btn.onclick = async () => {
      try { await apiJson(`/api/users/${btn.dataset.uid}`, { method: "DELETE" }); refreshUsers(); }
      catch (e) { alert(e.message); }
    };
  }
}

async function refreshCriticalSecretStatus() {
  const s = await apiJson("/api/settings/critical_secret_configured");
  CRITICAL_OPCODES = new Set(s.critical_opcodes);
  document.getElementById("critical-secret-status").textContent = s.configured
    ? "a critical-command secret is configured" : "NOT configured yet -- critical commands will be refused";
  updateCriticalSecretVisibility();
}

function updateCriticalSecretVisibility() {
  const opcode = parseInt(document.getElementById("cmd-opcode").value, 10);
  document.getElementById("critical-secret-row").hidden = !CRITICAL_OPCODES.has(opcode);
}

// =====================================================================
// Tabs
// =====================================================================

function switchTab(name) {
  for (const btn of document.querySelectorAll(".tab-btn")) btn.classList.toggle("active", btn.dataset.tab === name);
  for (const panel of document.querySelectorAll(".tab-panel")) panel.hidden = panel.id !== `tab-${name}`;
  if (name === "analytics") refreshAnalytics();
  if (name === "satellite") { refreshSatPorts(); refreshSatStatus(); }
  if (name === "admin") { refreshUsers(); refreshCriticalSecretStatus(); }
}

// =====================================================================
// wiring
// =====================================================================

function populateSelect(id, map) {
  const sel = document.getElementById(id);
  sel.innerHTML = "";
  for (const [code, name] of Object.entries(map)) {
    const opt = document.createElement("option");
    opt.value = code;
    opt.textContent = `0x${Number(code).toString(16).padStart(2, "0")} ${name}`;
    sel.appendChild(opt);
  }
}

let ws = null;
function connectWebSocket() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onmessage = (msg) => {
    const ev = JSON.parse(msg.data);
    if (ev.kind === "CYCLE_PHASE" || ev.kind.startsWith("DIGI_") || ev.kind === "SAT_RAW") {
      logEvent(ev, "digi-log");
      refreshSatStatus();
    } else {
      logEvent(ev, "event-log");
      if (ev.kind === "TELEMETRY") refreshTelemetry();
      if (ev.kind === "STATS" || ev.rssi !== undefined) refreshStatus();
    }
  };
  ws.onclose = () => setTimeout(connectWebSocket, 2000);
}

async function afterLogin() {
  hideLogin();
  await checkAuth();
  refreshPorts(); refreshStatus(); refreshTelemetry(); refreshMissions();
  refreshCriticalSecretStatus();
  connectWebSocket();
}

document.addEventListener("DOMContentLoaded", async () => {
  initThree();
  populateSelect("cmd-opcode", CMD_NAMES);
  populateSelect("cmd-subsys", SUBSYS_NAMES);
  populateSelect("cmd-data-id", DATA_ID_NAMES);

  const opcodeCheckboxes = document.getElementById("new-user-opcodes");
  for (const [code, name] of Object.entries(CMD_NAMES)) {
    const label = document.createElement("label");
    label.style.fontSize = "10px";
    label.innerHTML = `<input type="checkbox" value="${code}"> ${name}`;
    opcodeCheckboxes.appendChild(label);
  }

  const cliGrid = document.getElementById("cli-grid");
  for (const kw of CLI_KEYWORDS) {
    const btn = document.createElement("button");
    btn.textContent = kw;
    btn.onclick = async () => {
      let secret;
      if (CRITICAL_OPCODES.has(CLI_OPCODES_JS[kw])) secret = prompt("critical command secret:");
      try {
        await apiJson("/api/cli", { method: "POST", body: JSON.stringify({ keyword: kw, secret }) });
        refreshMissions();
      } catch (e) { alert(e.message); }
    };
    cliGrid.appendChild(btn);
  }

  document.getElementById("login-btn").onclick = doLogin;
  document.getElementById("login-password").addEventListener("keydown", (e) => { if (e.key === "Enter") doLogin(); });
  document.getElementById("logout-btn").onclick = doLogout;
  document.getElementById("login-close-btn").onclick = hideLogin;

  for (const btn of document.querySelectorAll(".tab-btn")) {
    btn.onclick = () => switchTab(btn.dataset.tab);
  }

  document.getElementById("refresh-ports-btn").onclick = refreshPorts;
  document.getElementById("connect-btn").onclick = async () => {
    const status = await apiJson("/api/status");
    if (status.connected) {
      await apiJson("/api/serial/disconnect", { method: "POST" });
    } else {
      const port = document.getElementById("port-select").value;
      const baud = parseInt(document.getElementById("baud-select").value, 10);
      if (!port) { alert("no port selected -- click Refresh"); return; }
      try { await apiJson("/api/serial/connect", { method: "POST", body: JSON.stringify({ port, baud }) }); }
      catch (e) { alert(e.message); }
    }
    refreshStatus();
  };

  document.getElementById("cmd-opcode").addEventListener("change", updateCriticalSecretVisibility);

  document.getElementById("send-cmd-btn").onclick = async () => {
    const body = {
      opcode: parseInt(document.getElementById("cmd-opcode").value, 10),
      subsystem: parseInt(document.getElementById("cmd-subsys").value, 10),
      data_id: parseInt(document.getElementById("cmd-data-id").value, 10),
      address: parseInt(document.getElementById("cmd-address").value, 16) || 0,
      num_packets: parseInt(document.getElementById("cmd-num-packets").value, 10) || 0,
      secret: document.getElementById("cmd-secret").value || undefined,
    };
    try { await apiJson("/api/command", { method: "POST", body: JSON.stringify(body) }); }
    catch (e) { alert(e.message); }
    refreshMissions();
  };

  document.getElementById("dpm-test-btn").onclick = async () => {
    const secret = document.getElementById("dpm-secret").value || undefined;
    try { await apiJson("/api/dpm/test", { method: "POST", body: JSON.stringify({ secret }) }); }
    catch (e) { alert(e.message); }
    refreshMissions();
  };

  document.getElementById("sat-refresh-ports-btn").onclick = refreshSatPorts;
  document.getElementById("sat-connect-btn").onclick = async () => {
    const s = await apiJson("/api/satellite/status");
    if (s.connected) {
      try { await apiJson("/api/satellite/disconnect", { method: "POST" }); } catch (e) { alert(e.message); }
    } else {
      const port = document.getElementById("sat-port-select").value;
      const baud = parseInt(document.getElementById("sat-baud-select").value, 10);
      if (!port) { alert("no port selected -- click Refresh"); return; }
      try { await apiJson("/api/satellite/connect", { method: "POST", body: JSON.stringify({ port, baud }) }); }
      catch (e) { alert(e.message); }
    }
    refreshSatStatus();
  };

  document.getElementById("add-user-btn").onclick = async () => {
    const username = document.getElementById("new-user-username").value.trim();
    const password = document.getElementById("new-user-password").value;
    const role = document.getElementById("new-user-role").value;
    const allowed_opcodes = [...opcodeCheckboxes.querySelectorAll("input:checked")].map(el => parseInt(el.value, 10));
    try {
      await apiJson("/api/users", { method: "POST", body: JSON.stringify({ username, password, role, allowed_opcodes }) });
      refreshUsers();
    } catch (e) { alert(e.message); }
  };

  document.getElementById("set-critical-secret-btn").onclick = async () => {
    const secret = document.getElementById("critical-secret-input").value;
    try {
      await apiJson("/api/settings/critical_secret", { method: "POST", body: JSON.stringify({ secret }) });
      document.getElementById("critical-secret-input").value = "";
      refreshCriticalSecretStatus();
    } catch (e) { alert(e.message); }
  };

  const authed = await checkAuth();
  if (authed) await afterLogin();

  setInterval(() => { if (CURRENT_USER) refreshStatus(); }, 3000);
});

// keyword -> opcode, mirrors mission_control_api.py's CLI_OPCODES
const CLI_OPCODES_JS = {
  COMMAND: 0x01, HEARTBEAT: 0x02, HEALTH: 0x12, HOUSEKEEPING: 0x13,
  BEACON1: 0x14, BEACON2: 0x15, ADCS: 0x16, CAMERA: 0x10, DOWNLOAD: 0x11,
  SATLOG: 0x18, DIGIPEATER: 0x17, RETRANSMIT: 0x19, ABORT: 0x03,
};
