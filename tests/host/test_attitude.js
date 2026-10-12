// Numeric check of the attitude cube (tools/common/web/ui.js): the matrix must be a proper rotation, and the measured field direction,
// expressed in the cube's local coordinates, must be turned to "up" on the screen (CSS -y).
global.window = {};
global.React = { useState() {}, useEffect() {}, useRef() {}, useMemo() {}, createElement() {}, Component: class {} };
global.htm = { bind: () => () => null };
require('../../tools/common/web/ui.js');
const { attitude } = window.UI;
let bad = 0;
const check = (c, msg) => { if (!c) { console.log('FAIL', msg); bad++; } };
const cases = [[0, 0, 1], [1, 0, 0], [0, 1, 0], [0, 0, -1], [3, -4, 5], [-20, 7, -33], [0.1, 0.1, 40], [29, 1, -16]];
for (const b of cases) {
  const a = attitude(b);
  const m = a.matrix.slice(9, -1).split(',').map(Number);          // column-major 4x4
  const T = [[m[0], m[4], m[8]], [m[1], m[5], m[9]], [m[2], m[6], m[10]]];
  const det = T[0][0] * (T[1][1] * T[2][2] - T[1][2] * T[2][1]) - T[0][1] * (T[1][0] * T[2][2] - T[1][2] * T[2][0]) + T[0][2] * (T[1][0] * T[2][1] - T[1][1] * T[2][0]);
  check(Math.abs(det - 1) < 1e-9, `det ${det} for ${b}`);
  const n = Math.hypot(...b), u = b.map(v => v / n);
  const local = [u[0], -u[2], -u[1]];                               // P * u
  const v = T.map(r => r[0] * local[0] + r[1] * local[1] + r[2] * local[2]);
  check(Math.abs(v[0]) < 1e-9 && Math.abs(v[1] + 1) < 1e-9 && Math.abs(v[2]) < 1e-9, `field not up for ${b}: ${v}`);
  const ang = Math.acos(u[2]) * 180 / Math.PI;
  check(Math.abs(a.angles[2] - ang) < 1e-9, `angle to +Z for ${b}`);
}
console.log(bad ? `attitude: ${bad} FAILED` : `attitude OK (${cases.length} fields: proper rotation, field up, axis angles)`);
process.exit(bad ? 1 : 0);
