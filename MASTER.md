# S2S2 -- Master Documentation

Single entry point into this repo. Everything here is a pointer to the
real, detailed doc or source file -- this file stays short on purpose;
when it and a linked doc disagree, the linked doc (or the source) wins.

## 1. What this is

A 1U amateur UHF CubeSat digipeater ("S2S2") and its ground segment:
firmware for both the satellite and the ground station radio, plus the
tools to build, flash, operate, and monitor them.

**Hardware**: STM32WL55JC, a dual-core SoC on both the satellite and the
ground-station board:

| Core | Role | OS / build system |
|---|---|---|
| CPU1 (Cortex-M4) | Main OBC application | NuttX RTOS (`nuttx/`, `apps/`, board files under `nuttx/boards/arm/stm32wl5/nucleo-wl55jc/`) |
| CPU2 (Cortex-M0+) | Standalone real-time sub-GHz radio coprocessor -- RF PHY, GMSK modulation, S2S2 link layer (RS FEC, AX.25/FX.25 framing) | Bare-metal C, no RTOS (`CPU2/`) |

CPU1 boots CPU2 on startup (`stm32wl5_pwr_boot_c2()`, gated by
`CONFIG_ARCH_BOARD_ENABLE_CPU2` -- see §4). The two cores talk over IPCC
and shared SRAM2; CPU2 otherwise runs the entire radio stack
independently once booted.

Full CPU2 architecture: [`CPU2/README_GMSK_S2S2_ARCHITECTURE.md`](CPU2/README_GMSK_S2S2_ARCHITECTURE.md).

## 2. RF link parameters

- **Frequencies**: satellite TX 435.000 MHz / RX 437.375 MHz; ground
  station is the mirror image (TX 437.375 / RX 435.000). See
  `CPU2/core/Inc/config.h`.
- **Modulation**: GMSK realized on the SX126x's GFSK modem (h = 0.5,
  Fdev = bitrate/4, BT = 0.3 Gaussian shaping).
- **Operating bit rate: 4800 bps.** `RADIO_BIT_RATE_BPS` in
  `CPU2/core/Inc/config.h`, with `RADIO_FDEV_HZ = 1200` and
  `RADIO_RX_BANDWIDTH_HZ = 9700` (nearest real SX126x discrete
  bandwidth step above the Carson's-rule minimum) derived from it. This
  is the `S2S2_PHASE_MID` design point in `s2s2com.h`'s per-elevation
  table, now the fixed operating rate rather than just a mid-pass
  fallback -- better link margin than 9600 bps at half the throughput,
  without the 1200 bps `S2S2_PHASE_HORIZON` phase's convolutional-code
  overhead. `s2s2com.h`'s three-phase table (1200/4800/9600) still
  exists as the adaptive-rate design reference; it is not wired into
  the runtime radio init.
- **Framing**: real AX.25/FX.25 UI frames (any AX.25/FX.25-aware ground
  station -- SatNOGS, gr-satellites, Direwolf -- can receive S2S2
  traffic with no custom software). FX.25 correlation tag 0x06,
  shortened RS(255,223): 128 data + 32 parity = 160-byte RS block +
  8-byte tag = 168-byte on-air frame, t=16 correction strength.
  Callsigns are placeholders (`NEPSAT`/`GROUND`) -- swap in a real
  assigned amateur callsign before any actual RF transmit.
- TX power: `RADIO_TX_POWER_DBM` = 22 dBm (production); DEBUG_BUILD adds
  a runtime `POWER <dbm>` override for bench testing.

Custom packet framing (non-AX.25 path): [`CPU2/format.md`](CPU2/format.md).
Command set: [`CPU2/format_commands.md`](CPU2/format_commands.md).
Digipeater mission design: [`CPU2/DPM_ARCHITECTURE.md`](CPU2/DPM_ARCHITECTURE.md).

## 3. Repo layout

```
CPU2/                  CPU2 (M0+) bare-metal firmware -- satellite + GS builds
CPU2/tools/            ground-segment Python tools (decode, GUIs, mission control, FLASHER)
nuttx/, apps/          git submodules: apache/nuttx + apache/nuttx-apps (CPU1 OS)
nuttx/boards/arm/stm32wl5/nucleo-wl55jc/   the actual CPU1 board port (buildable)
src/, include/, configs/, scripts/, Kconfig, CMakeLists.txt   (top level)
                       original CPU1 board files -- reference copies; the
                       buildable, corrected copy lives inside nuttx/ above
                       (see nuttx/boards/.../ vs. these -- they differ: the
                       top-level ones predate the stm32_* vs stm32wl5_*
                       symbol-prefix fix, don't build against upstream NuttX as-is)
```

`~/nuttxspace/Reedsolomon/` (outside this repo) is a local scratch build
tree: same layout, plus the built `kconfig-frontends`/`nuttx-tools`
toolchain support files (not vendored here -- local build tools, not
project code).

## 4. Building + flashing CPU2 (satellite / ground station firmware)

### FLASHER (recommended)

```
python3 CPU2/tools/flasher.py
```

Textual TUI: pick **mode** (Satellite / Ground Station), **build**
(debug/production) and **board** (nucleo/gs_custom/sat_custom), pick
the **ST-Link probe** from the detected list (serial-number aware --
safe with multiple probes plugged in), then **Build**, **Flash**, or
**Build + Flash**. Also does **Reset** and a byte-for-byte **read-back
verify** against the on-chip flash. Live-streamed build/flash log,
color-coded status bar. Keys: `b` build, `f` flash, `shift+b`
build+flash, `r` refresh probes, `q` quit.

It's a thin wrapper -- everything it runs is also runnable by hand:

### By hand

```
cd CPU2
make sat                              # -> ipcc_m0plus.{elf,bin,map}
make gs                               # -> gs_m0plus.{elf,bin,map}
make sat BUILD=production BOARD=gs_custom   # any APP x BUILD x BOARD combo
make flash_sat                        # openocd + ST-Link, flashes 0x08020000
make flash_gs
st-info --probe                       # list connected ST-Link probes by serial
st-flash --serial <SN> --reset write ipcc_m0plus.bin 0x08020000   # what FLASHER runs
```

`APP=satellite` (default) builds `ipcc_m0plus_main.c` -> `ipcc_m0plus.{elf,bin}`.
`APP=gs` builds `satellite/gs_main.c` -> `gs_m0plus.{elf,bin}` (yes, the
GS main lives under `CPU2/satellite/` -- existing naming, not a typo).
Both link to flash offset `0x08020000` (`scripts/wl55jc_cm0p.ld`), both
require the real ARM toolchain with newlib
(`/Applications/ArmGNUToolchain/15.3.rel1/`, not the headers-less
Homebrew `arm-none-eabi-gcc`) and `~/sdks/STM32CubeWL`.

### Building CPU1 (NuttX OBC)

```
cd nuttx
./tools/configure.sh nucleo-wl55jc:demo
make -j4
```

Needs `kconfig-tweak`/`kconfig-mconf` (build from
`github.com/patacongo/tools`'s `kconfig-frontends` -- the plain
upstream kconfig-frontends is too old to parse modern NuttX Kconfig's
`imply` keyword) and the same real arm-none-eabi toolchain (with
newlib) as CPU2, not the headers-less Homebrew package. `demo` defconfig
already sets `CONFIG_ARCH_BOARD_ENABLE_CPU2=y` +
`CONFIG_BOARD_LATE_INITIALIZE=y`, so CPU1 boots CPU2 on startup.

## 5. Mission Control dashboard

```
python3 CPU2/tools/mission_control_api.py
# -> http://localhost:7909/
```

FastAPI + WebSocket server, serves `CPU2/tools/mission_control_static/`
(vanilla JS + three.js). Talks to the GS board's real UART, persists
events/telemetry/commands to `CPU2/tools/mission_control.db` (SQLite,
**gitignored -- never commit it**, it holds password hashes).

Auth: PBKDF2 password hashes + signed session cookies (`auth.py`).
First run prints a one-time admin password. Lost it / need a fresh
admin account:

```
python3 CPU2/tools/mission_control_api.py create_admin_creds [username]
```

`CPU2/tools/.session_secret` is the HMAC signing key for session
cookies, generated on first run, **gitignored -- never commit or share
between deployments**; rotating it invalidates every session.

Critical commands (abort, camera, download, digipeater) additionally
require a shared operational passphrase whose SHA-256 is checked
against an admin-configured fingerprint (`/api/settings/critical_secret`)
-- the plaintext secret itself is never stored.

## 6. Ground-segment tools (`CPU2/tools/`)

| Tool | What it does |
|---|---|
| `flasher.py` | Advanced TUI: build + flash CPU2 firmware over ST-Link (§4) |
| `mission_control_api.py` | Web dashboard + API for the ground station (§5) |
| `auth.py` | Password hashing + session tokens for the dashboard |
| `gmsk_demod.py` | GMSK demodulator + frame sync for the S2S2 downlink, matching the firmware's exact PHY config |
| `s2s2_decoder.py` | Pure-Python receive-side decode engine, ported line-for-line from the CPU2 firmware C sources |
| `gs_protocol.py` | Pure-Python parser for `gs_main.c`'s real UART output + beacon telemetry decoding |
| `s2s2_rx_gui.py` | PyQt receiver GUI for the S2S2 GMSK/AX.25/FX.25 downlink (RTL-SDR or recorded IQ file) |
| `s2s2_tx_gui.py` | PyQt "backdoor" control panel for the Nucleo-WL55JC over its debug UART |
| `satellite_gui.py` | Bench console for the satellite board's own debug UART |
| `response_analyzer.py` | Structured "what did the satellite just say" decoder for command responses |
| `ml_denoise.py` / `denoiser.npz` | Small windowed MLP denoising the FM-discriminator output ahead of the bit slicer, trained on synthetic data; `denoiser.npz` is its trained weights |
| `cw_monitor.py` | Turns a real on-air CW carrier into an audible tone (bench monitoring) |
| `morse.py` | Shared Morse table + audio synthesis/decode, used by both GUIs |
| `audio_player.py` | Shared streaming audio output for both GUIs |
| `make_demo_capture.py` | Builds a synthetic `.cfile` IQ capture from real firmware-encoded frames, for demoing/testing without hardware |
| `test_decoder.py` / `test_demod.py` / `test_digipeater.py` | Self-tests: RS(255,223) decode, end-to-end demod+decode, digipeater path -- all run against real firmware-encoded bytes, no hardware needed |

## 7. Secrets -- never commit

Gitignored, generated locally, per-deployment:

- `CPU2/tools/.session_secret` -- dashboard session-signing key
- `CPU2/tools/mission_control.db` -- runtime DB (password hashes, telemetry)

If either shows up in `git status` as trackable, something is wrong --
check `.gitignore` before adding.
