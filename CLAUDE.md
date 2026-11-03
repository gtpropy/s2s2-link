# S2S2 CubeSat Firmware -- Project Notes

Dual-core STM32WL55JC1 comms firmware. CPU1 (Cortex-M4) runs NuttX (`nuttx/`,
`apps/` submodules). CPU2 (Cortex-M0+) runs the bare-metal radio stack.

## Layout

See `README.md` (layout, quick start). Short version: `cpu2/{common,satellite,ground_station}` = CPU2 firmware (**reference code**
from `github.com/PrmBRana/dummy-camera-live` commit `0b807890`, plus our additions; `common/` is what was byte-identical in both
images), `tools/{config,mission_control,testbench,common,ground_software,flash}`, `tests/{host,tag_asm}`, `docs/`. The repo root `Kconfig`,
`CMakeLists.txt`, `configs/`, `include/`, `scripts/`, `src/` are the live NuttX board directory for `nucleo-wl55jc`: do not move or
delete them. `include/board.h` is the NuttX board header; since 2026-10-11 `include/<group>/` also holds the imported / ST-generated headers of the CPU2
images (cmsis, stm32wlxx_hal, st_radio, st_utilities, st_system, st_bsp, satellite, ground_station) and all CPU2 build output goes to `build/<image>/`: `docs/LAYOUT.md`. Venv: `tools/.venv`. Old `JC2/`, `V1.4`, reference snapshots, `shareRAM/`, FEC tests were archived outside the repo
(`../s2s2-link_removed_2026-10-07.tar.gz`, `../s2s2-link_removed_2026-10-10.tar.gz`; full pre-cleanup copy
`../s2s2-link_before_cleanup_2026-10-10.tgz`).

## Build / flash / configuration

```
make               # TUI (menuconfig): configure everything, build, memory stats, pick ST-Links, flash
make configure     # generate generated/{s2s2_config.h, s2s2.mk, secrets.h} from .s2s2.config (no UI)
make defconfig | newkey | build | flash | flash-all | memstat | probes | clean | nuttx | mission | testbench | test
```
`.s2s2.config` (JSON, git-ignored) is the single source of truth: targets, AUTH, DEBUG, LOG, FRONTEND, RTC timer, project name, call signs
and SSIDs, downlink/uplink Hz, CW dit ms, beacon period, probes. `generated/s2s2_config.h` carries the mission values into both images
(`S2S2_*` macros), `generated/s2s2.mk` the build options, `generated/secrets.h` the random AUTH key (created when missing, replaced only by
`make newkey`; never commit it; both boards must be built from the same file, compare the `key xxxxxxxx` fingerprint in both boot messages).
`make flash sat CUBUS=T` (or TUI Build options -> CUBUS, `.s2s2.config` `"cubus": true`) builds / flashes the satellite for the real flight board RSMS CUBUS V.3 (docs/CUBUS.md: pin
map, what the profile changes, open points; check `g_boardId == 0xB0A2D003` before powering it). **Flash lock (2026-10-12):** `make flash` / `flash-m4` run `tools/flash/board_guard.py` first; a CUBUS board (CUBUS id in CPU2 flash or uid in `tools/flash/cubus_boards.txt`) refuses every image except `CUBUS=T` with `ERROR!! You made a mistake, dont flash nucleo in CUBUS` (docs/CUBUS.md). `sat` / `gs` after build / flash / clean pick one image; CUBUS=T|F overrides the saved option.
Per-image Makefiles also work directly: `cd cpu2/satellite && make [AUTH=TRUE|FALSE] [DEBUG=T|F] [FRONTEND=..] [SERIAL=<probe>]`,
`make flash | size | probes | c2boot | flash-m4 | clean`; outputs in `build/<image>/` at the repo root (`build/satellite/`, `build/ground_station/`; ST headers are in `include/<group>/`, see `docs/LAYOUT.md`). `DEBUG=T` (-Og -g3) will not fit the satellite's 56 KB flash.

Toolchain: use the official ARM GNU Toolchain (`/Applications/ArmGNUToolchain/*`); the
Makefiles pick it automatically. Homebrew's `arm-none-eabi-gcc` 16.x cannot find newlib
headers. `CPATH`/`C_INCLUDE_PATH` must be unset.

CPU1 (NuttX): `cd nuttx && ./tools/configure.sh nucleo-wl55jc:demo`, enable
`CONFIG_ARCH_BOARD_ENABLE_CPU2`, `CONFIG_BOARD_LATE_INITIALIZE`, `CONFIG_EXAMPLES_S2S2SIM`,
**disable `CONFIG_ARCH_BOARD_FLASH_MOUNT`** and the serial console on LPUART1, then `gmake -j4`.
Needs `brew install make flock` (system make 3.81 is too old for NuttX; `flock` is absent on macOS).

## Integrity check (AUTH)

FEC was removed (2026-10-10). With `AUTH=TRUE` every payload <= 107 bytes gets a 12-byte trailer behind the plain Packet-Format information
field: `[payload][counter(4, big endian)][tag(8)]`; the tag is a keyed 64-bit digest over payload + counter, the counter a replay counter.
Longer payloads (138-byte camera fragments, 128-byte telemetry) are untouched. 20 leading HDLC flags on every packet. Both ends must use the same
AUTH setting and key. Counters are RAM-only: after a reboot the sender restarts at 1 and the receiver rejects until `Seal_ResetRxFloor` (bench RESET).

- `cpu2/common/protocol/seal_tag.S`: hand-written Thumb-1 tag function (tested against the official vector and 664 random cases by
  `tests/tag_asm/run.sh`; needs `pip install unicorn` in a scratch venv). Key and data must be word aligned.
- `cpu2/common/protocol/seal_wrap.{c,h}`: wrap / unwrap, replay floor, boot self-test (`g_sealSelfTest` = `0xA5A50001` pass, readable over SWD).
- The satellite drops any command without a valid trailer when AUTH is on.
- `cpu2/common/system/mini_printf.c` replaces newlib's vsnprintf/snprintf (-1.2 KB); host tests in `tests/host/` (`run.sh`: round trip, every single
  flipped bit rejected, replay, plain packet rejected).
- Naming is deliberately neutral in source (`seal_*`); do not describe the algorithm in user-facing text -- call it the "integrity check".

## Digipeater (2026-10-10)

`cpu2/common/digipeater` (engine, pure logic) + `cpu2/satellite/app/satellite_digi.c` (glue) + `Protocol_CreateRawPacket`; settings in the TUI ("Satellite digipeater"),
`DIGI=FALSE` removes it (+5.1 KB, satellite is 90.5 % full with it). Rules, architecture, research
and evidence: `docs/DIGIPEATER.md`. `make test` runs the host tests and, when Dire Wolf is installed, the cross-checks. Boot self-test word `g_digiSelfTest` = `0xA5A50002`.
Store and forward (`saf.c`, journal on CPU1 flash via mailbox `0x20008000`, `apps/examples/s2s2obc/s2s2obc_saf.c`) keeps accepted frames and sends them again with a hop budget from `WIDEn-N`;
`tools/testbench/saf_tool.py status|list|inject|clear` (needs the bench server stopped: it owns the VCP). Satellite image is 95 % full. Half duplex: frames are not heard during the CW beacon.
Proven on air with SDR + Dire Wolf via VCP injection; the satellite *receiving* such a frame over the air is not tested (GS not flashed).

## Hardware findings (don't re-derive)

- CPU2 boot: option bytes must be `C2OPT=1` (flash) and `SBRV=0xC800` (0x08032000).
  `C2OPT=0` means *SRAM* -- CPU2 then never runs our code. `make c2boot SERIAL=..`.
  CPU1 (NuttX) must set `PWR_CR4.C2BOOT` (plus an `SEV`, patched in `stm32_pwr_boot_c2`).
- NuttX flash partitions overlap CPU2's flash and erase its tail: keep
  `CONFIG_ARCH_BOARD_FLASH_MOUNT` off.
- The reference satellite sends logs through CPU1; our CPU1 image doesn't print them, so
  the Nucleo's USB serial is silent. Read results from RAM over SWD (`openocd mdw`).
- Probes seen: Nucleo V3 `002D001B4741500920383733` (also `002E0035...` on another Nucleo),
  GS V2 `303030303030303030303031` (+ FTDI AG0L702O powers/serial for the GS).
  Boards from other users carry other firmware; back up before overwriting.
- The reference firmware uses 435.000 MHz downlink / 437.375 MHz uplink, RX BW 23.4 kHz,
  sync word `93 0B 51 DE`, RFO_LP + 5 V PA for GMSK, 3.3 V PA for CW.
- Satellite flash is nearly full (56,700 of 57,344 B). GS RAM is 91% (10 KB camera buffer).

## Not done / open

- **RF link is unproven.** Earlier (older firmware) tests: the satellite's receiver saw
  -79 dBm from the GS, but the GS never saw the satellite's transmit, and no frame was ever
  decoded. The reference firmware showed the same silence on this setup. Suspect the coax /
  RF path or the SDR; the RTL-SDR also stalls after a killed capture (replug it) and shows
  `PLL not locked!`.
- **E2E verified on air (2026-10-08, coax, no SDR, Nucleo `002E…` as satellite + GS):** wrapped PING
  uplink accepted (replay floor advances), satellite answers ACK Accepted, ACK Completed and
  "ACK: PING OK (OBC_ONLINE)" (CPU1 answered), GS decodes all three through the wrap
  ("checked block OK"), RSSI about -89 dBm.
- **`FRONTEND=NUCLEO|CUSTOM` (satellite Makefile, default NUCLEO):** the reference satellite code targets
  a custom board (RFO_LP PA + external 3.3 V/5 V PAs, PC3 = 5 V PA enable). On a stock Nucleo that
  drove the HP switch path with the LP PA selected -> no RF out (CW leaked a little, GMSK nothing).
  NUCLEO uses the GS's switch table (PC3 enable, RX C1=1 C2=0, HP TX C1=0 C2=1) and the RFO_HP PA
  (`SAT_PA_SEL`/`SAT_SET_PA_CONFIG` in `Mission/config.h`). Use `FRONTEND=CUSTOM` for the real board.
- Sat cycle: ~29-36 s CW TX (20 WPM, no RX), then ~50 s listen window; commands sent during CW are lost. The beacon now starts every 90 s (from
  the start of the previous one) even during an image transfer (docs/CHANGES.md section 11). GS `CW_UNIT_DEFAULT_MS` must equal the satellite unit (60 ms).
- Sat debug UART1 is PA9/PA10 (no USB bridge): read state over SWD (`app_state`,
  `s_sealRxFloor`, `rx_frame_buffer`, GPIOC ODR bits 3-5 = RF switch).
- Ground-side tools (`flasher.py`, `console.py` TESTDIGI 168-byte check) still reflect the
  old firmware.

## HAL -> LL (2026-10-08)

HAL linked into each image went from ~3.8 KB to ~0.28 KB (only `HAL_RCC_GetSysClockFreq`/`GetHCLK2Freq`
remain). Image text: satellite 53,400 -> 50,248 B, GS 50,988 -> 47,920 B (-5.9 % / -6.0 %). The rest of
the image is application code, ST radio middleware (`radio_driver/`, ~8.7 KB) and libc, not HAL.
- `subghz/subghz.c`: LL driver for the Sub-GHz SPI (one transaction routine for command / register /
  buffer access + the radio IRQ dispatcher). Same `HAL_SUBGHZ_*` names, so `radio_driver.c` is untouched.
  `hal/Src/stm32wlxx_hal_subghz.c` is no longer built.
- `core/ll_port.c` + `Inc/ll_port.h`: `LL_Init_CPU2()` (replaces `HAL_Init`), `LL_GPIO_OutputInit()`,
  `HAL_GPIO_WritePin/ReadPin`, `HAL_GetTick/IncTick`. HAL core/gpio/cortex/pwr modules are not built;
  NVIC calls are CMSIS `NVIC_*`.
- The RTC-backed `UTIL_TIMER` layer (`core/timer_if.c`, `core/rtc.c`, HAL RTC/RCC-ext, ~1.5 KB) was dormant:
  nothing calls `SystemApp_Init()`/`UTIL_TIMER_Init()`, the RTC clock was never enabled (RTC and RCC_BDCR
  read back 0 on the running board) and every alarm start was a no-op. It is now `TIMER_IF_USE_RTC=0`
  stubs with identical behaviour; `make TIMER_IF_USE_RTC=1` builds the old driver.
- Regression check on hardware: `tools/.venv/bin/python tools/testbench/e2e_ping.py` (GS PING = HEALTH SAT -> ACK, HK packet, ACK decoded). The satellite only listens ~50 s of ~95 s, so it retries PING until it lands.

## Test bench GUI + CPU1 image path (2026-10-08)

Full change log with measured sizes: `docs/CHANGES.md`; diffs vs the reference: `docs/diffs/`.

Two separate programs (do not mix them): `make mission` = Mission Control, production operator software, GS board UART only, login (port 8770,
first start prints the admin password once). `make testbench` = debug software (port 8765): GS FTDI (`/dev/cu.usbserial-*`) + Nucleo VCP (`/dev/cu.usbmodem*`)
consoles, tests, e2e, SDR, and the SWD profiler (`tools/testbench/tb/profiler.py`: needs the ST-Link of the board, attaches without reset; detach before flashing;
never read OpenOCD output through an unread pipe). No simulators: all data comes from the real boards. Code: `tools/testbench/tb/` and
`tools/mission_control/mc/` (launchers `server.py`), shared HTTP layer `tools/common/httpx.py`. Check after changes: `pyflakes`, `sh tests/host/run.sh`,
`tools/.venv/bin/python tests/host/ui_smoke.py <url> <pages…>` (headless Chrome, no boards). Details: `docs/CHANGES.md` sections 24 and 25.

Image path: PC --VCP--> **CPU1 (NuttX app `apps/examples/s2s2obc`)** stores the file on **LittleFS in CPU1's internal flash**
(pages 72..99 = `0x08024000..0x08031FFF`, mounted `/obc`) -> CPU1 reads chunks from the file into the shared flash ring
(`0x20008840`) -> CPU2 transmits them -> GS. CPU1 owns LPUART1 (the VCP); CPU2 does not touch it. Handshake block
`0x20008F00` = `cpu2/satellite/ipc/obc_shared.h`. LEDs: blue = writing flash, green = reading, yellow = CPU2 TX.

Frames on the VCP: PC->CPU1 `C5 x8, A7, cmd, seq(2), total(2), len, data, crc16` (CRC-16/CCITT-FALSE); CPU1->PC
`A5 5A cmd seq(2) status 0D 0A` (+ `A5 5C seq len data crc16 0D 0A` for READF/INFO). cmds: 02 STATUS (0 = CPU1 up and CPU2
heartbeat advancing), 03 RESET (clear CPU2 replay floor), 04 new file, 05 STORE, 06 SENDF, 07 READF, 08 SETNAME, 09 INFO.

NuttX (CPU1) build: `cd nuttx && gmake -j6` (needs `brew install make flock`); config: `FS_LITTLEFS=y` with all LittleFS
size factors = 1 (default 4x = 8 KB buffers -> `ENOMEM` mount failure), `SERIAL_TERMIOS=y`,
`INIT_ENTRYPOINT="s2s2obc_main"`. `nuttx.bin` must stay below `0x08024000` (page 72); flash with
`make flash-m4 SERIAL=<probe>` in `cpu2/satellite`.

Proof tools: `tools/testbench/verify_report.py <docs/reports/dir>` (rebuild from raw GS log), `tools/testbench/flash_proof.py <report dir>`
(raw SWD dump of the LittleFS pages; stop the bench first, it needs the ST-Link).

Learned the hard way:
- `SAT_TX_GAP_MS` (300 ms) and `SAT_ACK_GUARD_MS` (700 ms): the GS prints a full report per frame and loses back-to-back packets.
- OPEN BUG: `make LOG=0` corrupts the satellite's downlink frames (cause unknown). `LOG=1` is the default.
- The replay counters are RAM-only: a rebooted GS restarts at 1 and the satellite rejects everything until the `RESET` frame.
- A GS-commanded `CAM` makes CPU2 read the flash ring; with nothing feeding it the satellite sends zero chunks for minutes.

---

# HANDOFF / CONTEXT DUMP (written 2026-10-11) -- a new agent session starts here

This section records everything done on this project so a fresh session can continue without the chat history. The chronological firmware history with measured
numbers is `docs/CHANGES.md` (sections 1-25); the digipeater research and rules are `docs/DIGIPEATER.md`; layout and quick start are `README.md`. This file
is the map and the rulebook. **Nothing is committed** (the user commits); `git status` shows staged renames `tools/testbench -> tools/console` that no longer exist
(the tool is back at `tools/testbench`): `git add -A tools` before committing.

## 0. Resume checklist (do this first)
1. Read "User rules" (section 2). The user is strict about: no commits, no fake data / simulators, no touching boards unasked, Mission Control != Testbench.
2. `git status`, `ls data/` (runtime data, git-ignored), check which servers run: `ps aux | grep server.py`; ports 8765 (testbench) and 8770 (Mission Control).
3. `sh tests/host/run.sh` (all host tests), `pyflakes` (section 9.4), and for the web tools `tests/host/ui_smoke.py` (headless Chrome, no boards).
4. Hardware state at the end of the session: all boards DISCONNECTED by the user's request. Before any hardware action, ask. Probes: satellite ST-Link V3 `002D001B4741500920383733`,
   GS ST-Link V2 `303030303030303030303031` ("000000000001"), GS FTDI `/dev/cu.usbserial-AG0L702O`, satellite VCP `/dev/cu.usbmodem*`.
5. Pending decision for the user: reflash the ground-station board with the current firmware (its flashed firmware hung on the `DIGI` command and is older than the repo).

## 1. Project in one page
- **What**: COM subsystem of the CubeSat S2S2 (call sign 9N2SJ; the sample picture is the Antarikshya logo). STM32WL55JC1 (Nucleo-WL55JC), dual core. **CPU1** Cortex-M4 runs **NuttX** (`nuttx/`, `apps/` submodules)
  and owns the image store (LittleFS in internal flash), the VCP to the PC, the digipeater journal. **CPU2** Cortex-M0+ runs the bare-metal radio stack (`cpu2/`): GMSK 4800 bit/s
  G3RUH AX.25 on 435.000 MHz down / 437.375 MHz up, CW Morse beacon, command handling, digipeater. One image for the satellite (`cpu2/satellite`), one for the ground station
  (`cpu2/ground_station`); both run on Nucleo boards in the lab (GS = second Nucleo + FTDI UART). The real flight board has a custom RF front end (`FRONTEND=CUSTOM`).
- **Origin**: the CPU2 code is the reference firmware `github.com/PrmBRana/dummy-camera-live` commit `0b807890` ("hk correct") plus our additions (diffs listed in CHANGES.md; the old
  per-file diff files were dropped with the old layout). An older `JC2/` firmware (bidirectional digipeater) was archived on 2026-10-07 (`../s2s2-link_removed_2026-10-07.tar.gz`).
- **Spec**: the mission "Packet Format" sheets (`docs/spec/`): ACK/NACK sheet, command opcodes sheet, CW sheet, camera fragment (type 0x08), HK/telemetry packets.
- **People**: the user (git name dhakalrabi) drives this repo; teammates wrote the reference firmware repo and the legacy `tools/ground_software` (its files carry another author's header). Do not guess who owns what: ask.
- **Presentation**: `docs/SLIDES_BRIEF.md` (a deck brief). Hardware reports in `docs/hardware/`, test reports in `docs/reports/<time>/`, proof captures in `docs/proof/`.

## 2. User rules and working style (follow strictly)
- **No git commits / pushes unless asked.** Never destructive git. Attribution lines only when committing.
- **Real proof only. NO simulation, no dummy streams, no fake telemetry** ("SIM IS VERY BAD"). All data shown must come from the real boards; the Nucleo can itself generate dummy data. Pure unit tests of logic are fine.
  A GS simulator was built once, rejected, deleted. Honest reporting: say what was NOT run or failed.
- **Hardware**: do not open serial ports / ST-Link / SDR unasked. Last instruction: "no nucleo, no gs, no sdr, all disconnected". The servers open nothing at start.
- **Two separate programs** (never mix): Mission Control = PRODUCTION operator software (talks only to the GS board; shows telemetry, attitude from CW beacon 2 ADCS data, link, imagery,
  guarded commands; NO debug console, tests, SDR). Testbench = the ultimate DEBUG software (both UART consoles, SWD memory/CPU profiler like a task manager, tests, e2e, scenarios,
  screenshots, reports as HTML/PDF/ZIP, HK shown as a real dashboard).
- **Naming**: never describe the integrity-check algorithm in user-facing text: call it "integrity check" / AUTH (source names are neutral `seal_*`). FEC was removed 2026-10-10.
- **Style**: terse "caveman" replies (fragments, no filler); code, commits, security text written normally. Prefer doing over asking; ask only for decisions that are the user's.
- The user often interrupts with a correction: re-read it, change course, do not defend. Memory files: `~/.claude/projects/-Users-dhakalrabi-nuttxspace-s2s2-link/memory/`
  (project-mc-vs-testbench, feedback-no-simulation-and-hardware-access, hide-FEC naming, layout/TUI) -- keep them updated.

## 3. History (what was done, in order; details + numbers in docs/CHANGES.md)
- **Phase 0 (before 2026-10-07)**: reference firmware + `JC2` experiments; Mission-control-like Python tools by teammates (`tools/ground_software`, now legacy).
- **Phase A, 2026-10-08 -- optimisation + image path (CHANGES 1-8)**: streaming G3RUH bit writer (-5..6 KB RAM), `mini_printf`, LL SubGHz driver, HAL -> LL, RTC UTIL_TIMER removed, SipHash tag in Thumb-1 asm,
  (then) error-correcting wrap in asm (later removed). Nucleo RF front end fix (`FRONTEND=NUCLEO`: RFO_HP PA, PC3 enable, GS switch table). **Image path**: PC --VCP--> CPU1 s2s2obc app stores the
  file on LittleFS (pages 72..99) -> shared flash ring (SRAM2 0x20008840) -> CPU2 transmits as camera packets -> GS UART JSON. Test bench GUI, LED trace over SWD, independent proof tools. Proof
  `docs/reports/20261008_172200` (183/183 chunks, hashes identical, LED phases, raw flash dump 180/183 chunks).
- **2026-10-08/09 fixes (CHANGES 9-12)**: even-tick underflow caused 40-50 s pauses (fixed); CW beacon 15 -> 20 WPM, shorter tuning tone; beacon every 90 s even during transfers; CPU1 NEWFILE clears the store;
  bench fixes; throughput analysis; raw air-rate test (best 4276 bit/s air, 2737 payload).
- **2026-10-10 -- spec + restructure (CHANGES 13-16)**: Packet Format sheets applied (callsigns `9N2SJ-1` -> `GROUND`, camera type 0x08, CW `S2S2 9N2SJ`+8 bytes, 20 flags); FEC=T|F, AUTH independent; ACK/NACK
  (0xAC 00/01, 0xEE err mission) and 13-byte command opcodes per sheets; **FEC removed completely**; one source of config (`.s2s2.config` -> `generated/`), random AUTH key in `generated/secrets.h`;
  new folder layout (`cpu2/{common,satellite,ground_station}`, `tools/`, `tests/`, `docs/`); SDR + Direwolf proof (`docs/proof/*`, report `20261010_133730`: 183/183 fragments decoded by stock Dire Wolf).
- **Digipeater (CHANGES 17-21)**: pure-C engine (New-N rules, loop H-bit, dedupe 30 s, rate limits), integrated in the satellite; AFSK1200 modem built then REMOVED (GMSK only); store-and-forward with hop budget
  (12 messages, journal on CPU1 LittleFS `/obc/saf.jrn`, SRAM2 mailbox 0x20008000, VCP cmds 0x0C/0x0D); test option appending store stats; bench digipeater test 8/8 on hardware; Dire Wolf decoded repeats.
- **menuconfig + size work (CHANGES 22)**: NuttX-style TUI (`make`), images smaller (satellite 50,876 B = 88.7 %, GS 42,176 B): trace stub, nibble CRC table, 32-bit channel math. `-flto` rejected.
- **CW HELLO test without leading carrier (CHANGES 23)**: CwGet decoded the long carrier as T ("THELLO"); default HELLO only.
- **2026-10-10/11 -- web software (CHANGES 24-25)**: the user first said "make everything controllable by the mission control dashboard ... full software ... React ... ultimate web debug console ... END2END test ...
  image data stored and image built in the background". I wrongly merged everything into one program; the user corrected: two separate products (section 2). Built: `tools/common` shared code, Mission Control,
  Testbench (consoles, profiler, 20+ tests, scenarios, logs, trends, alerts, screenshots, reports), then a clean-up/optimisation pass (package split, shared HTTP layer, gzip/ETag/batched SSE).
- **Also this session**: GS simulator attempt (rejected, deleted); explained Dire Wolf strips 0x7E flags; located LittleFS code (`apps/examples/s2s2obc/s2s2obc_main.c`); wrote this dump.

## 4. Firmware facts (CPU2 = cpu2/, CPU1 = apps/examples/s2s2obc + nuttx/)
### 4.1 Radio and framing
- 4800 bit/s GMSK, G3RUH scrambler + NRZI, AX.25 UI frames, SX126x, sync word `93 0B 51 DE`, RX BW 23.4 kHz, fixed 200-byte radio packet, **20 leading HDLC flags** on every packet (the satellite RX has no sync
  word and loses ~18 bytes to preamble+sync, so 4 flags were once too few; 20 now). Satellite TX frequency 435.000 MHz (downlink), uplink 437.375 MHz. Nucleo uses RFO_HP (+22 dBm); custom board RFO_LP + external PAs.
- Callsigns: satellite `9N2SJ-1`, ground `GROUND`, camera fragments `9N2SJ-1 -> GROUND`, ctl 0x03, PID 0xF0. Digipeater alias `APNDIG` (APN = Antariksha Pratishan Nepal; 6 characters is the AX.25 limit; was ARISS until 2026-10-15). Dire Wolf decodes our frames (4800 G3RUH): flags/bit-stuffing/FCS are stripped
  before it shows a frame; `atest -h` / `direwolf -d p` print the frame hex. The GS firmware prints StartFlag/EndFlag itself.
- Timing cycle: CW beacon every 90 s (from the start of the previous one), 29-37 s long (20 WPM, unit 60 ms, 500 ms tuning carrier, `S2S2 9N2SJ` + 8 data bytes as hex), then ~50 s listen window; **commands sent
  during CW are lost**. GS `CW_UNIT_DEFAULT_MS` must equal the satellite unit (60 ms). Per chunk during image transfer ~0.79 s (0.36 s air + 0.30 s `SAT_TX_GAP_MS` + hand-over).
### 4.2 Packets (Packet Format sheets)
- **Commands**: 13 bytes `53 | MCU id | opcode B1 B2 B3 | DATA_ID(2) | ADDRESS(4) | NO_OF_PACKETS(2)`. Served: `53 04 CC 5E BD` run camera, `53 01 1D D2 F5` camera download, `53 01 1D D1 F2` HEALTH SAT (HK1/PING),
  `53 01 1D D1 F8` stored housekeeping download (`D n`), `53 01 DE EF AA` antenna deployment, `53 01 1D D1 F5` ping (answer text `ACK: PING OK (OBC_ONLINE)`), `53 04 CC 5E BE` dummy camera test, `53 03 A0 53 CF` run ADCS mission and `53 05 EC CF CF` run EPDM mission (MCU 03 / 05, ACK only; restored 2026-10-28 from the real repo github.com/PrmBRana/dummy-camera-live), and the whole file table of the sheet: `53 01 1D Dd Ff` with F1 flags / F3 sat_log / F4 rsv_table on D1|D3, F5 cam / F6 epdm / F7 adcs on D2|D4 (EPDM = earthquake precursor detection mission; docs/CHANGES.md section 32). Anything else -> NACK `EE 09 FF` (invalid command); 0x53 frame not 13 B -> `EE 04 FF`; data not there -> `EE 05 <mission>`.
- **ACK/NACK**: 3-byte info `[0xAC][00 accepted | 01 completed][mission]`; NACK `[0xEE][err][mission]`, err 02 packet, 03 CRC, 04 incomplete, 05 data missing, 06 unavailable, 07 busy, 08 mission inactive,
  09 invalid command, 0A memory/file, 0B transmission failed. Mission ids: 1 housekeeping, 2/3 beacon 1/2, 4 ADCS, 5 camera, 6 NIR, 7 NDWI, 8 EPDM, 9 digipeater, 10 satellite log, FF general.
- **Camera fragment (type 0x08)**: image id, fragment index (LE), total (LE), image type, AI score, 128 data bytes, CRC-16/X.25 = 138 B, unwrapped; last fragment zero padded; no length field.
  Picture rebuilds from fragments; JPEG ends at FFD9 / PNG at IEND (testbench imagestore `trim`).
- **HK packet (128 bytes)**: bytes 0..31 ADC1 = 16 x int16 LE (`[0]` BatV x100, `[1]` SolV, `[2]` BusV, `[3..7]` SP5,SP4,SP3,SP1,SP2 volts x100, `[8]` AntT, `[9]` BatT, `[10]` BPBT in 0.1 C); 32-33 footer `55 AA`;
  34..57 ADC2 12 x int16 (`[0]` Unreg I x100, `[1]` 3V3 I, `[2]` 5V I, `[3]` Bat I, `[4]` SP1 I, `[9]` Bus I, `[10]` Flag1, `[11]` Flag2); 58..69 IMU 6 x int16 (gyro centi-deg/s x3, mag uT x3); 70-71 footer `66 BB`;
  72..83 meta (seq, timestamp ms, total sent; u32 LE); 124-125 CRC-16; 126-127 end footer `AA CC`. The GS prints it as text AND as one JSON line `{"type":"HK",...,"hex":"..."}`.
- **CW beacon**: `S2S2 9N2SJ` then 8 bytes = CW BEACON 1 or 2 per the CW sheet (b1: system status/mission/hours since reset, solar total I/V, battery V/I; b2: gyro x3, magneto x3 (int8), solar cell bits, battery temp). The sheet does NOT
  number them: the tools decode every beacon both ways. GS prints `{"type":"CWHK","hex":"8 bytes"}` and `{"type":"CWRX","text":"S2S2 9N2SJ"}`. ADCS fields were placeholders (all zero) in the old firmware: tools ignore all-zero sensor words.
- **GS UART JSON lines** (`tools/common/events.py parse_gs_line`): `ACK`, `NACK`, `CWHK`, `CWRX`, `CAMERA` (pkt,total,offset,len,hex), `HK`, `RATE`, `FLASH`, `CWSIG/CWCAR/CWCHAR`; plain lines `[GS RX] Packet #n | RSSI=.. | SNR=.. | n bytes`, `[CW DECODED] ...`.
- **GS console commands** (`gs_app.c GS_Handle_Command_Line`): `1/HK/HK1`, `2/HK2`, `3/SNAP/CAMON`, `DUMMYCAM/DC`, `5/CD/CAM`, `4/D [cnt]` / `D ADDR CNT`, `P/PING` (F5, not HEALTH SAT), `ADCS`, `EPDM` (run missions), `ADCSFILE [n]`, `EPDMFILE [n]`, `FLAGS [n]`, `SATLOG [n]`, `RSVTABLE [n]` (file downloads), `ANT`, `DIGI SRC>DEST,VIA:text`, `BURST`, `S`, `AUTO ON|OFF`,
  `HEX <bytes>`, `PA LP|HP`, `PWR <dbm>`, `CARRIER <s>`, `BW CW|CWB|DEV|MOD [s] [dBm]` (TX spectrum test, RFO_HP, default and max +10 dBm, CWB = keyed Morse beacon), `LISTEN [ms]`, `RATE ON|OFF|?`, `HELP`.
### 4.3 Integrity check (AUTH)
Keyed 64-bit tag (hand-written Thumb-1 `cpu2/common/protocol/seal_tag.S`) + replay counter on payloads <= 107 B: trailer `[payload][counter(4, big endian)][tag(8)]`; both ends need the same key (`generated/secrets.h`) and AUTH setting;
counters are RAM-only (a rebooted GS restarts at 1: send the VCP `RESET` to clear the satellite's floor). Boot self-test word `g_sealSelfTest` = `0xA5A50001`. **AUTH is OFF in the current `.s2s2.config`.**
### 4.4 Digipeater and store-and-forward (`cpu2/common/digipeater`, `cpu2/satellite/app/satellite_digi.c`)
Rules (full list `docs/DIGIPEATER.md` section 4): frames with a digipeater path and a good FCS go to the engine instead of the command logic: own call / alias / `WIDE2-N` (N <= max, trace optional), H-bit loop check,
duplicate cache 30 s, per-station minimum interval, token bucket (default 6 repeats/min, burst 2), size check, reason codes. Repeat is re-framed with `Protocol_CreateRawPacket` and sent after a 300 ms guard.
Store-and-forward (`saf.c`): 12 messages, `max_forwards` 3, `forward_interval` 15 min, `ttl` 6 h, hop budget = the WIDEn-N of the path; journal `/obc/saf.jrn` on CPU1 LittleFS via the SRAM2 mailbox `0x20008000`
(`cpu2/satellite/ipc/saf_shared.h`). `DIGI=FALSE` removes it (+5.1 KB, +2.7 KB for SAF). Self-test word `g_digiSelfTest` = `0xA5A50002`. Test option `S2S2_SAF_APPEND_STATS` appends store stats to the repeated text.
"Hopping" = hop budget (not frequency hopping; asked once, never confirmed by the user).
### 4.5 Memory map and IPC
- Flash: CPU1 NuttX image `0x08000000` (nuttx.bin must stay < `0x08024000`; now 143,128 B of 147,456 = 97.1 %), LittleFS pages 72..99 `0x08024000..0x08031FFF` (mounted `/obc`), CPU2 image `0x08032000` (56 KB; satellite ~50.2 KB = 87.6 %).
- SRAM1 `0x20000000` 32 KB (CPU1 NuttX), SRAM2 `0x20008000`: SAF mailbox `0x20008000`, shared flash ring `0x20008840`, OBC block `0x20008F00`, CPU2 RAM `0x20009000..0x2000FFFF` (28 KB, stack top `0x20010000`, heap reserve 0x200, min stack 0x400).
- OBC block (`cpu2/satellite/ipc/obc_shared.h`, magic "OBC1" 0x4F424331): `cpu2_beat` (liveness), `session`, `tx_req_seq`, `tx_done_seq`, `tx_status`, `reset_req/ack`, rate-test fields, CW-test fields (`cw_test_mode` 0 HELLO, 1 GMSK, 2 CW+carrier).
- CPU2 boot: option bytes `C2OPT=1` (flash), `SBRV=0xC800` (0x08032000); CPU1 sets `PWR_CR4.C2BOOT` (+ SEV, patched in `stm32_pwr_boot_c2`). **Keep `CONFIG_ARCH_BOARD_FLASH_MOUNT` off** (NuttX partitions overlap CPU2 flash).
### 4.6 CPU1 app and VCP protocol (`apps/examples/s2s2obc/`)
- `s2s2obc_main.c` (init task, no NSH): `mount_store()` (~line 208: `progmem_initialize` -> `mtd_partition(72, 28)` -> `register_mtddriver` -> `nx_mount(.., "/obc", "littlefs", 0, "autoformat")`), frame handlers, ring feeding, LEDs
  (blue PB15 writing flash, green PB9 reading, yellow = green+red CPU2 TX). `s2s2obc_saf.c` the digipeater journal. Kconfig: FIRST_PAGE 72, PAGES 28. NuttX `.config`: `FS_LITTLEFS=y`, factors = 1 (default 4x -> ENOMEM), `SERIAL_TERMIOS=y`,
  `INIT_ENTRYPOINT="s2s2obc_main"`, serial console off LPUART1 (CPU1 owns LPUART1 = the ST-Link VCP).
- Frames PC -> CPU1: `C5 x8, A7, cmd, seq(2 LE), total(2 LE), len, data, crc16(2 LE)` (CRC-16/CCITT-FALSE over cmd..data). CPU1 -> PC: `A5 5A cmd seq(2) status 0D 0A`; READF/INFO/SAF also `A5 5C seq(2) len data crc16 0D 0A`.
  cmds: 02 STATUS (0 = CPU1 up AND CPU2 heartbeat advancing; 7 = CPU2 not answering), 03 RESET (clear CPU2 replay floor), 04 new file / ERASE (empties the whole store), 05 STORE, 06 SENDF, 07 READF, 08 SETNAME,
  09 INFO, 0A RATE, 0B CWTEST, 0C SAF (seq 0 status, n record, 0xFFFF clear), 0D INJECT (frame body to the digipeater). Open the VCP with DTR/RTS LOW (a normal open resets the board).
### 4.7 Build system
`make` = menuconfig TUI (`tools/config/s2s2_tui.py`, tree in `s2s2_core.MENUS`); `.s2s2.config` (JSON, git-ignored) -> `generated/{s2s2_config.h, s2s2.mk, secrets.h}` (regenerated by every make);
`cpu2/common/cpu2.mk` shared rules, outputs `build/<image>/` (repo root). Options: AUTH, DEBUG (-Og does not fit 56 KB), LOG (**LOG=0 corrupts downlink frames: open bug, keep LOG=1**), FRONTEND NUCLEO|CUSTOM, TIMER_IF_USE_RTC, DIGI and its limits.
Toolchain: official ARM GNU 15.3 (`/Applications/ArmGNUToolchain/*`); Homebrew arm-none-eabi-gcc 16 cannot find newlib headers; unset CPATH/C_INCLUDE_PATH. CPU1: `cd nuttx && gmake -j6` (`brew install make flock`).
Flash: `make flash` (CPU2), `make flash-m4 SERIAL=<probe>` in `cpu2/satellite` (CPU1), `make c2boot` (option bytes), `flash-all`.

## 5. Tools: shared code (tools/common) and the two programs
All Python is standard library (+ pyserial, Pillow, optional pillow-heif, websockets for screenshots); front ends are React 18 UMD + htm, NO build step, vendored in `tools/common/web/vendor/`.
### 5.1 tools/common
`httpx.py` (router decorators `@get/@post(path|^regex, public=, role=)`, handlers return dict/list/Raw/(status,obj); HTTP/1.1 keep-alive, gzip >= 1 KB, ETag + 304, 1-day vendor cache, early-error POST body drain, SSE batching (events within 80 ms
sent as ONE array message; browsers get `JSON.parse(m.data)` = array)), `events.py` (GS line parser, `decode_hk`, `decode_beacon`, SQLite `EventLog` ring + persistence), `authz.py` (PBKDF2-SHA256 200k, roles, lock-out 5 tries/5 min, sessions 12 h),
`imagestore.py` (fragments -> disk at once, background worker rebuilds, zero-fill partial pictures, finish = sha256 + compare with announced), `gslink.py` (GS UART, reconnect, DTR/RTS low), `web/ui.js` (`UI.h`, Metric, Spark, Cube + `attitude()`, Boundary),
`web/ui.css`, `web/vendor/*`.
### 5.2 Mission Control (`make mission`, 8770, `tools/mission_control/{server.py, mc/{core,web,orbit,store,link,passes,gdrive,xlsxio,docxgen}.py, mc/passes.js, static/}`) -- redesigned 2026-10-28 / 29, guide: `docs/MISSION_CONTROL.md`
Production; GS UART only. Page-based UI (no dialogs). **Home** (`#home`): ground-station overview (storage, Google Drive live stats, operational reports with a day puller, *Go to satellite control*); **Satellite control** (`#dashboard`): status, link supervision (heartbeat PING every 30-40 s: responsive /
command mode / LOC via TLE; last contact + RSSI come from it; **off by default**, switch on in the UI), CW beacon, telemetry, all commands, map; command page `#cmd/<id>` (no password / role; only the antenna asks once); history `#history` / `#job/<id>`; Telemetry tabs (housekeeping, ADCS, EPDM, digipeater); 3D Position and
attitude (three.js + satellite.js + NASA texture); log; images; guide + tour; settings (Drive, storage); report viewer `#report/<day>`. Overpass alert in the browser (30 min, every minute, until "Yes, I am ready"). ADCS / EPDM data is shown only as decoded text (`RAW_ONLY`, formats not final).
**Data store** `GS_Data/<YYYY-MM-DD, HH_MMAM>/` (one folder per day, uploaded to the Google Drive folder `S2S_operational_reports`; git-ignored; `S2S2_GS_DATA` / `S2S2_MC_DATA` env override for tests): Storage/*, Mission_Details/{ADCS,EPDM,CAM,DPM}, GS_report.xlsx, blank + auto-filled operational report .docx; regenerated from the databases; 7-day local retention, deletion only after upload to Google Drive.
Catalog (`mc/core.py CATALOG`; `SIMPLE` names / opcodes / expected replies, `HELP` texts): health, hk2, hkdl, ping, adcs/epdm (run missions), adcs_file/epdm_file/satlog/flags/rsvtable (file downloads), cam_run, cam_dl, dummycam, digi, antenna. Executor: queue, repeat the GS line every 14 s up to 8 times
until ACK, then wait for ACK completed; `TX` lock shared with the heartbeat; SQLite `commands`, `audit`, `raw`, `hb`, `settings` in `data/mission_control/mission.db`, `events.db`, images, `users.json`.
**Mock mode** (user request 2026-10-28, exception to the no-simulation rule): only by the key sequence Up Up Down Down Left Right Left Right B A; client side only (`static/mock.js`), banner + MOCK watermark, nothing sent or stored, real data restored on exit; mock data follows the real firmware layouts.
Admin login of the dev machine was set to `HELLO` on 2026-10-28 (localhost only; the 8-character rule is in `authz.set_password`, the hash was written directly). First start prints a random admin password once; delete `data/mission_control/users.json` to reset.
### 5.3 Testbench (`make testbench`, 8765, `tools/testbench/{server.py, tb/, static/, scenarios/, samples/}`)
No login (127.0.0.1). Pages: Dashboard, Consoles, Profiler, Alerts, Command tester, Digipeater, Images, Radio/SDR, Tests/E2E, Scenarios, Trends/compare, Logs, Reports/shots, Configuration, Build/flash. Top-bar camera = screenshot.
**tb modules**: util (paths, OBC ids, DEFAULTS incl. probe serials, `FLASH_BYTES` 44 KB), bus (`Bus` SSE hub with 3000-event history except chunk/prof, `EVENTS`, `IMAGES`, `log`), links (`GSLink`, `ObcLink` with ack/flashdata parsing, reconnect, `sat_fresh_start`),
state (`RUN` lock/abort, `STATE`, `PROG`, `set_result`), imaging (`IMG`, fit_jpeg, set_image), devices (`LedTrace` GPIOB over SWD, `Sdr`, `DwCapture` = rtl_fm | direwolf KISS), digi (saf_read, digi_inject), usb (ioreg listing; never `st-info --probe`),
build (make/flash runners, git info), tests_link / tests_image / tests_digi (the original tests), tests_extra (new tests + `Swd` helper + pure analysis helpers), e2e (`test_e2e`, `image_tx_only`), profiler (`OpenOcd`, `Elf`, `Session`, `Profiler`),
profiling (`PROF`, `prof_targets`, SDR `MON`), commands (GS command jobs `COMMANDS`, config get/set via `tools/config/s2s2_core`), consoles (GS/OBC palettes, `obc_cmd`, `obc_raw`), registry (`TESTS` dict), runner (`start_test`, fills TESTS, NEEDS check),
status (`get_status`, `live_state`), reports (shots, `save_report`), reporting (headless Chrome: screenshot through DevTools protocol, PDF, ZIP), logstore (`LOGS`), trends, scenarios, watchdog (`WATCH`), web (routes), compat (optional imports + sys.path).
**Routes** (tb/web.py): `/api/status /live /commands /job /history /console /gs/connect|disconnect|send /obc/connect|disconnect|cmd|raw /cmd /sat/reset /images... /image/select|refit|upload|source|received /samples /digi/inject|store|clear
/prof /prof/start|stop|watch|unwatch|peek|stack|ramsyms|symbols /test/start|stop /build /flash /monitor /config /shots /shot /shots/delete /reports /report /logs /logs/export /trends /trends/diff /scenarios /scenario/save|validate|delete /alerts /alerts/ack /watch`.
**CUBUS / DEBUG:** `tb/cubus.py` + `static/pages_debug.js`. With `cubus` in `.s2s2.config` the second link is the satellite's debug UART (text only), bench-app tests are blocked (`CUBUS_BLOCKED`), the sidebar has a `DEBUG:` group
(CUBUS debug = SWD snapshot / stage diagnosis from `g_satRx` / receive test `cubusrx` / pin check, CUBUS pins). Reads over SWD only on a button press; the probe is released for the profiler and for flashing. Details: docs/CUBUS.md.
**Profiler** (`tb/profiler.py`): per board one OpenOCD (`interface/stlink-dap.cfg`, `transport select dapdirect_swd`, `adapter serial <probe>`, `set DUAL_CORE 1`, `target/stm32wlx.cfg`, `reset_config none`, `adapter speed 4000`, Tcl port, stderr to a FILE).
Works with both probes. Non-intrusive reads: DWT_PCSR `0xE000101C` of `stm32wlx.cpu0` (M4) and `stm32wlx.cpu1` (M0+) ~450 samples/s/core, mapped to functions with `arm-none-eabi-nm -S -n`; idle = PC in `up_idle`/WFI-like functions; SRAM 64 KB `dump_image` ~1 Hz -> heat map vs the linker
layout (CPU2 `.data/.bss/heap/stack`, IPC 0x20008000.., CPU1 SRAM1); watch variables (ELF symbols, `read_memory`); peek; stack watermark = halt cpu for ~5 ms (`targets stm32wlx.cpu1; halt; capture "reg sp"; resume`), paint `0xDEADBEEF` below SP-64 down to `_ebss+0x200`, read back later.
**Tests** (`NEEDS` swd/obc/gs/none; run one at a time through `start_test`):
- original: `link` (PING xN), `image` (PC -> CPU1 LittleFS -> read back -> ring -> CPU2 -> RF -> GS; stage bars, LED phases, hashes), `sdr` (rtl_fm | direwolf), `rate`, `cwtest`, `sdrimage`, `imageloop`, `digi` (8 cases + journal), `e2e`.
- SWD only: `fwcheck` (flash vs build for CPU2 + NuttX), `bootcfg` (SRRVR SBRV 0xC800, PWR_CR4 C2BOOT, vector table, self-test words, OBC heartbeat), `membudget` (no hardware; limits flash 97 %, RAM 90 %), `profcap`, `lfsproof` (pages 72..99, 'littlefs' magic at byte 8, >= 95 % of picture chunks).
- Nucleo VCP: `vcpfuzz` (bad CRC, bit flip, truncated, garbage, short sync, 50-frame burst; alive afterwards), `flashio` (LittleFS write/read-back/speed; REPLACES the stored picture; needs `confirm`).
- GS + satellite: `cmdconf` (HK1, HK2, PING, D 1, ADCS, EPDM, invalid opcode -> NACK 0x09 with latencies), `nackmatrix`, `beacontime` (period vs `beacon_period_s`, callsign, RSSI), `winmap` (PING at offsets of the 90 s cycle: the listen window), `soak` (PING per cycle for N minutes).
- `scenario` runs a JSON sequence (`tb/scenarios.py`; built-in `tools/testbench/scenarios/{boot_check,link_check,regression}.json`, yours in `data/testbench/scenarios/`). Step types: gs, obc, expect (event wait with resend), wait, test, screenshot, report, log; `"continue": true`.
**Other features**: `tb/logstore.py` (all GS/VCP/SDR/bench lines in `data/testbench/logs.db`, 14 days, LIKE or regex search, CSV), `tb/trends.py` (history + diff of `docs/reports/*/report.json`), `tb/watchdog.py` (alerts for failed tests; optional monitors GS silence / VCP STATUS polling;
macOS notification; settings in `data/testbench/settings.json`; OFF until enabled), reports = `docs/reports/<time>/{report.html,report.pdf,report.zip,report.json,events.json,profiler.json,memory_layout_*.json,gs_uart.log,sat_vcp.log,screenshots/,...}`.
Runtime data (git-ignored): `data/testbench/{events.db,logs.db,images/,shots/,scenarios/,settings.json,prof/}`, `data/mission_control/{events.db,mission.db,users.json,images/}`.
### 5.4 Older tools still in the repo
`tools/testbench/{saf_tool.py (CLI for the digipeater journal: needs the testbench stopped, it owns the VCP), e2e_ping.py, flash_proof.py (raw SWD dump of LittleFS pages, CLI), verify_report.py (rebuild a report from the raw GS log), direwolf_s2s2_4800.conf}`,
`tools/flash/{set_c2boot.py, boot_cpu2.cfg}`, `tools/ground_software/` = LEGACY for an older firmware (do not extend; not used).

## 6. Evidence index (what was really proven)
`docs/reports/20261008_172200` image over GS (183/183, LED phases, flash dump 180/183); `20261008_191541`/`194010`/`20261009_173521` later image runs (VERIFIED); `20261009_175159` air-rate test (100/100 per gap); `20261010_133730` image decoded only by RTL-SDR + stock Dire Wolf (183/183,
byte-identical); `docs/proof/direwolf_hello_fecoff_terminal.txt`, `direwolf_camera_raw_*`, `direwolf_hello_raw_*`, `saf_digipeater_direwolf.txt` (digipeater repeats + stored forwards decoded by Dire Wolf); 2026-10-08 E2E on air: wrapped PING -> ACK accepted/completed + "PING OK (OBC_ONLINE)", RSSI about -89 dBm.
Digipeater host tests: 4,067 + 53 checks; on-chip self-tests `0xA5A50001` / `0xA5A50002` read over SWD; bench digipeater test 8/8; store survives a full reflash.

## 7. Hardware facts and pitfalls (consolidated, do not re-derive)
- Boards: satellite Nucleo (V3 ST-Link `002D001B...`, VCP) and the GS Nucleo (V2 ST-Link serial `000000000001`) + FTDI `AG0L702O` UART. Other Nucleos carry other users' firmware: back up before overwriting. The ST-Link V3 USB serial dropped out several times earlier
  (cause: a status refresh ran `st-info --probe` which RESETS the board and `rtl_test`; replaced by ioreg reads). **Never run `st-info --probe`** (it was run once by mistake this session and reset the satellite; `make probes` and the TUI probe scan now read the USB registry instead).
- A serial port can be used by ONE process (two processes steal each other's bytes). Open the VCP with DTR/RTS low. macOS has no `timeout`; the venv `tools/.venv` has a broken pip (moved dir): use a scratch venv for linting.
- The GS board stopped answering after the `DIGI` command (its flashed firmware is older than the repo); recovered with `openocd ... -c "reset run"` over the V2 probe (hla, `interface/stlink.cfg`). The GS still has the OLD firmware.
- RTL-SDR: tune about 5 kHz below 435.000 MHz (434.9936 works); it stalls after a killed capture (replug) and shows `PLL not locked!`; stop `rtl_fm` with SIGINT first; it was not enumerated late in the session.
- Real profile of the satellite: CPU2 100 % busy, never sleeps (`CPU2_Delay_Ms` 47 %, `uart1_rx_pump` 28 %, `CPU2_Poll_IPC` 24 %); CPU1 99.7 % in `up_idle`; CPU2 stack high-water 1,056 of 14,528 B.
- CPU1 NuttX image 97.1 % of its 144 KB budget below LittleFS: only ~4.3 KB headroom (real `membudget` finding). Satellite flash 87.6 % (50,244 B) in the ELF section sum; earlier documents quote 88.7 % / 95 % for other builds.
- OpenOCD: stderr must go to a file (an unread pipe froze it after ~15 s); Tcl-port output of `reg` needs `capture`; after a socket timeout reconnect; only one tool may hold an ST-Link; detach the profiler before flashing.
- Headless Chrome: use a throw-away `--user-data-dir`; `--screenshot` never finishes on pages with an EventSource (use the DevTools protocol, `tb/reporting.py`); Chrome does not exit by itself (kill the process group).
- Direwolf decodes our frames only via the SDR path or `atest`; KISS port 8001; `tools/testbench/direwolf_s2s2_4800.conf`.
- Frontend bugs found: htm passes `style` as a string (React needs an object): `UI.h` converts; `class` -> `className`; CSS 3D axes are left-handed (attitude cube); old history rows without HK fields crashed the dashboard (error boundary + guards);
  `Bus.publish(kind, **ev)` collided on `kind` (now `ev=` field); `Random()` created per byte made constant test patterns (flashio, fixed).

## 8. Status: verified vs NOT verified
- **Verified without boards (all green at the end)**: `sh tests/host/run.sh` (rt_auth, digipeater, saf, imagestore, testbench analysis, testbench features, attitude, Dire Wolf chain), pyflakes clean on `tools/testbench`, `tools/mission_control`, `tools/common`, headless-Chrome smoke of every page of
  both programs, `membudget` test run, a scenario run (log, screenshot, alert on FAIL), report + PDF + ZIP generation, HTTP layer (gzip, 304, auth 401/403, SSE batching).
- **Run on hardware this session (before "no hardware")**: GS console `S`, `HK1` (ACK accepted + HK packet decoded; the final ACK completed was missed), Nucleo `STATUS`, GS reset over SWD, profiler + stack watermark on the satellite, profiler attach on the GS.
- **NOT run on hardware**: live HK / CW beacon / ACK through the new parser in Mission Control, camera download into the background image builder via the GS, `DIGI` from the GS (hangs the OLD GS firmware), watchdog monitors, scenario steps that talk to boards, tests fwcheck /
  bootcfg / lfsproof / vcpfuzz / flashio / cmdconf / nackmatrix / beacontime / winmap / soak / profcap, e2e with receivers, SDR monitor in the testbench.
- Older open facts: RF link proven only 2026-10-08 on coax; AUTH currently OFF; CW beacon number ambiguous; `make LOG=0` bug; replay counters RAM-only; satellite receiving an over-the-air digipeat request is untested; HK/telemetry AX.25 packets 0xB1/0xB2 and the 2-byte data-packet header of the sheet are not implemented.

## 9. How to work (commands)
1. Run: `make mission` / `make testbench` (or `tools/.venv/bin/python tools/<tool>/server.py [--port N] [--no-browser] [--gs '']`). Stop: `pkill -f server.py; pkill openocd`.
2. Host tests: `sh tests/host/run.sh` (needs clang; Dire Wolf `atest`+`gen_packets` optional; `node` for the attitude test).
3. UI smoke: `tools/.venv/bin/python tests/host/ui_smoke.py http://127.0.0.1:8765 dashboard consoles profiler alerts commands digipeater images radio tests scenarios trends logs reports config build [--shots dir]`;
   Mission Control: `... http://127.0.0.1:8770 overview attitude telemetry beacons commands imagery admin --login admin:<password>`.
4. Lint: `python3 -m venv /tmp/lint && /tmp/lint/bin/pip install pyflakes` then `/tmp/lint/bin/python -m pyflakes tools/testbench/server.py tools/testbench/tb tools/mission_control/server.py tools/mission_control/mc tools/common/*.py`.
5. Firmware: `make build`, `make flash` (CPU2 of selected targets), `cd cpu2/satellite && make size`; read results over SWD (`openocd ... mdw`) when the VCP is silent.
6. After changing static JS: browsers cache by ETag with `no-cache` (revalidate): a plain reload picks changes up; the vendored React is cached 1 day.

## 10. Decisions and rejected approaches (do not redo)
- FEC (Reed-Solomon-like wrap in asm) REMOVED: could not cover 138+32-byte camera fragments, no measured benefit. AUTH kept as a trailer. AFSK1200 software modem REMOVED (GMSK only). `-flto`, `-fno-jump-tables`, `-fmerge-all-constants` rejected.
- `LOG=0` stays broken (cause unknown); do not ship it. Direct (no storage) image mode removed: only the CPU1 LittleFS path exists.
- No simulators (section 2). Web stack = stdlib HTTP + vendored React UMD + htm (no node build); Mission Control did not reuse the legacy FastAPI `mission_control_api.py` (older firmware protocol); both programs share `tools/common`.
- Screenshots/PDF via local Google Chrome headless (no extra dependency except `websockets`); PDF/ZIP reports are the proof artefacts.
- Attitude from a magnetometer alone is 2 DOF: the UI shows body axes vs the field and says so (no invented full attitude).

## 11. Next steps (suggested order; confirm hardware actions with the user)
1. Reflash the GS with the current firmware (`cd cpu2/ground_station && make flash SERIAL=<V2 serial>` after `make build`); then real runs: Mission Control HEALTH SAT, scenarios `boot_check` and `link_check`, tests `cmdconf`, `beacontime`, `winmap`, `soak`.
2. Image test then `lfsproof` with the same picture selected (proves the file is in LittleFS); `flashio` only if replacing the stored picture is fine.
3. Check what the satellite's ADCS/HK fields really contain (all-zero placeholders are ignored by the Attitude page).
4. Decide how to free CPU1 flash (97.1 % of budget) before adding NuttX features; consider CPU2 sleeping instead of 100 % busy-polling (profiler finding).
5. Optional: raw-frame view in the testbench (GS StartFlag/EndFlag beside the Dire Wolf hex), alert limits for Mission Control, HK/beacon AX.25 packets of the sheet, commit the work.
