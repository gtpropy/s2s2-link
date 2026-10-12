# S2S2 CubeSat COM firmware

Dual-core STM32WL55JC1 communications subsystem: **CPU1** (Cortex-M4) runs NuttX and owns the image store, **CPU2**
(Cortex-M0+) runs the bare-metal radio stack (4800 bit/s GMSK / G3RUH AX.25 + CW beacon). One image for the satellite,
one for the ground station.

## Quick start

```
make                 # menuconfig (NuttX style menu tree): configure everything, build, memory bars, flash
make configure       # no UI: generate generated/{s2s2_config.h, s2s2.mk, secrets.h} from .s2s2.config
make build           # build the selected targets        make flash   # flash CPU2 of the selected targets
make mission         # Mission Control: production operator software, ground-station board only (http://127.0.0.1:8770, login)
make testbench       # Testbench: debug consoles of both boards, SWD profiler, tests, end-to-end, reports (http://127.0.0.1:8765)
make flash sat CUBUS=T # flash only the satellite, built for the RSMS CUBUS V.3 flight board (docs/CUBUS.md); sat / gs restrict build / flash / clean to one image
make test            # host tests (framing, integrity check, digipeater, store and forward, Dire Wolf cross-check)
```

The menuconfig (`tools/config`, a menu tree like NuttX's: Enter opens a submenu or edits a value, Space toggles `[*]`, `?` shows the help of an item, Esc goes back, `s` saves, `q` quits; every change is saved and generated at once) is the single place for all settings: targets, AUTH, build options, call signs, SSIDs,
frequencies, CW / beacon timing and the ST-Link probes. It writes `.s2s2.config` and regenerates `generated/`
(git-ignored); the Makefiles in `cpu2/` read it, so a plain `make` in a firmware directory uses the same settings.
`generated/secrets.h` holds the random AUTH key shared by both images; it is created automatically the first time
(`make configure` or the TUI) and replaced only by `make newkey` (then rebuild and reflash **both** boards).
Needs: ARM GNU Toolchain, STM32CubeProgrammer or OpenOCD, GNU make; `setup.sh` installs them.

## Layout

```
cpu2/                       CPU2 (Cortex-M0+) radio firmware
  common/                   code identical in both images
    protocol/               AX.25 + G3RUH framing, packet API, integrity check (seal_wrap.c, seal_tag.S)
    system/                 startup glue, trace, memory, low power, sequencer, mini_printf (our ll_port.h stays in system/include/)
    drivers/                SubGHz SPI driver (LL)
    ipc/                    shared-RAM ring buffer (CPU1 <-> CPU2)
    digipeater/             APRS / AX.25 digipeater engine + store-and-forward memory (pure logic, see docs/DIGIPEATER.md)
    vendor/                 ST SOURCES only (.c): HAL RCC, SX126x radio firmware, timer. Their headers live in include/
    cpu2.mk                 shared build rules, options, flash recipes
  satellite/                satellite image: app/ mission/ board/ system/ radio/ uart/ ipc/ boot/ + Makefile
  ground_station/           ground-station image: same structure
tools/
  config/                   the TUI and the generator (s2s2_tui.py, s2s2_core.py)
  mission_control/          production operator software (server.py + package mc/): telemetry, attitude from CW beacon 2, commands, imagery, accounts; talks only to the GS board
  testbench/                debug software (server.py + package tb/): consoles, SWD profiler, 20+ tests, scenarios, logs, trends, alerts, screenshots, reports (HTML/PDF/ZIP)
  common/                   shared by both: httpx.py (HTTP layer), GS line parser + SQLite event log, accounts, image store, GS serial link, web/ (ui.js, ui.css, vendored React)
  ground_software/          older ground tools for the previous firmware (SDR decoder, old web console and mission-control UI, GUIs); not used by the two programs above
  flash/                    CPU2 boot option byte helper
tests/
  host/                     host tests of the framing and integrity check
  tag_asm/                  emulator test of the assembly tag function
docs/                       CHANGES.md, spec (Packet Format sheet), hardware reports, proof captures, test reports
include/                    imported / ST-generated headers of the CPU2 images (cmsis, stm32wlxx_hal, st_radio, st_utilities, st_system,
                            st_bsp, satellite, ground_station), next to the NuttX board header board.h. See docs/LAYOUT.md
build/                      all build outputs, one folder per image: build/satellite/, build/ground_station/ (git-ignored)
generated/                  generated config + secrets (git-ignored; NOT in build/: `make clean` must never delete the AUTH key)
Kconfig CMakeLists.txt configs/ scripts/ src/ + include/board.h    NuttX board directory for CPU1 (nucleo-wl55jc), must stay at the root
nuttx/  apps/                                             NuttX and NuttX apps (submodules); apps/examples/s2s2obc is the CPU1 image store
```

Per image: `app/` is the application, `mission/` the mission protocol and radio app, `board/` the RF front end and board
glue, `system/` interrupt / clock / timer glue, `radio/` the SX126x driver glue, `uart/` the console, `ipc/` the CPU1
link, `boot/` startup code and linker script. Build outputs go to `build/<image>/` at the repository root (`.elf`, `.bin`, `.map`, objects,
`.opts`); ST / imported headers are in `include/<group>/`. Details and the rules for adding a header: `docs/LAYOUT.md`.

Digipeater: `docs/DIGIPEATER.md`. Details of what was changed against the reference firmware: `docs/CHANGES.md`. Project notes: `CLAUDE.md`.
