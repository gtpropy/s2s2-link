# Mission Control (operator software)

`make mission` starts it on http://127.0.0.1:8770 (login required; the first start prints the admin password once). It talks only to the ground-station board's UART.
Debug functions (raw UART consoles, SDR, SWD profiler, tests) live in the Testbench, not here.

The interface is written for operators who have not used it before: plain, professional wording; one task per page; a **Back to Home** link at the top of every page except the overview;
an information icon on every card; an interactive tour; text size (standard / large / extra large) and dark / light theme under Settings.

## Screens

**Home** (`#home`, the start page) is the overview of the whole ground station: radio board and link state, local storage (size of `GS_Data`, today's folder, last write), Google Drive (account space used / limit,
size and file count of the report folder, last upload, *Upload now*), the operational reports (last days on this computer with size and upload state; a day selector that also lists days that are only on Google Drive)
and the large **Go to satellite control** button. **Satellite control** (`#dashboard`) is the page described next; every page has **Back to Home**.

| Screen | Content |
|---|---|
| **Satellite control** | One screen, no page scroll, in the application's own theme. **Top:** Back to Home, **Fullscreen / Minimise** and one wrapping row of **LED flags** (LINK: GS, RESPONSIVE, CONTACT with RSSI, LOC, NEXT PASS, PASSES today; SAT: MODE, MISSION, UPTIME, HK age; **HB** and **AUTO** switches; hover for details). **Left:** *Next pass* (countdown to AOS or LOS, exact AOS and LOS times in local time, time since launch once the launch time is set in Settings > Satellite orbit) and *Commands* (every command, groups open and close, selecting one opens its page). **Middle:** *Position* map (the satellite is propagated about 30 times a second so it moves; flown and predicted track, footprint, station 5\u00b0 limit, night side, Sun; elevation / azimuth / range in the header) and *Telemetry* charts (battery V, bus V, battery I, solar V, battery T, RSSI; 1 h / 6 h / 24 h / all). **Pointing at a chart shows where the satellite was when that value was measured** (latitude / longitude, over land or sea, altitude, visible from the ground station or not) and marks it on the map; selecting pins it. The position comes from the current TLE, so it is less exact for times long before the TLE epoch; land / sea uses the coarse outline of the map. **Right:** *RF RX log* (what the ground station received from the satellite) and the *Ground station log*. Fullscreen starts from **Go to satellite control**, the sidebar link or the button; Esc leaves it. In a small window (below about 1150 px wide or 600 px high) the panels stack and the page scrolls. |
| **Commands** | Descriptions of every command. Selecting one opens its own page. |
| **Command pop-up** (`#cmd/<id>` for a direct link) | Selecting any command (dashboard list, Commands page, Telemetry, ...) opens it in a pop-up over the current page; it shows the same content as the command page: specification, transmit control, live decoded response. Esc or Close dismisses it. After **Capture image** completes, the pop-up offers **Download image**; starting a download (from there or from the list) closes the pop-up and opens the **Images** page, which shows the reception live: the picture decoded so far, one square per fragment, rate and remaining time. |
| **Command history** / entry (`#job/<id>`) | All commands with status, duration and operator; an entry shows its decoded response. |
| **Telemetry** | Tabs: **Housekeeping** (values, orientation, limits), **Trends** (a time frame of 3 h to 3 days from the database: battery V, bus V, battery I, solar V, battery T, RSSI next to quantities computed from the orbit: elevation, altitude, speed, latitude; eclipse shaded; the window with the lowest and the highest solar input voltage and how much of it was in eclipse; pointing at a chart shows where the satellite was), **Beacons** (decoded CW beacon 1 / 2, raw bytes), **ADCS**, **EPDM** (decoded text only, formats not final), **Digipeater**. |
| **Position and attitude** | 3D view (see below), position, attitude estimate, orbit data, pass table. |
| **Ground station log** | Every printed line, filters, search, pause, hexadecimal view, export. |
| **Images** | Pictures assembled from camera fragments. |
| **Operations guide** | Tour, status indicators, terms, troubleshooting. |
| **Settings** | Text size, theme, ground-station port, satellite orbit, station position, alert limits, **Google Drive**, **data storage**, accounts, audit log. |
| **Report viewer** (`#report/<day>`) | Tabs: Summary, Commands, Overpasses, Link (heartbeat), Signal at commands, ADCS, EPDM, CAM, DPM (statistics, stored responses, raw operation log), Files (every file of the folder viewed in place: spreadsheets as tables, logs as text, images, the report as text). Days that are only on Google Drive are downloaded first. |

### Command responses
ACK / NACK are decoded with the error and the corrective action; housekeeping packets as value tiles; plain-text answers (ping: `ACK: PING OK (OBC_ONLINE)`) as text; file downloads
(`53 01 1D Dd Ff`, 128-byte blocks) are assembled into one file card with a download button and an optional hexadecimal view. **ADCS and EPDM data (mission commands and the two files) is shown
only as the text decoded from the received bytes, without interpretation, because their formats are not final**; to add a decoder later, remove the names from `RAW_ONLY` in `static/parts.js`.
Starting the ADCS or EPDM mission is answered with ACK accepted and ACK completed only (as in the reference firmware).

### CW beacon
The tape is built from the decoder lines of the ground station (`CWCAR` = beacon starts, `CWSIG` = tone on / off, `CWCHAR` = one decoded letter); the finished text and the 8 beacon bytes follow (`CWRX`, `CWHK`).
The packet carries no beacon number (the COM alternates 1, 2, 1, 2); the beacon is classified from its contents (status bits and battery voltage plausible = beacon 1). BAT_I is a signed 16-bit value.

### 3D view and attitude
Position: SGP4 (satellite.js) from the stored TLE, in the browser; rendering with three.js (both vendored in `tools/common/web/vendor/`); Earth texture `static/earth_2048.jpg` (NASA Blue Marble, public domain).
**Attitude is an estimate, not a measurement of all three angles.** The only attitude sensor in the data is the magnetometer (health packet, microtesla). One vector observation fixes two of three
angles: the body starts in the nominal nadir-pointing frame and is turned by the smallest rotation that aligns the measured field with a centred-dipole model of the geomagnetic field (IGRF-13
dipole, 29.4 uT equatorial); rotation about the field direction cannot be determined. Body axes are assumed to coincide with the magnetometer axes. Without magnetometer data the model is drawn
in the nominal frame and the page says so. The satellite model is not to scale. If WebGL is not available the 2D map is shown instead.

### Satellite orbit
Settings -> Satellite orbit -> NORAD number -> *Fetch from CelesTrak* (`https://celestrak.org/NORAD/elements/gp.php?CATNR=<n>&FORMAT=tle`, refreshed every 6 h), or enter two TLE lines (length and
checksum are checked). The TLE is cached, so the view works offline; data older than 14 days is flagged. **No NORAD number is stored yet** (the satellite has to be in the public catalogue). The station
position defaults to Kathmandu until set. Passes are counted above 5 degrees elevation.

## Link supervision, automatic sampling, overpass notification
* **Heartbeat** (`mc/link.py`): every 30 to 40 s the ground station sends `PING`. An answer (ACK accepted, then the text `ACK: PING OK (OBC_ONLINE)`) means the link is up, the satellite is in command mode (listening) and responsive.
  **Last contact and RSSI on every screen come from these answers.** It is **off after a fresh install** because it transmits by itself (the open 5 V PA question of the flight board); switch it on with *Heartbeat on* on the Satellite control page.
  It pauses while an operator command is being served (half duplex; one radio sequence at a time).
* **LOC maintained** (line of contact) = the satellite is above the ground-station horizon according to the TLE (shown with the elevation).
* **Automatic sampling**: during an overpass (server-side pass prediction, `mc/passes.py`, which runs the browser's orbit code in `node`) and while the satellite responds, a health report and one block of the ADCS file are requested every 30 s
  as normal commands (operator "autosample", recorded like any command). Without orbit data it samples while the satellite responds and says so.
* **Overpass notification**: when the next pass starts in 30 minutes or less, a full-screen alert with a loud tone and a spoken message repeats every minute until **Yes, I am ready** is pressed (once per pass). Browsers allow sound only after one click or key press
  in the page; the alert says so if sound is still locked. A green bar shows while a pass is in progress. It needs the page to be open.

## Data store (`GS_Data/`, git-ignored)
```
GS_Data/<YYYY-MM-DD, HH_MMAM>/       one folder per day directly in GS_Data, created automatically
    S2S_OPERATIONAL_REPORT_<date>_<time>.docx            blank template for the operator to fill in (never overwritten)
    S2S_OPERATIONAL_REPORT_<date>_<time>_AUTOFILL.docx   the same report filled with the day's data (regenerated)
    GS_report.xlsx        Summary, Commands, Overpasses, Link (heartbeat), Signal at commands
    Storage/DAY_SUMMARY/   Day_Summary.xlsx / .txt, environment.json, README.txt: START HERE (see below)
    Storage/ADDRESS_DETAILS/Address_Data.xlsx   BEACON_DETAIL/Beacon_details.xlsx   LOG_DETAILS/<YYYYMMDD>/<ts>_LOG_raw.txt   HK_DETAILS/   CAM_DETAILS/(CAM_LOGS/)   MSN_LOG/   EPDM_DETAILS/   ADCS_DETAILS/   DIGIPEATER_DETAILS/
    Storage/Mission_Details/{ADCS,EPDM,CAM,DPM}/   operation_raw.log, <mission>.xlsx (Commands + Statistics), data/command_response_<ts>.txt (hex + decoded), CAM: data/image_stored_<ts>.jpeg
```
Same day-folder layout as the operations team's reports (`Long Duration Test/S2S_Operational_Reports/<month>/<day>/`, without the month level) plus `Storage/Mission_Details` (moved under Storage 2026-11-02; an old top-level folder is moved automatically), `Storage/DAY_SUMMARY` and `GS_report.xlsx`. File time stamps are UTC, folder names local time. Everything except the raw log is regenerated from the databases about every
15 s when something changed and after every command, so a restart loses nothing. Per command the response file lists sent bytes, ACK / NACK bytes, file blocks and packets in hex, the decoded text, the result, the attempts and the RSSI at send and at response.
**Retention**: folders older than 7 days (setting) are deleted from this computer, but only after they are on Google Drive (setting "delete only after upload", on by default); without Drive configured nothing is deleted.

## Google Drive backup (`mc/gdrive.py`)
Day folders go into a Google Drive folder named **S2S_operational_reports** (found or created automatically in My Drive; a folder id in the settings can override it, which a Shared Drive needs): `S2S_operational_reports/<YYYY-MM-DD, HH_MMAM>/...`. Settings -> Google Drive. Two ways: (1) **OAuth** with your own Google account: create a *Desktop* OAuth client in the Google Cloud console (Drive API enabled), enter client id and secret
(optional), press *Authorise with Google* and allow access; (2) a **service-account key** (JSON) with a **Shared Drive** folder (a service account has no storage of its own in My Drive). Only new or changed files are uploaded, every 5 minutes and on
*Upload now*; past days are marked uploaded after a clean sync. The home page shows the account's used / total space and the size of the report folder live (cached 2 minutes). Opening a day that is only on Drive downloads it to `GS_Data/_pulled/`.

## Mock mode
Off by default and not mentioned on the screens. Toggled by a key sequence (not inside a text field): **Up Up Down Down Left Right Left Right B A**.

While active: a striped banner and a "MOCK" watermark on every page, tab title `[MOCK]`; real data is put aside and restored unchanged afterwards; generated beacons (real layouts: beacon 1 / 2 alternating, CW text `S2S2 9N2SJ` + four hex groups), housekeeping
in realistic ranges (one Li-ion cell), RSSI and command responses (ACK, housekeeping packet, ping text, file replies with clearly labelled placeholder text, a NACK 05 case) are produced in the browser (`static/mock.js`);
commands, settings and accounts are answered in the browser: **nothing is sent to the ground station and nothing is written to the database, the raw log or the audit trail**. The orbit uses the stored TLE or a
built-in sample orbit labelled as such.

## Tour
Operations guide (or the *Tour* button; offered once at first login): 15 steps that highlight the real controls and wait for real clicks (open a command, transmit, Back to Home). While it runs the Transmit button sends nothing.

## API additions
`GET /api/home /api/link /api/passes /api/drive/stats /api/reports /api/report /api/report/file|sheet|docx /api/gdrive`, `POST /api/link /api/store/flush|upload|cfg /api/reports/pull /api/gdrive`, `GET /api/rawlog?n&q&kind`, `GET /api/orbit`, `POST /api/orbit` (admin: `{norad}` / `{refresh:true}` / `{name,line1,line2}` / `{station:{name,lat,lon,alt}}`), SSE event `raw`, job `detail.raw`, catalogue entries carry
`simple`, `icon` (code badge), `opcode`, `reply`. Table `raw` in `mission.db` keeps the newest 30,000 lines.

## Tests
`tests/host/test_gs_store.py` (real executor against a fake ground station: day folder tree, all files and their content, heartbeat, passes, retention, Drive upload / stats / pull against a local fake Drive server), `tests/host/test_mission_control.py` (TLE checks, orbit maths in node, beacon / ping parsers), `tests/host/ui_smoke.py <url> dashboard commands history telemetry location logs pictures help settings cmd/health --login admin:<password>`
(software WebGL via SwiftShader). Not run: a live session with a real ground station, and the real Google Drive (no credentials; only the fake server). The generated .xlsx / .docx files are read back by this program, not opened in Excel / Word.


**CW beacon bar.** The live Morse bar sits in the top menu bar on every page ("beacon details" opens Telemetry > Beacons). In fullscreen only the sidebar is hidden, so the bar stays.

**Chart data.** Charts average dense data into buckets (with a min / max band) and break the line wherever there is a gap of more than about 10 minutes, so hours of silence are never joined by a straight line. Map geography: `static/geo.json`, built by `tools/mission_control/build_geo.py` from Natural Earth (public domain, naturalearthdata.com).

**Satellite control, charts and sidebar.** The sidebar is hidden on this page (menu button at the top left opens it over the page, also in fullscreen). Charts: round axis values and clock-time ticks, smooth monotone curves, min / avg / max and the 10-minute change under each value (when the tile is tall enough), limits, eclipse (grey) and pass (blue) shading, a pulsing live point, and a cursor that follows the pointer continuously with a tooltip; all charts share it. In mock mode a day of history is generated and a quiet live stream (every 3 s) makes the charts move.

**Day summary** (`mc/daysum.py`, `Storage/DAY_SUMMARY/`, report viewer tabs *At a glance, Anomalies, Contacts, Satellite health, Day: Link quality, Environment*). Rebuilt from the databases with every flush. *At a glance*: needs-attention count, passes heard of predicted, last signal, signal strength min / mean / max,
satellite restarts, commands sent / completed / rejected / no answer, beacons and housekeeping counts, battery range, pictures, orbit-data age, software commit, Drive status. *Anomalies* (ERROR / WARN / INFO, sorted): NACKs with code, timeouts, retries, limit breaches, satellite restarts (beacon hours-since-reset falls or
HK sequence restarts), HK checksum failures and sequence gaps, ground-station disconnects, program starts, incomplete / mismatching pictures, packets outside every predicted pass, passes without a signal, old / missing orbit data. *Contacts*: one row per predicted pass with first / last signal, **AOS offset** to the prediction (a large offset or signals
outside passes = old TLE, wrong PC clock or station position), packets, RSSI / SNR, beacons expected vs received, heartbeats, commands. *Environment*: software commit and changed files, TLE name / epoch / age, station, limits, disk, Drive, GS port and its last boot lines (frequencies, board, key fingerprint). Received-signal samples are kept in table `rx`
(30 days) because the event log holds them in memory only; connects / disconnects of the ground station and program starts are in the audit table.
