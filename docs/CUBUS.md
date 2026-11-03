# RSMS CUBUS V.3 board profile (`CUBUS=T`)

The flight board "RSMS CUBUS V.3" carries COM, OBC and the EPS bus on **one STM32WL55JC**: CPU2 (Cortex-M0+) is the radio, CPU1 (Cortex-M4)
is the OBC and the power controller. The schematic is `RSMS CUBUS V.3 COM SCH.pdf` (7 sheets). The satellite CPU2 image can be built for it:

```
make build sat CUBUS=T          # build only the satellite image for CUBUS
make flash sat CUBUS=T          # flash only the satellite's CPU2 (needs the board's ST-Link in the TUI probe list)
make flash sat CUBUS=F          # the same image for the Nucleo / FRONTEND setting
make                            # TUI: Build options -> "CUBUS: build the satellite for the RSMS CUBUS V.3 flight board"
cd cpu2/satellite && make CUBUS=T
```

`CUBUS=T|F` on the command line overrides the saved option for that run; the TUI / `.s2s2.config` (`"cubus": true`) saves it. `sat` / `gs` after
`build`, `flash` or `clean` restrict the action to one image; the ground station is always a Nucleo and ignores CUBUS. CUBUS replaces the
`FRONTEND` setting. Switching CUBUS rebuilds everything (the option string is part of the build stamp). `make flash-all` refuses with CUBUS: it
would overwrite the OBC firmware in CPU1 with the Nucleo image-store app.

What a CUBUS image reports: boot line `[BOARD] RSMS CUBUS V.3 (id 0xB0A2D003)` and the word `g_boardId` over SWD (`0xB0A2D001` Nucleo,
`0xB0A2D002` reference custom front end, `0xB0A2D003` CUBUS). Always check it before connecting power to the real board.

## What CPU2 owns on the board (sheet 4, "COM MCU")

| Pin | Net | Use by CPU2 |
|---|---|---|
| PB0 | VDD_TCXO | TCXO supply (SX126x DIO3 control), as on the Nucleo |
| RFO_LP | -> H6 0 ohm -> matching -> **ADL5324 (PA1)** | transmit. RFO_HP is not wired to the PA |
| RFI_P / RFI_N | RFI network -> RF_IN of the RF switch | receive |
| PC2 | AMP_3V3EN | gates the 3.3 V into AMP_VIN (Q8): the PA runs from 3.3 V for the CW beacon |
| PC3 | AMP_5VEN | gates the 5 V into AMP_VIN (Q7): the PA runs from 5 V for GMSK. Never both on |
| PC4 / PC5 | FE_CTRL1 / FE_CTRL2 | RF switch RFSW2 (VEN / VCTL): RX = 1/0, TX = 0/1 |
| PA0 | EN_5V_DC/DC&OCP | enable of the **bus** 5 V DC/DC and its 2 A over-current switch (sheet 2) |
| PA9 / PA10 | DEBUG_TX_COM/OBC, DEBUG_RX_COM/OBC | USART1 debug console (ACCESS connector) |

Everything else on the chip belongs to CPU1 and must stay untouched by the radio image:

| Pins | Nets |
|---|---|
| PA1, PB9, PB11, PB12, PB10, PB14, PB15 | ADC2_CS, **CS_MPU, CS_MAG**, ADC1_CS, ADC_SCK, ADC_MISO, **ADC_MOSI** (SPI2: the two ADS7953 ADCs, gyro, magnetometer) |
| PA2 / PA3, PC0 / PC1 | OBC_TX / OBC_RX (LPUART1), OBC_RX_1 / OBC_TX_1 |
| PA4..PA7 | SMSN_FM_CS / SCK / MISO / MOSI: the shared mission flash (SPI1) |
| PA8, PA11, PA12, PA15 | EN_UNREG, EN_GLOBAL_RESET, EN_3V3_MSN_DCDC&OCP, BURNER_SIGNAL (antenna burner) |
| PB1..PB7, PB13 | KILL_SWITCH2 +/-/STAT, KILL_SWITCH_EN, KILL_SWITCH1 +/-/STAT, KILL_SWITCH_ON |
| PB8 | **WD_WDI_OBC**: input of the watchdog U5 (see open points) |
| PC6, PC13 | GPIO2, GPIO1 |

The pin assignments were checked against the PDF's text layer: every net label sits at the x position of its pin name along the top of the MCU symbol
(PB6 = KILL_SWITCH1_-, PB8 = WD_WDI_OBC, PB9 = CS_MPU, PB11 = CS_MAG, PB15 = ADC_MOSI, PC0 / PC1 = OBC_RX_1 / OBC_TX_1) and at the y position along the left side
(PA9 / PA10 = DEBUG_TX / RX_COM/OBC, PA0 = EN_5V_DC/DC&OCP, PA8 = EN_UNREG, PA11 = EN_GLOBAL_RESET, PA12 = EN_3V3_MSN_DCDC&OCP, PA15 = BURNER_SIGNAL).

## What the CUBUS profile changes

Same radio, framing, protocol and mission logic as every other build. Only board handling differs (`SAT_CUBUS`, `cpu2/satellite/board/`):

1. **No PB6 alternate function** (`uart/uart_debug.c`). The Nucleo build also made PB6 a USART1 TX pin (Arduino D1). On CUBUS that is KILL_SWITCH1_-,
   a coil-drive line of the kill switch; the debug UART is PA9 / PA10 only.
2. **No LEDs** (`uart/obc_led.c`). The Nucleo LED pins PB15 / PB9 / PB11 are ADC_MOSI, CS_MPU and CS_MAG on CUBUS. `Obc_Led()` is a no-op.
3. **PA0 (EN_5V_DC/DC&OCP) is owned by CPU2, as in the reference firmware.** GMSK raises it, the end of a downlink drops it, `RBI_Init` drives it low. (The bus 5 V is shared with
   the BPB connector's "5 V -> 2 A line": nothing else may rely on that rail during a downlink.) The pin goes through `RBI_Set5VRail()` / `RBI_Get5VRail()`, plain GPIO writes on every board.
4. **The 3.3 V PA is off between transmissions** (`SAT_PA3V3_IDLE`). The reference firmware leaves PC2 high at all times. On CUBUS PC2 gates the ADL5324's
   supply, so that is a constant quiescent load (the reference's own RF report says PC2 is disabled in RX; its code left it on). The CW beacon (`Satellite_CW_Prepare`) and the GMSK sequence switch the PA on themselves.
5. Front-end pin logic is the reference "custom board" path (RFO_LP, `SAT_SET_PA_CONFIG` 0x04/0x00/0x01/0x01, OCP 0x18, 14 dBm drive, GMSK at 10 dBm on the 5 V PA,
   FE_CTRL1/2 table above). It matches the schematic; the stock-Nucleo block (RFO_HP, PC3 as switch enable) is not compiled.

6. **No invented telemetry** (`satellite_app.c`, `satellite_main.c`). The OBC (`apps/examples/OBC_main`) delivers beacon 1 (batt V, solar V, antenna T, batt T, BPB T) and beacon 2
   (3V3 I, total solar I, batt I, flag 1, flag 2) through the ring. The reference firmware filled every other HK channel with constants, fell back to made-up values when a
   word was 0, took the HK gyro words from the two flag words and the "5 V rail current" from the solar current, and answered a failed flash read with a synthetic record.
   On CUBUS: the HK packet carries exactly those delivered values in their slots (batt V, solar V, antenna / battery / BPB T, 3V3 I, battery I, the two OBC flags), everything else is 0
   (= not measured; the gyro / magnetometer are not delivered to CPU2 at all, total solar current has no HK slot and is in CW beacon 1 only); the start values of the beacons are 0
   instead of 4.00 V / 25.0 C; a flash read that the OBC does not answer sends one chunk with status 1 (flash read error) and no data.
7. **Probe scan never opens a probe.** `make probes` and the TUI probe picker used `st-info --probe`, which resets the board behind the probe; they now read the USB registry
   (`s2s2_core.scan_probes`). Safe with the flight board attached.

CPU1 side: `apps/examples/s2s2obc` has a Kconfig switch `EXAMPLES_S2S2OBC_LEDS` (default y, set in `nuttx/.config`); with it off the app never touches PB15 / PB9 / PB11.

## Flash lock: no Nucleo image on the CUBUS board (2026-10-12)

`make flash` and `make flash-m4` (all routes: root Makefile, TUI, `cd cpu2/<image> && make flash`, testbench Build page) first run `tools/flash/board_guard.py`.
It reads the attached chip over SWD, read-only (same bare MEM-AP method as `swd_rx_check.sh`: no reset, no halt, no write, 500 kHz; the script is scanned for write / reset / halt /
flash commands before OpenOCD starts), and calls the board a CUBUS when
1. the CPU2 flash contains the word `0xB0A2D003` (`g_boardId` of a CUBUS=T image), or
2. its 96-bit unique id is listed in `tools/flash/cubus_boards.txt` (for a board that still runs other firmware, e.g. the reference one:
   `python3 tools/flash/board_guard.py --register-cubus` with only that board connected).

On a CUBUS board only the CUBUS CPU2 image (`make flash sat CUBUS=T`) is allowed. A Nucleo satellite image, the ground-station image and the NuttX CPU1 app (`flash-m4`) are
refused: the guard prints `ERROR!! You made a mistake, dont flash nucleo in CUBUS`, exits 1 and `make` stops before any flash tool starts. If the board cannot be read
(no probe, no answer, readout protection) the flash is locked as well. There is no override switch; to flash a board that was registered by mistake, delete its line from the registry.
`python3 tools/flash/board_guard.py --status` only reports what is detected. Not covered: `make c2boot` (option bytes, same value the flight board already has) and tools that call
STM32_Programmer_CLI / OpenOCD directly. Tested without hardware by `tests/host/test_board_guard.py` (stub `openocd`; a real `make flash` of the Nucleo image, the GS image and `flash-m4`
against a stub CUBUS board stops with the error). The SWD read ran on a Nucleo (the GS, 2026-10-12: unique id and flash scan work, verdict "not CUBUS"); not yet run on the CUBUS board itself.

## Check (no hardware): `tests/host/test_cubus_pins.py`

Part of `make test`. It resolves the `SAT_CUBUS` conditionals of every satellite and common source and fails on any GPIO call or register access that is not on the allow list
(PA0, PC2..PC5, PA9 / PA10), builds a CUBUS image into a temporary directory and checks it: `g_boardId`, `Obc_Led` present, `Obc_Led` empty, no `BSP_RADIO_Init`
(the ST BSP switch code that drives PC3 / PC4 / PC5 on its own), no `led_init`, and the GPIOB_AFRL address (PB6) in no literal pool. With CUBUS off the same check reports 22 violations.

## Testbench with CUBUS (`make testbench`)

The testbench follows the profile in `.s2s2.config` (`cubus`, TUI Build options). With CUBUS on:

- **Links.** The second serial link ("Sat UART" in the top bar) is the satellite's debug UART (PA9 / PA10 on the ACCESS connector through a USB serial adapter), text only. The Nucleo ST-Link VCP
  and the `s2s2obc` frames do not exist on the flight board (CPU1 runs the OBC firmware).
- **Blocked tests** (they need the bench app on CPU1; they refuse to start with the reason): image, imageloop, sdrimage, digi, rate, cwtest, vcpfuzz, flashio, lfsproof, e2e.
  Available: the ground-station tests (link, cmdconf, nackmatrix, beacontime, winmap, soak), the SWD tests (fwcheck compares CPU2 only, bootcfg, membudget, profcap) and the profiler (no CPU1 ELF is assumed).
- **Build / flash page.** A "CUBUS board" checkbox (default = saved option) passes `CUBUS=TRUE|FALSE` to the build and the flash of the satellite; the confirmation dialog says which image it is.
- **DEBUG: section** (sidebar), two pages, nothing touches a board until you press a button:
  - **CUBUS debug** answers "why does the satellite not receive / hand off a command?" stage by stage. *Read the satellite now (SWD)* reads CPU2 over the board's ST-Link (memory only, no writes):
    image id (`g_boardId` against the profile), CPU2 alive, state machine and time since the last beacon (it is deaf during the CW beacon), RF switch and PA pins, the receive counters `g_satRx`
    (radio -> decode -> validate -> accepted -> written to the OBC ring), the OBC command ring (magic set by the M4? commands waiting?), and prints findings in plain words with the likely cause.
    *Live* repeats it every 3 s and compares the counters with the previous read. The *Receive test* sends one ground-station command every few seconds for a satellite cycle and reports the stage
    counters before / after, so a lost command is located (radio / decode / validation / hand-off) instead of "no ACK". The debug UART terminal, stage counts from the log lines, the pin-ownership
    check (`tests/host/test_cubus_pins.py`) and the list of blocked tests are on the same page.
  - **CUBUS pins** is the pin table of the schematic with the owner of each pin and the live state of the CPU2 pins (PA0, PC2..PC5, RF switch).
- **Firmware support.** The satellite counts every receive stage in `g_satRx` (`struct sat_rx_stats_s`, `satellite_main.c`, 88 bytes, in every build, about 350 bytes of flash); a command that cannot be written to the
  OBC ring is now counted and logged (`ring_fail`) instead of being lost silently. `tests/host/test_testbench_cubus.py` checks that the decoder in `tb/cubus.py` matches the C struct field by field.

## Decisions taken from the reference firmware (`GMSK_CW_OBC_COM_test`, M0+ and `apps/examples/OBC_main`)

| Question | What the reference does | CUBUS build |
|---|---|---|
| Who switches the 5 V bus rail (PA0)? | CPU2 owns it: `RBI_Init` low, GMSK high, low again after each downlink | same |
| Who feeds the watchdog U5 (PB8 = WD_WDI_OBC)? | nobody: no code in the M0+ image or in `OBC_main` touches PB8 or any watchdog | same: PB8 is never touched. The watchdog must be disabled on the board (EN / jumper JP2) or fed by other firmware |
| Gyro / magnetometer in the live HK packet | not delivered: the ring has only beacon 1 and beacon 2 (`Sring_buffer.h`); `OBC_main` builds its own HK records with fixed numbers | same ring, nothing added; the words are 0 (not measured), not the reference's fixed numbers |
| TCXO | Nucleo scheme: `SetTcxoMode` on DIO3, `TCXO_CTRL_1_7V`, `IS_TCXO_SUPPORTED = 1` | same |
| RF switch table | RX FE_CTRL1/2 = 1/0, TX = 0/1 | same |

Where the CUBUS build deliberately differs from the reference (safety, not behaviour): no PB6 UART, no LEDs on PB15/PB9/PB11, no invented telemetry values, 3.3 V PA off between transmissions.

## What only the real board can confirm

- RF switch table and output power with the CUBUS switch part (the schematic only names its pins VEN / VCTL).
- TCXO: the schematic powers it from COM_3V3 (PB0 unused); the DIO3 scheme only adds a start-up wait. Check the 32 MHz reference on the bench.
- Watchdog U5 state (populated / enabled) on the board.
- The CPU1 image on the flight board is the OBC firmware, flashed separately; `apps/examples/s2s2obc` is a Nucleo bench app and should not run there.
- The CUBUS image has not been run on the real board. It builds and links (satellite 50,336 B, 87.8 % of the 56 KB); the checks done are in `docs/CHANGES.md` section 26.

## SWD receive check

`tools/flash/swd_rx_check.sh [-l] [-s SERIAL] [-e satellite.elf] [-n 15 -i 2] [-x rx]` lists the ST-Links (USB registry only), lets you pick one, asks for confirmation and then reads the device id, PWR_CR4.C2BOOT, CPU2 clocks, radio BUSY, PA0 / PC2..PC5 mode and level, the RF switch decode (RX = PC4 1 / PC5 0) and, with an ELF, the firmware variables and the `g_satRx` counters (also works with the reference firmware's `app_state`, `rx_done_flag`, `last_rx_rssi`). Run it while the ground station transmits, after the CW beacon. Exit code 1 when a check fails.

Safety design (flight board): a bare MEM-AP target instead of `target/stm32wlx.cfg` (that file writes DBGMCU_CR and DBGMCU_APB1_FZR1 on attach, and the Cortex-M target type writes DHCSR / DEMCR); no reset, halt, resume, memory / register write or flash command; reads only an allow-list of registers (GPIO, RCC enable, PWR, FLASH_SRRVR, DBGMCU_IDCODE) plus RAM; the generated script is scanned for write / reset / halt / flash words before OpenOCD starts; 500 kHz. Attaching still powers up the chip's debug domain and uses the MEM-AP's own CSW / TAR registers, like any SWD tool. Checked against a mocked memory and a probe-less config parse only; not yet run on a board.

Note: the testbench profiler and the CUBUS debug snapshot use `stm32wlx.cfg` and so do write those DBGMCU registers on attach; the profiler's stack watermark also halts CPU2 for about 5 ms. Use the script above on the flight board instead.

## Bench findings 2026-10-11 (reference firmware on the CUBUS board, nothing of ours flashed)

Read-only: debug UART (FTDI, 115200, DTR/RTS low, never written) and `tools/flash/swd_rx_check.sh` (ST-Link V2 clone, serial reported as `3*`; the script selects such a probe by USB port on Linux, or as the only attached probe on macOS).

- The board runs the team's firmware (CW callsign `9NS2S2NEPAL`, 15 WPM, M4 OBC with a LittleFS daemon). Cycle about 99 s: CW about 49 s, then a 50 s listen window ("RX Listening armed on 437.375 MHz").
- During one whole listen window (14 SWD samples, 19:14:32 to 19:15:11) the RF switch was in the RX state every time: PC4 = 1, PC5 = 0, PC2 = 1 (3.3 V PA on), PC3 = 0, PA0 = 0. No OFF / TX gaps. The earlier all-zero samples were CW gaps (the reference code sets OFF in `CW_Prepare` / `CW_Carrier_Off` / `CW_Finish`).
- In the same window the UART showed no `[STEP 1: READ RAW]`, i.e. the radio completed no packet. It is not known whether the ground station was transmitting during that window (the GS `S` status read afterwards showed `Pkts TX 0`, but that GS may have been reset).
- `PWR_SR2` = `0x1FD` in every sample, `RFEOLF` (radio end-of-life flag) set; its meaning is not verified (see RM0453). Radio BUSY is not set (`RFBUSYS` and `RFBUSYMS` must both be 1 for busy).
- GS (second board): `TX Power +22 dBm (RFO_HP)`, and it detected the satellite's CW carrier at -95 dBm, so the downlink path works and the air path loss is about 110 dB. The uplink (GS +22 dBm) should therefore reach the satellite at about -87 dBm, well inside its input range.
- Open: preamble detect with `SyncWordLength = 0`, TCXO / clock, `AMP_VIN` in RX (the schematic shows the RF path radio -> PA1 (ADL5324, supply `AMP_VIN`) -> RF switch -> SMA; no separate bypass was found in the crop checked, which would also explain why the reference keeps the 3.3 V PA on in RX), and the RF path itself.
- Next test when the boards are back: `tools/flash/rx_window_test.py --sat-port .. --gs-port .. --go` (GS sends 3 non-command frames in one listen window while the satellite UART is logged; the satellite does not answer, so the 5 V PA stays off).
