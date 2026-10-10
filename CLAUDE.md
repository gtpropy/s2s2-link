# S2S2 CubeSat Firmware -- Project Notes

Dual-core STM32WL55JC1 comms firmware. CPU1 (Cortex-M4) runs NuttX (`nuttx/`,
`apps/` submodules). CPU2 (Cortex-M0+) runs the bare-metal radio stack.

## Layout

See `README.md` (layout, quick start). Short version: `cpu2/{common,satellite,ground_station}` = CPU2 firmware (**reference code**
from `github.com/PrmBRana/dummy-camera-live` commit `0b807890`, plus our additions; `common/` is what was byte-identical in both
images), `tools/{config,testbench,ground_software,flash}`, `tests/{host,tag_asm}`, `docs/`. The repo root `Kconfig`,
`CMakeLists.txt`, `configs/`, `include/`, `scripts/`, `src/` are the live NuttX board directory for `nucleo-wl55jc`: do not move or
delete them. Venv: `tools/.venv`. Old `JC2/`, `V1.4`, reference snapshots, `shareRAM/`, FEC tests were archived outside the repo
(`../s2s2-link_removed_2026-10-07.tar.gz`, `../s2s2-link_removed_2026-10-10.tar.gz`; full pre-cleanup copy
`../s2s2-link_before_cleanup_2026-10-10.tgz`).

## Build / flash / configuration

```
make               # TUI (menuconfig): configure everything, build, memory stats, pick ST-Links, flash
make configure     # generate generated/{s2s2_config.h, s2s2.mk, secrets.h} from .s2s2.config (no UI)
make defconfig | newkey | build | flash | flash-all | memstat | probes | clean | nuttx | testbench | test
```
`.s2s2.config` (JSON, git-ignored) is the single source of truth: targets, AUTH, DEBUG, LOG, FRONTEND, RTC timer, project name, call signs
and SSIDs, downlink/uplink Hz, CW dit ms, beacon period, probes. `generated/s2s2_config.h` carries the mission values into both images
(`S2S2_*` macros), `generated/s2s2.mk` the build options, `generated/secrets.h` the random AUTH key (created when missing, replaced only by
`make newkey`; never commit it; both boards must be built from the same file, compare the `key xxxxxxxx` fingerprint in both boot messages).
Per-image Makefiles also work directly: `cd cpu2/satellite && make [AUTH=TRUE|FALSE] [DEBUG=T|F] [FRONTEND=..] [SERIAL=<probe>]`,
`make flash | size | probes | c2boot | flash-m4 | clean`; outputs in `cpu2/<image>/build/`. `DEBUG=T` (-Og -g3) will not fit the satellite's 56 KB flash.

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

`make testbench` opens http://127.0.0.1:8765 (link test, image test, SDR+Direwolf test, proof reports). It needs two serial
ports and no SWD: the GS FTDI (`/dev/cu.usbserial-*`) and the Nucleo's ST-Link VCP (`/dev/cu.usbmodem*`).

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
