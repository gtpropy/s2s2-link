# S2S2 COM subsystem: what changed, what it costs, how it was verified

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

