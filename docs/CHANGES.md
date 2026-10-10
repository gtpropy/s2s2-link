# S2S2 COM subsystem: what changed, what it costs, how it was verified

> **Paths:** sections 1-15 use the layout before 2026-10-10 (`Satellite_M0+/Com_sat`, `Ground_Station`, `fec_asm_tests`, `host_tests`,
> `test_reports`); section 16 maps old to new. The per-file diffs against the reference (`docs/diffs`) were dropped with the old layout
> (the reference is `github.com/PrmBRana/dummy-camera-live` commit `0b807890`). The light FEC wrap of sections 7, 8 and 13-15 was removed
> in section 16; AUTH stays.

Baseline = `github.com/PrmBRana/dummy-camera-live` @ `0b807890` ("hk correct"), built unmodified with the same
ARM GNU Toolchain 15.3 (`-Os`). Full source diffs against it are in `docs/diffs/`:

| Tree | Files changed | Lines | Diff |
|---|---|---|---|
| `Satellite_M0+/Com_sat` (CPU2, satellite) | 26 | +2,379 / −294 | `docs/diffs/Satellite_M0__Com_sat.diff` |
| `Ground_Station` (CPU2, ground station) | 16 | +1,998 / −238 | `docs/diffs/Ground_Station.diff` |
| NuttX `.config` (CPU1) | – | see file | `docs/diffs/nuttx_config_changes.txt` |

Regenerate: `diff -ruN --strip-trailing-cr --exclude='*.o' --exclude='*.d' --exclude='*.elf' --exclude='*.bin' --exclude='*.map' <reference>/Satellite_M0+/Com_sat Satellite_M0+/Com_sat`.

## 1. Size and RAM, measured

| | Reference | Now | Change |
|---|---|---|---|
| `satellite.bin` | 52,264 B | **51,536 B** | −728 B (−1.4 %) |
| satellite static RAM (`.data`+`.bss`) | 14,072 B | **8,096 B** | −5,976 B (−42 %) |
| `gs_m0plus.bin` | 48,352 B | **48,648 B** | +296 B (+0.6 %) |
| GS static RAM | 25,696 B | **19,320 B** | −6,376 B (−25 %) |

"Now" already contains everything the reference does *not* have: the error-correcting wrap (~2.0 KB of assembly code
and tables), the keyed integrity tag, the CPU1 ring consumer, the Nucleo RF front end. Those features were paid for by
the optimizations below, so flash ends up level while static RAM drops by about 6 KB per image. A HAL → LL change
alone is only ~7 % of a 53 KB image; the large savings came from dead code and from RAM-hungry buffers.

CPU1 (NuttX): `nuttx.bin` 141,544 B, ends below the LittleFS store at `0x08024000` (page 72).

## 2. Optimizations (each verified on hardware by the end-to-end ping/image test)

| # | Change | Where | Effect |
|---|---|---|---|
| 1 | **Streaming G3RUH/NRZI bit writer.** The reference built the frame as a bit-per-byte array (one byte per bit, ~1.6 KB + more) and scrambled it in separate passes. Now one packed bit stream is written directly into the 200-byte radio buffer. Receive side uses a packed bit buffer too. Byte-identical on-air output. | `Mission/protocol.c` | −5 to −6 KB RAM per image |
| 2 | **`mini_printf.c`** replaces newlib `vsnprintf`/`snprintf` (exact libc formatting incl. width/precision/`%f` for the GS; checked against libc in `host_tests/`). | `Mission/mini_printf.c` | −1.2 KB flash (satellite) |
| 3 | **Sub-GHz SPI driver on LL**: one transaction routine replaces six copies of the HAL command/register/buffer code; no handle state machine, lock or callback tables. Same `HAL_SUBGHZ_*` names so the radio middleware is untouched. | `subghz/subghz.c` | −460 B |
| 4 | **RTC-backed `UTIL_TIMER` layer removed from the build.** `SystemApp_Init()` / `UTIL_TIMER_Init()` are never called, so the RTC clock was never enabled (RTC and `RCC_BDCR` read 0 on the running board) and every alarm start was a no-op. Replaced by stubs with identical behaviour; `make TIMER_IF_USE_RTC=1` restores the driver. | `core/timer_if.c`, `core/rtc.c`, Makefiles | −2.2 KB |
| 5 | **HAL core / GPIO / Cortex / PWR modules replaced** by `core/ll_port.c` (`LL_Init_CPU2`, GPIO output init, `GPIO_WritePin/ReadPin`, tick) and CMSIS `NVIC_*`. | `core/ll_port.c`, `Inc/ll_port.h` | −530 B |
| 6 | **SipHash-2-4 tag in hand-scheduled Thumb-1 assembly** (`seal_tag`), tested against the official vector + 664 random cases in a Cortex-M0 emulator. | `Mission/seal_core.S` | −904 B vs the C version |
| 7 | **Error-correcting wrap core in assembly** (`seal_encode/syndromes/decode`: Berlekamp–Massey, Chien, Forney, re-check; 1,168 B code + 832 B tables). 1,537-case stress test: no overruns, no false successes. | `Mission/seal_core.S`, `fec_asm_tests/` | corrects up to 16 byte errors per packet |
| 8 | **Faster PA bring-up/tear-down on a Nucleo**: the 100 ms 5 V-boost wait and 10/20 ms PA-off waits only matter on the custom board. | `Mission/config.h` (`SAT_*_MS`) | ~170 ms less per packet |

Not done (honest): the ST radio middleware (`radio_driver/`, ~8.7 KB) still carries unused Generic/LoRa paths; libc
`strtoul` is still linked; `LOG=0` (log strings out, −15 KB) is **broken** (see §6).

## 3. Functional changes to the reference firmware

| Change | Why | Where |
|---|---|---|
| **Light FEC wrap + integrity tag.** Payloads ≤107 B go out as `[counter(4)][len(1)][payload][pad to 112][tag(8)]` + 32 repair bytes = 152 B; larger payloads unchanged. Replay counter is RAM-only. | error correction + authentication of commands | `Mission/seal_*`, `protocol.c` hooks, `satellite_main.c` (+drop of invalid commands), `gs_app.c` (+unwrap) |
| **`FRONTEND=NUCLEO` (default) / `CUSTOM`.** The reference targets a custom board (RFO_LP PA, external 3.3 V/5 V PAs, `PC3` = 5 V PA enable). On a stock Nucleo that pattern routes the **HP** path while the **LP** PA was selected → no RF out. NUCLEO uses the GS's switch table (`PC3` enable, RX `C1=1 C2=0`, HP-TX `C1=0 C2=1`) and the RFO_HP PA. | root cause of "satellite never heard" | `Mission/config.h`, `target/radio_board_if.c`, `satellite_app.c` |
| **4 leading HDLC flags for wrapped frames** (was 12). The satellite RX has no sync word, starts at preamble detect and loses ~18 of its 200 bytes to preamble+sync; with 12 flags the closing flag/FCS fell off the end. | commands were silently dropped | `Mission/protocol.c` |
| **`SAT_ACK_GUARD_MS` (700 ms) and `SAT_TX_GAP_MS` (300 ms).** The GS prints a full report per frame and has one RX buffer; back-to-back packets were lost. | downlink reliability | `Mission/config.h`, `satellite_main.c`, `satellite_app.c` |
| **UART RX ring + pump** (`uart1_rx_pump`, called from `CPU2_Poll_IPC` and the TX wait loops). The console UART has a 1-byte register and the main loop polls every ~10 ms. | no lost console bytes | `uart/uart_debug.c`, `protocol/stm32wlxx_it.c` |
| **CPU2 no longer touches LPUART1 (the ST-Link VCP).** CPU1 owns it now. | CPU1 talks to the PC | `uart/uart_debug.c` |
| **Ring consumer for the image path** (`Obc_Ring_Service`): takes chunks CPU1 put in the shared flash ring, transmits them as 0xCAFE camera packets, reports done. | CPU1 → ring → CPU2 → RF | `satellite_main.c`, `satellite_app.c` (`Satellite_Send_Camera_Chunk`), `ipc/obc_shared.h` |
| **Replay-floor reset** `Seal_ResetRxFloor()`. | GS reboot restarts its counter at 1 | `Mission/seal_wrap.c` |

## 4. The image path (CPU1 flash → ring buffer → CPU2 → RF)

```
PC --ST-Link VCP (/dev/cu.usbmodem*)--> CPU1 (M4, NuttX app s2s2obc)
    stores the file on LittleFS in CPU1's internal flash  (pages 72..99 = 0x08024000..0x08031FFF, mounted at /obc)
    reads a chunk back out of that file --> shared flash ring (SRAM2 0x20008840)
CPU2 (M0+) takes the chunk from the ring --> GMSK 4800 bps, 435.000 MHz --> ground station --> UART JSON --> PC
```

* **CPU1 app** `apps/examples/s2s2obc/s2s2obc_main.c` (init task; NSH removed). Mounts LittleFS on an `mtd_partition()` of
  the progmem MTD (`autoformat`), parses the frames below, writes each chunk with `write` + `fsync` + read-back compare,
  reads chunks for `SENDF`/`READF`, drives the LEDs. NuttX config: `FS_LITTLEFS`, `SERIAL_TERMIOS`, LittleFS
  read/prog/cache factors = 1 (default 4× needs 8 KB buffers and failed with `ENOMEM`), init stack 5 KB.
* **Memory map** (nothing overlaps): CPU1 image `0x08000000–0x08023FFF`, LittleFS store pages 72–99
  `0x08024000–0x08031FFF`, CPU2 firmware `0x08032000`. SRAM2: flash ring `0x20008840`, OBC status block
  `0x20008F00` (`ipc/obc_shared.h`: heartbeat, session, tx request/done, reset), CPU2 RAM from `0x20009000`.
* **Frames on the VCP** (little endian). PC→CPU1: `C5×8, A7, cmd, seq(2), total(2), len, data, crc16(2)`
  (CRC-16/CCITT-FALSE). CPU1→PC: `A5 5A cmd seq(2) status 0D 0A`; `READF`/`INFO` also send `A5 5C seq(2) len data crc16 0D 0A`.
  Commands: `02` STATUS (status 0 = CPU1 up *and* CPU2's heartbeat advancing), `03` RESET, `04` new file, `05` STORE chunk,
  `06` SENDF (file → ring → CPU2 → RF, answered after the RF transmission), `07` READF, `08` SETNAME, `09` INFO.
* **LEDs** (PB15 blue, PB9 green, PB11 red, active high): **blue** = writing the flash, **green** = reading it,
  **yellow** (green+red) = CPU2 transmitting.

## 5. Tooling

| What | Where |
|---|---|
| Root `Makefile` + Textual TUI (build, memory stats, probe pick, flash); `make testbench` | `Makefile`, `tools/s2s2_tui.py`, `tools/s2s2_core.py` |
| Per-tree Makefile options: `AUTH`, `DEBUG`, `SEAL_TEST`, `FRONTEND`, `LOG`, `TIMER_IF_USE_RTC`, `SERIAL=` | `Satellite_M0+/Com_sat/Makefile`, `Ground_Station/Makefile` |
| **Test bench GUI** (`http://127.0.0.1:8765`): link test, image test (store → read back → transmit, per-stage progress, LED indicator), SDR+Direwolf test, proof reports | `tools/testbench/server.py`, `index.html` |
| Independent proof checkers (do not use the bench code): rebuild from the raw GS log + hashes; raw-flash dump over SWD | `tools/testbench/verify_report.py`, `flash_proof.py` |
| Bench regression ping | `tools/e2e_ping.py` |
| FEC/tag emulator tests (Unicorn), host round-trip tests | `fec_asm_tests/`, `host_tests/` |

## 6. Proof of the image path (APN logo, 2026-10-08, run from the GUI)

`test_reports/20261008_172200/` (report.html, report.json, source.jpg, received.jpg, gs_uart.log, sat_vcp.log, led_trace.json):

| Check | Result |
|---|---|
| File | `antarikshya_logo.png` (94,764 B) compressed to JPEG 441×423 q90 = 23,406 B, 183 chunks |
| Stored by CPU1 | `/obc/antarikshya_logo.jpg 23406 B on LittleFS (CPU1 flash pages 72..99)` (CPU1's own `INFO` answer) |
| Read back from that file | 183/183 chunks identical; SHA-256 `7e992663a2ac35b8…` = source |
| Over the air | GS printed 183/183 chunks, 0 re-sends, received SHA-256 = source, byte-identical file |
| LED pins sampled over SWD (GPIOB ODR, independent of firmware) | blue 30.6 s (write), green 141.7 s (read), yellow 144.8 s (CPU2 transmitting); expected colour present 83 % / 99.9 % / 99.6 % of the three phases |
| `verify_report.py` (separate code, rebuilds from the raw `gs_uart.log`) | VERIFIED |
| `flash_proof.py` (raw SWD dump of flash pages 72–99) | 28/28 pages programmed, 180/183 chunks found verbatim (3 straddle LittleFS block headers) |
| Rate | 324.8 s total, ~1.5 s per chunk (CPU1 file read + CPU2 transmit + GS pacing), RSSI −90.5 dBm |

Not usable for verification: the reference GUI's own chunk parser (`Ground_Station/gs_communicator.py`) ingests the GS's
camera JSON inside its GUI function (not importable), and its importable parser needs raw frame dumps the GS does not
print for camera chunks.

## 7. Known problems (not fixed)

1. **`make LOG=0` corrupts the satellite's downlink frames** (log strings out = −15 KB). Bisected to `satellite_app.c`
   prints; cause not understood. `LOG=1` is the default and works. Suspects (unconfirmed): log calls acted as timing
   delays before the PA keys; the quiet macro drops its arguments (found + fixed: `Seal_SelfTest()` was called inside a
   print and never ran with `LOG=0`). Not re-tested on air after that fix.
2. **Replay counters are RAM-only.** If the GS reboots its counter restarts at 1 and the satellite rejects everything
   until its floor is cleared (`RESET` frame).
3. **SDR + Direwolf test never passed**: the RTL-SDR was not detected during the sessions.
4. The satellite is deaf to RF while keying its CW beacon (~45 s of every ~95 s); the bench waits for it.
5. Satellite flash is ~90 % full; the LittleFS store leaves ~5.9 KB between the NuttX image and page 72.

## 8. Comment pass and bench fix (2026-10-08, later)

- **Comments added** at every changed place that lacked one, marked `CHANGED:` (reference behaviour replaced) or `ADDED:`
  (new code): `satellite_app.c`, `satellite_main.c`, `stm32wlxx_it.c`, `radio_board_if.c`, `stm32wlxx_nucleo_radio.c`,
  `uart_debug.{c,h}`, `obc_led.c`, `subghz.c` (both trees), `gs_app.c`, the CPU1 app `s2s2obc_main.c` and the bench server.
  Comments only: both CPU2 images are byte-identical in size (satellite 51,536 B, GS 48,648 B). Added comment lines vs the
  reference rose from 254 to 540 (satellite) and 181 to 317 (GS); `docs/diffs/` regenerated.
- **`LOG=0` explained** in `uart/uart_debug.h`; one real `LOG=0` bug fixed (`Seal_SelfTest()` was called inside a
  `uart1_printf()` argument list and vanished when the macro expands to nothing). The frame corruption itself is still open.
- **Bench "stopped working" = the removed "direct" mode.** The page still offered "direct (no storage)", which sends the old
  `CHUNK` frame; CPU1 has no such command, so it failed after 2 s with 0 chunks. The button is removed and the server refuses
  any mode except `store`. The RESET wait is now 6 s (CPU1 itself waits up to 3 s for CPU2, so the bench gave up at the same
  instant and logged a false "RESET not acknowledged").
- **Digipeater:** not in the current firmware. The earlier `JC2/` firmware had a bidirectional digipeater with `DIGI ON/OFF`;
  it was archived (`../s2s2-link_removed_2026-10-07.tar.gz`) when the CPU2 code was reset to the reference. Only the unused
  mission id `SAT_MISSION_DIGIPEATER` (0x09) remains in `satellite_app.h`.

## 9. The periodic 40-50 s transmit pauses (fixed, 2026-10-08)

Cause: `Obc_Ring_Service()` (satellite_main.c) stamped the "keep CW off" timer with `HAL_GetTick() | 1U`. On an even
tick that stamp is 1 ms in the future, so `now - s_last_cmd_time_ms` underflows, the 60 s hold looks expired and, once the
50 s listen window ran out, the satellite dropped into its 42 s Morse CW beacon in the middle of the transfer (~50 % per
loop pass, so effectively always). Seen over SWD: `session=1`, hold age 242 ms, yet `app_state` = CW and the CPU2 heartbeat frozen.
Before: 3 pauses (41.6 s, 51.6 s, 41.6 s), run 326.7 s. After: max gap between chunks 0.85 s, run 192.6 s (report `20261008_191541`,
VERIFIED). Where the remaining time goes (192.6 s): store to flash 37 s, read back 8 s, 183 chunks x 0.79 s = 145 s.
Per chunk 0.79 s = 0.36 s on air (200 B at 4800 bit/s + preamble) + 0.30 s `SAT_TX_GAP_MS` + ~0.13 s hand-over.

## 10. CW beacon optimisation (2026-10-08)

| | Before | After |
|---|---|---|
| Morse speed | 15 WPM (unit 80 ms) | 20 WPM (unit 60 ms) |
| Tuning carrier / pause / gap callsign->payload | 2000 / 1000 / 1000 ms | 500 / 300 / 700 ms |
| Beacon length on air (CW1 / CW2) | ~50 s / ~41 s | ~36 s / ~29 s (measured: tuning carrier at 388.1 s, text decoded at 424.5 s = 36.4 s) |
| SPI transactions per dit/dah | 6 (PA config, TX params, OCP, drive, switch, CW) | 1 (CW only; PA set up once in `Satellite_CW_Prepare`) |
| Beacon vs. the PC | keys to the end (up to ~48 s wait before the bench can talk) | stops between characters as soon as CPU1 sees any VCP frame; bench start-up wait 0.2 s |

- Satellite: `satellite_app.h` (timing defaults), `satellite_app.c` (`s_cw_cfg_valid`, lean `Satellite_CW_CarrierOn`, `cw_yield_to_obc`
  checked in `Satellite_CW_SendChar` and around the tuning tone / gap). Image +100 B (51,644 B, 90.1 % of the 56 KB).
- Ground station: `cw_rx.h` `CW_UNIT_DEFAULT_MS` 80 -> 60; every decoder threshold scales with it. The GS decodes the new beacon
  (`9NS2S2NEPAL 4.00V 4.00V 25.0C 25.0C 25.0C`). The satellite and GS units MUST match.
- CPU1 (`s2s2obc_main.c`): any frame from the PC opens the CPU2 session (was only STORE/SENDF/NEWFILE). `nuttx.bin` 141,628 B (limit 147,456 B).
- Spec impact: the beacon is 20 WPM with a shorter tuning tone. If the mission packet-format sheet fixes 15 WPM / 2 s, set the three
  `SAT_CFG_DEFAULT_CW_*` values back and `CW_UNIT_DEFAULT_MS` to 80.
- Verified: report `20261008_194010` (183/183, VERIFIED); STATUS answered at once and RESET acknowledged (it timed out in the CW before).

## 11. Beacon every 90 s even during a transfer; bench and store fixes (2026-10-09)

- **CW beacon is no longer blocked by an image transfer** (`satellite_main.c`). It starts every `SAT_BEACON_PERIOD_MS` = 90 s,
  measured from the START of the previous beacon (was: 50 s of listening after the beacon ended, so 79-86 s, and held off for
  the whole transfer). When it is due the chunk in flight finishes, the beacon is sent (29-37 s), the transfer resumes
  (CPU1 waits up to 90 s for `tx_done_seq`). `Obc_Ring_Service` no longer refreshes `s_last_cmd_time_ms`; only a GS command
  session (60 s hold) still defers the beacon. The "yield to the PC" early-stop from section 10 was removed again (it
  contradicted this). Measured on air: pauses of 29.9 s, 37.3 s, 29.9 s inside one 183-chunk transfer, beacon starts 90.8 s /
  91.5 s / 92.1 s apart (GS detection jitter), CPU2 SWD sampling: beacon at tick 112,350 for a target of 22,333 + 90,000.
- **CPU1 `NEWFILE` now empties the whole store** (`clear_store()`): files left under other names (earlier uploads) filled the
  56 KB store and a 23 KB image failed with a flash error at chunk 176. New `nuttx.bin` 141,732 B.
- **Bench**: fit buttons re-process the loaded file instead of swapping in the dropdown sample; default fit = compress to the
  CPU1 flash; a file that is too big is compressed automatically at test time; EXIF rotation, truncated JPEGs, HEIC
  (`pillow-heif`) handled; `/api/image/refit`; the Nucleo's USB serial port is re-opened automatically if it drops out mid-test.
- **Hardware note**: the Nucleo now connected is a different board (ST-Link `002D001B...`, was `002E0035...`). Its old firmware
  was overwritten with ours (backups of CPU1/CPU2 flash and of the old LittleFS pages 72-99 are in the session scratch
  folder, not in the repo). Its ST-Link USB serial port dropped out twice mid-transfer (not the MCU: CPU2 kept running).
  Cause not found; suspects are USB power/cable or RF pick-up.
- **Throughput (why 4800 bit/s shows as 650-1300)**: 4800 raw -> 4360 after preamble/sync -> 2790 because only 128 of the
  200 bytes in each packet are file data -> ~1300 with the 0.42 s idle per packet (300 ms `SAT_TX_GAP_MS` + ~120 ms
  hand-over) -> 654 for the whole 286 s test (flash write 37 s, read-back 8 s, three beacons ~97 s).
- Verified: report `20261009_173521`: 183/183, 0 retransmits, source = CPU1 flash = air SHA-256, `verify_report.py` VERIFIED.

## 12. Raw air-rate test (2026-10-09)

New end-to-end test `Raw rate test` (bench page, `RATE` command on CPU1, `Satellite_Rate_Test()` on CPU2, `RATE ON / RATE? / RATE OFF`
on the GS). The satellite sends N camera-format packets with a chosen pause between them, PA set up once, next packet built
while the previous one is on air; no flash, ring buffer, beacon or per-packet PA sequencing. The GS goes silent (`g_uart_quiet`)
and only counts. Result (100 packets per setting, report `20261009_175159`, RSSI -94 dBm, GS received 100/100 every time):

| gap | ms / packet | air rate (200 B) | payload rate (128 B) |
|---|---|---|---|
| 300 ms (what image transfers used) | 671.0 | 2384 bit/s | 1526 bit/s |
| 100 ms | 473.0 | 3382 bit/s | 2164 bit/s |
| 50 ms | 423.5 | 3778 bit/s | 2417 bit/s |
| 0 ms (back to back) | 374.1 | 4276 bit/s | 2737 bit/s |

Ideal packet time at 4800 bit/s is 366.7 ms (128 preamble + 32 sync + 1600 packet bits); measured 374.1 ms = +7.4 ms of SPI/IRQ latency.
The 300 ms gap in image transfers is therefore not needed once the GS stops printing a full report per frame. Added:
`obc_shared_s` rate fields, CPU1 `CMD_RATE`, GS `RATE` commands and `g_uart_quiet`, bench `test_rate`. Satellite image 52,108 B,
GS 49,248 B, nuttx.bin 141,852 B. A rate test blocks the CPU2 loop (and so the 90 s beacon) for up to N x (0.37 s + gap).

## 13. Packet format, FEC=T|F, SDR + Direwolf proof (2026-10-10)

- **Packet Format sheets applied** (kept the FEC wrap and AUTH): callsigns satellite `9N2SJ-1` -> `GROUND` (GS: `GROUND` -> `9N2SJ`);
  camera fragment is now the sheet's type `0x08` packet (image ID, fragment index, total, image type, AI score, 128 data bytes,
  CRC-16/X.25; no length field, last fragment zero padded); CW beacon = `S2S2 9N2SJ` + the 8 bytes of CW BEACON 1 / 2 as hex digits
  (grouped `XXXX XXXX XXXX XXXX`; fields the M4 does not deliver are sent as 0); 20 leading HDLC flags on every unwrapped packet.
  Not done: Beacon 1/2 and HK telemetry AX.25 packets of the sheet (0xB1 / 0xB2), image type / AI score values.
- **`FEC=T|F` build option** (satellite and GS Makefiles, `tools/s2s2_tui.py`): `FEC=T` (default) = light FEC wrap on short packets; `FEC=F`
  = every packet is the plain AX.25 information field of the sheets (HELLO, ACK, beacons, commands unwrapped), the wrap sources are not
  linked (satellite 52,492 -> 49,092 B, GS 46,448 -> 43,144 B), AUTH is forced off (its tag lives inside the wrapped block), 20 leading
  flags. Both ends must be built the same way (`make FEC=F flash-sat` / `flash-gs`). Verified on air with FEC=F: Direwolf decodes
  `9N2SJ-1>GROUND:HELLO` as a 21-byte frame (`docs/proof/direwolf_hello_fecoff_terminal.txt`).
- **Proof without the GS**: bench test "Image via SDR + Direwolf": the Nucleo sent the 23,406 B APN logo, RTL-SDR + stock Direwolf decoded
  183/183 fragments (valid type 0x08, CRC-16/X.25), `9N2SJ-1 -> GROUND-0`, ctl 0x03, PID 0xF0, rebuilt picture byte-identical
  (`test_reports/20261010_133730`). Raw terminal captures: `docs/proof/direwolf_*_raw_terminal.txt`.
- **Bench fixes**: a status refresh ran `st-info --probe` (resets the board) and `rtl_test -t`; both are replaced by reads of the USB
  registry. This was the reset loop (CPU1 and CPU2 clocks restarting every ~4.5 s) behind many of the earlier USB drop-outs and "CPU2 hung"
  symptoms. New bench tests: raw rate, CW HELLO / GMSK HELLO, camera fragments on repeat, SDR image proof with live Direwolf frame log;
  the Nucleo's VCP is opened with DTR/RTS low and reconnects at most every 3 s; `rtl_fm` is stopped with SIGINT first.


## 14. ACK / NACK and command opcodes per the Packet Format sheets (2026-10-10)

- **ACK / NACK (Sheet 2)**, 3-byte information field `[type][status/error][mission id]`: ACK accepted `AC 00 <mission>`, ACK completed
  `AC 01 <mission>` (was `EE 01`; `0xEE` is now only the NACK/error type, as in the sheet's legend), NACK/error `EE <err> <mission>` with
  the sheet's codes (0x02 packet, 0x03 CRC, 0x04 incomplete, 0x05 data missing, 0x06 data unavailable, 0x07 busy, 0x08 mission inactive,
  0x09 invalid command, 0x0A memory/file, 0x0B transmission failed); the old NACK byte `0xFF` is gone (satellite and GS).
  Note: Sheet 2's example row "ACK command completed" literally shows `EE 00 05`, which contradicts its own legend (ACK = 0xAC, status
  0x01 = completed); the legend was followed.
  Sent now: invalid/unknown opcode `EE 09 FF`; 0x53 frame that is not 13 bytes `EE 04 FF`; downlink commands whose data is not there
  (empty flash ring after the M4 timeout) `EE 05 <mission>` instead of silently sending nothing. Not generated: 0x02/0x03 (a frame with a
  bad FCS is dropped before it is known to be ours), 0x07 (RX is off while the satellite answers), 0x06, 0x08, 0x0A, 0x0B.
- **Command opcodes (Sheet 3)** - the satellite now serves exactly these 13-byte commands (SAT-ID 53, MCU ID, opcode B1-B3, DATA_ID 2,
  ADDRESS 4, NO OF PACKETS 2): `53 04 CC 5E BD` run camera mission, `53 01 1D D2 F5` camera download, `53 01 1D D1 F2` HEALTH SAT
  (sat_health), `53 01 1D D1 F8` housekeeping download, `53 01 DE EF AA` antenna deployment (ACK, forwarded to the M4, ACK).
  Removed (not in the sheet): the loose data-id / MCU-id matches, PING (`1D D1 F5`), dummy camera (`CC 5E BE`), ADCS / EPDM, the 8-byte
  command form and the 1-byte burst request; all of them now get NACK `EE 09 FF`. GS: `PING` now sends HEALTH SAT (ACK, HK packet,
  ACK), `FLASH`/`D` uses `1D D1 F8`, `HK2` is the housekeeping download, `DUMMY` is the sheet's run-camera command, new `ANT`.
- **CW (Sheet 4)**: already the sheet's format (`S2S2 9N2SJ` + 8 bytes of CW BEACON 1 / 2, system status 11 normal, mission status, time
  after reset, solar cell bits); nothing changed. The BIRDS-4 timing slide (35 s beacon, 80 s off) is for three satellites sharing a
  frequency; ours keeps one beacon every 90 s.
- Not done: the 2-byte header of the sheet's Data/Telemetry packet around the 128-byte HK data, the AX.25 Beacon 1/2 packets (excluded by
  request). The old GS firmware on the board is not reflashed (GS ST-Link not connected).

## 15. AUTH independent of FEC (2026-10-10)

- `FEC` and `AUTH` are now separate build options (satellite + GS Makefiles, TUI). All four combinations build:
  FEC=T AUTH=T = 152-byte block with counter + tag (as before); FEC=T AUTH=F = block with repair bytes only;
  **FEC=F AUTH=T = plain info field + 12-byte trailer `[counter(4, big endian)][tag(8)]`** (tag over payload + counter, same keyed digest
  and replay counter as the block form; the sheet layout stays in front of the trailer; packets > 107 B stay unauthenticated as before);
  FEC=F AUTH=F = nothing wrapped. Both ends must use the same FEC and AUTH settings.
- `seal_core.S` now assembles the FEC routines only with `SEAL_WRAP_ENABLE=1`; with FEC=F the tag function alone is linked (no tables).
  Satellite image text: FEC=T/AUTH=T 48,848 B, T/F 48,116, F/T 46,684, F/F 45,404.
- Receive: `SEAL_LEN_MAY_BE_WRAPPED(n)` (seal_wrap.h) decides whether to try `Protocol_DecodePayload`; with AUTH on the satellite still drops every
  command that is not authentic.
- Tests: `host_tests/rt_auth.c` (create -> extract -> verify -> unwrap; flipping any single bit of the info field is rejected; replay and a
  plain short packet are rejected; sheet prefix intact) and `rt_wrap.c` pass; on chip (Nucleo 002D, FEC=F AUTH=T) `g_sealSelfTest` = `0xA5A50001`
  (official tag vector, round trip, tamper and replay rejection). Not tested on air: the GS ST-Link is not connected.

## 16. FEC removed, generated config + secrets, new layout (2026-10-10)

- **FEC removed completely** (assembly encoder/decoder, tables, `FEC=` option, emulator tests, 152-byte block). AUTH stays and no longer depends
  on anything: payloads <= 107 bytes get a 12-byte trailer `[counter(4)][tag(8)]` behind the plain information field; every packet uses 20
  leading flags. Reason: the wrap could not cover the camera fragments (138 + 32 repair bytes does not fit the 200-byte radio packet),
  protected against the less likely failure (bit errors in a packet that was caught, not a missed sync) and had no measured benefit.
  `SEAL_TEST` is gone too (the boot self-test is always the full one). Satellite image text with AUTH: 46,700 B (was 48,848 B with FEC+AUTH).
- **The TUI is the configuration** (`make`, like NuttX menuconfig): `.s2s2.config` holds targets, AUTH, DEBUG, LOG, FRONTEND, RTC timer,
  project name, call signs + SSIDs, downlink / uplink Hz, CW dit ms, beacon period and the ST-Links. `make configure` (or any change in the TUI)
  generates `generated/s2s2_config.h` (the `S2S2_*` macros used by both images instead of the values that were hard-coded in six places),
  `generated/s2s2.mk` (build options) and `generated/secrets.h`. The per-image Makefiles regenerate on every run, so a plain `make` there works.
- **`generated/secrets.h`**: random 16-byte AUTH key, created automatically when missing, replaced by `make newkey` / the TUI button (asks twice).
  Git-ignored. Replaces the key that was hard-coded in `seal_wrap.c`. The boot messages of both images print `key <fingerprint>`.
- **New layout** (see README.md): `Satellite_M0+/Com_sat` -> `cpu2/satellite`, `Ground_Station` -> `cpu2/ground_station`, files identical in both
  images (261 of them after dropping dead sources: 81 unused HAL / SX126x / test `.c` files) -> `cpu2/common/{protocol,system,drivers,ipc,vendor}`;
  per image `app/ mission/ board/ system/ radio/ uart/ ipc/ boot/`. One shared `cpu2/common/cpu2.mk` replaces two near-identical 19 KB Makefiles;
  objects and outputs go to `cpu2/<image>/build/` (the GS output is now `ground_station.bin`). `fec_asm_tests` -> `tests/tag_asm`, `host_tests` ->
  `tests/host`, `test_reports` -> `docs/reports`, `tools/` split into `config/ testbench/ ground_software/ flash/`, `SLIDES_BRIEF.md`, the
  Packet Format sheet and the RF reports -> `docs/`. Removed (archived outside the repo): `V1.4`, `old_code_reference`, `old_reference`, `shareRAM`,
  stray images / logs / spreadsheets in `Ground_Station`.
- Verified: both images build for AUTH=TRUE and FALSE (satellite 46,700 / 45,392 B, GS 44,388 / 43,644 B); host tests (`make test`) and the
  tag-function emulator test pass; the satellite was flashed from the new tree and its boot self-test word reads `0xA5A50001`.
  Not re-tested on air: the GS ST-Link is not connected, so the GS image was not flashed.

## 17. Digipeater and AFSK1200 modem (2026-10-10)

- **Digipeater engine** `cpu2/common/digipeater` (pure C, no hardware): New-N path rules (own call, alias, `WIDE2-N` with trace), H-bit loop check, duplicate
  cache (source + destination + info, 30 s), per-station interval, token bucket, size check, reason codes and counters. Researched against Dire Wolf's
  `digipeat_match` / `dedupe`, aprsdigi, the AX.25 digipeater description, the PACSAT discussion and APRS practice; full write-up in `docs/DIGIPEATER.md`.
- **Satellite integration**: a decoded frame with a digipeater path and a good FCS goes to the engine instead of the command logic; a repeat is re-framed with
  `Protocol_CreateRawPacket()` (new; `Protocol_CreatePacket` was split into build + `Protocol_EncodeClean`, binary unchanged) and sent on 435.000 MHz after a 300 ms
  guard. `DIGI=TRUE|FALSE` and the limits are TUI settings (`S2S2_DIGI_*` in `generated/s2s2_config.h`). Cost: +5.1 KB, satellite 51,888 B = 90.5 % of the 56 KB.
  GS console: `DIGI SRC>DEST,VIA:text` sends a test frame (`GS_Send_Raw_Frame`, `GS_Transmit_Frame` split out of `GS_Send_Burst_Packet`).
- **AFSK1200 modem** `cpu2/common/modem`: the receiver chain of the supplied paper (FIR, decimate x4, I/Q correlators, discriminator, PLL, DCD, majority vote,
  NRZI, HDLC, CRC residue) and a DDS transmitter, integer only. Not part of the CPU2 images (no ADC / DAC / VHF front end on the Nucleo); it is the software modem for a
  1200 baud VHF digipeater and is tested on the host. Corrections to the paper: the correlator window must be one bit (8 samples), not 32 (0 of 29 clean frames
  decoded with 32), and the CRC residue is 0xF0B8.
- **Tests** (`make test`): `test_digipeater` (4,067 checks), `test_afsk` (loopback, SNR sweep, clock error, noise), `afsk_direwolf.sh` (our modem against Dire Wolf both ways),
  `digi_chain.sh` (sender packet -> satellite framing -> engine -> repeat -> 4800 bit/s G3RUH audio -> Dire Wolf decodes the repeats). On the chip: `g_digiSelfTest` = `0xA5A50002`.
- **Not done**: no over-the-air digipeat yet (the GS image with the `DIGI` command is built but not flashed: its ST-Link is not connected).

## 18. Store-and-forward digipeater with hop budget (2026-10-10)

- **Store and forward** (`cpu2/common/digipeater/saf.{h,c}`): every frame the digipeater accepts is kept (12 messages), sent again later (`max_forwards` 3, `forward_interval` 15 min, `ttl` 6 h;
  all TUI settings) with hop-budget priority; the hop budget is the `WIDEn-N` of the stored path (`Digi_HopsLeft`); hopping across nodes is bounded by that budget and the loop check
  (`tests/host/test_saf.c`, three-node chain). Not the other meaning of hopping (radio frequency hopping, LR-FHSS): not implemented.
- **Flash**: CPU1 keeps a journal `/obc/saf.jrn` on its LittleFS (`apps/examples/s2s2obc/s2s2obc_saf.c`), CPU2 talks to it through a SRAM2 mailbox (`cpu2/satellite/ipc/saf_shared.h`,
  `0x20008000`, one writer per field). Records are CRC-protected and appended with `fsync`; compaction at boot and past 16 KB; the image-store clean-up keeps `saf.*`.
  `nuttx.bin` 144,204 of 147,456 bytes.
- **Test path**: VCP commands `0x0C` (status / list / clear) and `0x0D` (inject a frame as if received) and `tools/testbench/saf_tool.py`.
- **Satellite image** 54,580 B = 95.2 % of 56 KB (+2.7 KB for store and forward, 2.7 KB free). `DIGI=FALSE` removes all of it.
- **Verified**: host tests (`make test`: 53 store-and-forward checks), the digipeater and stored messages on the air through the SDR + Dire Wolf, journal survives a full reflash and is
  restored into CPU2, stored messages are sent again (`docs/proof/saf_digipeater_direwolf.txt`). Not verified: reception of a frame over the air by the satellite (GS not flashed).

## 19. AFSK1200 modem removed, GMSK only (2026-10-10)

`cpu2/common/modem` (AFSK1200 software modem after the "AFSK APRS Digipeater" paper), `test_afsk`, `afsk_tool` and `afsk_direwolf.sh` are gone (archive `../s2s2-link_removed_afsk_2026-10-10.tar.gz`).
The digipeater runs on the satellite's GMSK 4800 bit/s G3RUH path only; `docs/DIGIPEATER.md` section 3 now describes that. The chain test (`digi_chain.sh`) still decodes the repeats with Dire Wolf at 4800 bit/s G3RUH.

## 20. Store statistics appended to digipeated messages (test option, 2026-10-10)

`S2S2_SAF_APPEND_STATS` (TUI "STATS", default off): the satellite appends ` | MEMORY OBJECTS: #n id len hops_left forwarded ttl_left | STORED n msg, N B` to the text of the frame it repeats
right away (`append_stats()` in `satellite_digi.c`; the stored copy is unchanged). `saf_tool.py inject` prints the stored objects after a repeat. Checked by reading the transmit buffer over SWD; not
decoded over the air this time (the RTL-SDR was not enumerated).

## 21. Digipeater test in the test bench (2026-10-10)

`tools/testbench/server.py`: test `digi` (8 injected frames with expected decisions, journal listing, optional RTL-SDR + Direwolf over-the-air check), `/api/digi/inject|store|clear`;
`index.html`: section "Digipeater + store and forward" with Run / Inject / Show stored / Clear. Needs only the Nucleo VCP. Run on the hardware: 8/8 as expected, 3 messages / 153 B stored.
Test frames use `>` status text and destination `APZS2S` (no Dire Wolf APRS warnings).

## 22. NuttX-style menuconfig, smaller images (2026-10-10)

- **menuconfig** (`tools/config/s2s2_tui.py`, menu tree in `s2s2_core.MENUS`): a real menu like NuttX's (`--->` submenus, `[*]` / `(value)` items, help line, `?` full help, Space / y / n,
  value dialog with validation, choice lists, options hidden when their parent is off, probe picker, build / flash / clean / memory as menu actions). Every change is saved to `.s2s2.config` and
  `generated/` is rewritten at once. The earlier version was a cramped form of widgets; it also overrode Textual's own `App.run_action`, which crashes on key bindings with
  `run_action() takes 2 positional arguments but 3 were given` (the "make tui errors" report): renamed. `make` now also checks the terminal size and the Textual import, and writes
  any other failure to `.s2s2_tui_error.log` with a one-line reason instead of a raw traceback.
- **Smaller images** (same behaviour): satellite 53,676 -> 50,876 B (88.7 % of 56 KB), ground station 45,628 -> 42,176 B.
  - ST's advanced-trace layer formatted every middleware log line and handed it to a driver that discards it: replaced by a stub (`common/system/trace_stub.c`), -2.1 KB;
  - CRC-16 table 256 -> 16 entries (nibble-wise, same result, checked on 20,000 random buffers): -480 B;
  - the SX126x channel conversion `freq * 2^25 / 32 MHz` and the timer stubs in 32-bit math (same result, checked on 200,000 frequencies): removes the 64-bit division from libgcc.
  - Tried and rejected: `-flto` (the startup code needs `HardFault_Handler_C` kept), `-fno-jump-tables` and `-fmerge-all-constants` (bigger).
- Checked: `make test` (framing, CRC, digipeater, store and forward, Dire Wolf chain), on-chip rule self-test `0xA5A50002`, bench digipeater test 8/8 on the flashed image.

## 23. CW HELLO test without the leading carrier (2026-10-10)

The CW HELLO test (bench "Send HELLO continuously") used to key a 1 s steady carrier in front of every HELLO. A CW decoder (CwGet) reads a long key-down as a dash, the letter T, so it printed
`THELLO`. Default now: HELLO only (`cw_test_mode` 0), about 6 s per round. The carrier is still available as a separate bench button / `cw_test_mode` 2 for a spectrum analyzer. The beacon's own 500 ms tuning
carrier before `S2S2 9N2SJ ...` is unchanged (our ground station skips it; third-party decoders may show a leading T or E). Satellite and CPU1 reflashed; the bench server has to be restarted to get the new button.
