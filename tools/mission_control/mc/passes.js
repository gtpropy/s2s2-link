// Pass prediction for the server: the browser's orbit code (static/orbit.js + satellite.js) run in node, so server and browser always agree.
// stdin: {"line1","line2","lat","lon","alt","start" (unix s),"hours","minEl"}   stdout: {"passes":[{aos,los,maxEl,maxT,azIn,azOut}]}
const fs = require('fs'), vm = require('vm'), path = require('path');
const root = path.resolve(__dirname, '..', '..');                         // tools/
const input = JSON.parse(fs.readFileSync(0, 'utf8'));
const ctx = { console, Math, Date, window: {} }; ctx.globalThis = ctx; vm.createContext(ctx);
vm.runInContext(fs.readFileSync(path.join(root, 'common', 'web', 'vendor', 'satellite.min.js'), 'utf8'), ctx);
vm.runInContext('var satellite = (typeof satellite !== "undefined" ? satellite : globalThis.satellite);', ctx);
vm.runInContext(fs.readFileSync(path.join(root, 'mission_control', 'static', 'orbit.js'), 'utf8'), ctx);
const O = ctx.window.Orbit;
O.set({ line1: input.line1, line2: input.line2 }, { lat: input.lat, lon: input.lon, alt: input.alt || 0 });
const passes = O.ok() ? O.passes(new Date(input.start * 1000), input.hours || 48, input.minEl == null ? 5 : input.minEl) : [];
process.stdout.write(JSON.stringify({ passes }));
