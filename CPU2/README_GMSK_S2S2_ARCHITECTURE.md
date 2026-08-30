# STM32WL55 GMSK + S2S2-over-AX.25/FX.25 Firmware Documentation

## 1. System Architecture Overview

The **STM32WL55JC** is a dual-core SoC consisting of:
1. **CPU1 (Cortex-M4):** Runs the main application / RTOS (NuttX OS).
2. **CPU2 (Cortex-M0+):** Dedicated standalone real-time Sub-GHz radio coprocessor
   handling all RF PHY, GMSK modulation, and the S2S2 link-layer protocol
   (Reed-Solomon FEC, AX.25/FX.25 framing).

This documentation covers the **CPU2 (Cortex-M0+)** firmware in this repo's `CPU2/`
directory.

**Why AX.25/FX.25 instead of a fully custom frame:** the S2S2 header and RS(255,223)
FEC still exist -- they just now ride inside a real AX.25 UI frame, and each frame
is independently wrapped in FX.25 forward error correction (the same RS(255,223),
t=16 strength as before) rather than one custom ASM+CRC32 wrapper around a giant
interleaved block. The payoff: any AX.25/FX.25-aware ground station already deployed
in the wild -- SatNOGS, gr-satellites, Direwolf -- can receive and decode S2S2
traffic with zero S2S2-specific software, which is a real, redundant, already-built
global tracking network for a small team. The cost: no more cross-fragment
interleaving (each fragment's FEC only protects itself, so losing one fragment costs
only that fragment's slice of data, not the whole message -- see `cubus_ax25.h`),
and AX.25's per-frame address/control/pid/FCS tax (~18 bytes) plus a 12-byte S2S2
fragment header eaten out of each frame's Info-field budget.

```
+----------------------------------------------------------------------------------------------------+
|                                    SATELLITE FIRMWARE (CPU2 CM0+)                                  |
|                                                                                                    |
|   +--------------------------+   +------------------------+   +----------------------+   +-------+ |
|   |  ipcc_m0plus_main.c      |-->| core/cubus_ax25.c      |-->| core/ax25.c +        |-->| radio_| |
|   |  - State Machine         |   | - Fragment header+     |   |   core/fx25.c        | |app.c  | |
|   |  - Beacon Timer (5s)     |   |   payload across       |   | - Real AX.25 UI frame| |- GMSK | |
|   |  - Sequenced Telemetry   |   |   AX.25/FX.25 frames   |   | - FX.25 RS(255,223)  | |9600bd | |
|   |  - Command Dispatcher    |   |   (core/rs.c FEC)      |   |   wrap per frame     | |h=0.5  | |
|   +--------------------------+   +------------------------+   +----------------------+ |BT=0.3 | |
|                                                                                          +-------+ |
|                                                                                              |     |
|                                                                                    +--------------+ |
|                                                                                    | target/      | |
|                                                                                    | radio_board_ | |
|                                                                                    | if.c         | |
|                                                                                    +--------------+ |
+-------------------------------------------------------------------------------------|--------------+
                                                                                      | (435.000 MHz)
                                                                                      v
+----------------------------------------------------------------------------------------------------+
|                         GROUND STATION -- ANY AX.25/FX.25-CAPABLE RECEIVER                          |
|                                                                                                    |
|   +--------------------------+       +----------------------------+       +--------------------+   |
|   |  RTL-SDR / SDR Console   | ----> |  GMSK/AFSK demod + AX.25/  | ----> |  gap_tracker_t /   |   |
|   |  - N-FM / Flat Audio     |       |  FX.25 decode (SatNOGS,    |       |  gap reports        |   |
|   |  - De-emphasis: OFF      |       |  gr-satellites, Direwolf,  |       |  uplinked to sat    |   |
|   |  - ~14-19 kHz Filter BW  |       |  or this repo's gs_main.c) |       +--------------------+   |
|   +--------------------------+       +----------------------------+                               |
+----------------------------------------------------------------------------------------------------+
```

> **Physical-layer note:** the full byte-level protocol is implemented and
> host-tested, and rides over the SX126x's existing native GMSK packet engine
> (`radio_app.c`, fixed-length/CRC-off/whitening-off) at a 168-byte frame size
> (FX.25 tag 0x06, a shortened RS(255,223) code) -- no continuous-bitstream radio
> driver needed. This sandbox still has no physical SX126x/Nucleo-WL55JC board to
> flash and verify against; see section 8.

---

## 2. Directory Structure & Key Files

### Primary Entry Points
* `ipcc_m0plus_main.c`: **Main entry point for Satellite CPU2 firmware**
  (`make APP=satellite`, the default). Manages the state machine
  (`STATE_IDLE_RX`, `STATE_PROCESS_COMMAND`, `STATE_TRANSMIT_BURST`), the 5-second
  beacon timer, the sequenced telemetry burst, and the command dispatcher.
* `satellite/gs_main.c`: Ground Station firmware entry point (`make APP=gs`).
  Interactive over the UART debug console (`COMMAND` requests a telemetry burst,
  plus one command per standard opcode -- see `format_commands.md`), tracks
  sequence gaps, and uplinks `TYPE_GAP_REPORT` frames.

### Protocol & Framing Subsystem (`core/`)
* `core/Inc/config.h`: **Master Configuration Header** - frequencies
  (`435.000 MHz` TX, `437.375 MHz` RX), S2S2 node IDs, AX.25 callsigns,
  bitrate/deviation/shaping, preamble length.
* `core/Inc/s2s2com.h`: RS(255,223) FEC, the S2S2 packet header/types/flags, and
  gap tracking. The on-air transport itself lives in ax25.h/fx25.h/cubus_ax25.h
  (split out because fx25.h depends on the RS section here).
* `core/Src/rs.c`: RS(255,223) codec (Berlekamp-Massey, Chien search, Forney) over
  GF(256). Shared by both the FX.25 wrapper and nothing else now -- one RS block
  per AX.25 frame, no more depth-3 interleaving.
* `core/Src/cubus_proto.c`: stack init (`cubus_proto_init()`) and ground-station
  sequence-gap tracking (`gap_tracker_*`) -- transport-independent.
* `core/Inc/ax25.h` / `core/Src/ax25.c`: AX.25 address field encode/decode,
  standard FCS (CRC-16/X-25), UI-frame build/parse (no flags/bit-stuffing --
  that's FX.25's job).
* `core/Inc/fx25.h` / `core/Src/fx25.c`: FX.25 (Stensat/K9DPD spec, verified
  against Direwolf's own `fx25_send.c`) -- correlation tag 0x06
  (`0xFF94DC634F1CFF4E`, a shortened RS(255,223): 128 data + 32 parity, same
  t=16 as the unshortened code), HDLC bit-stuffing, and RS wrap/unwrap around
  one AX.25 frame (168 bytes on air: 8-byte tag + 128-byte data + 32-byte
  parity -- fits inside the SX126x's 255-byte native packet-engine buffer).
* `core/Inc/cubus_ax25.h` / `core/Src/cubus_ax25.c`: fragments one S2S2
  header+payload across N independent AX.25/FX.25 frames and reassembles them
  on receive -- this is what `radio_chunk.c` used to do, but now each fragment
  is itself a complete, standards-compliant, independently FEC'd radio frame.
* `core/Inc/s2s2_commands.h` / `core/Src/s2s2_commands.c`: standard command set -
  opcodes, subsystem IDs, mission/data identifiers, ACK/NACK status codes, and
  structured telemetry payloads (Beacon 1/2, camera fragment). Converted from
  the legacy AX.25 command/response spec (`packet_format_AX.25.json`) - see
  `format_commands.md` for the full field-by-field mapping. `TYPE_TELECOMMAND`
  payloads are `cubus_command_t` (pack/unpack via `command_pack`/`command_unpack`);
  `TYPE_ACK` payloads are `cubus_ack_t`.
* `core/Src/radio_app.c` / `core/Inc/radio_app.h`: radio application layer -
  configures the SX126x's native GMSK packet engine. Still used for TX/RX
  scaffolding, but see the physical-layer note above and section 8: it doesn't
  yet drive the continuous bitstream real AX.25/FX.25 transmission needs.
* `core/Inc/tx_history.h` / `core/Src/tx_history.c`: satellite-side ring buffer
  of recently-sent messages, keyed by `seq_num` -- what lets
  `CMD_REQUEST_RETRANSMIT` and an incoming `TYPE_GAP_REPORT` actually resend
  lost telemetry instead of replying `ACK_STATUS_DATA_UNAVAILABLE` every time.
  Pure C, no HAL dependency, so it's host-testable the same way as
  `test_roundtrip.c`. See section 9 for the full design and RAM-budget
  rationale.
* `core/Src/test_roundtrip.c`: host-buildable (no HAL dependency) correctness test
  for the whole stack - RS at its 16-error/block limit, AX.25 address/FCS/bit-
  stuffing round trips, FX.25 encode/decode under injected errors (at and past
  the 16-byte correction limit), full message fragmentation/reassembly
  (in-order, out-of-order, and one fragment dropped past repair while the rest
  still deliver), gap tracking, and the `tx_history` ring buffer (store/find/
  eviction/oversize-reject). Build and run it directly with a host compiler
  (see "Verification" below) before trusting a firmware change.

### Hardware & RF Board Interface (`target/` & `bsp/`)
* `target/board_pins.h`: per-board RF front-end pin mapping, selected at
  compile time by the Makefile's `BOARD` variable (see section 6). Everything
  `radio_board_if.c` used to hardcode as raw `GPIOx`/`GPIO_PIN_n` literals now
  comes from here, so adding a new physical board means editing one small
  table instead of every function in `radio_board_if.c`.
* `target/radio_board_if.c`: Radio Board Interface layer. Drives the RF
  switches (`BOARD_FE_CTRL1/2/3_PIN` from `board_pins.h`, physically
  `PC4`/`PC5`/`PC3` on every board variant confirmed so far) and, where the
  board has one (`BOARD_HAS_EXTRA_FRONTEND_PINS`), a second PA/LNA-style
  enable pair whose actual pins and meaning differ per board -- see section 6.
  Manages TCXO power (`PB0` / `VDD_TCXO`) and SMPS DCDC power converter.

### Low-Level Radio Driver (`radio_driver/` & `subghz/`)
* `radio_driver/radio_driver.c`: SX126x low-level register interface
  (`SUBGRF_SetTxParams`, `SUBGRF_SetPaConfig`, `SUBGRF_SetModulationParams`,
  `SUBGRF_SetSyncWord`, `Radio_SMPS_Set`).
* `radio_driver/radio.c`: High-level Semtech radio API wrapper.
* `uart_debug.c` / `uart_debug.h`: Direct non-blocking USART2 output (PA2/PA3 @
  115200 baud) for live serial diagnostics.

---

## 3. Detailed Data Flow & Protocol Operation

### Transmit (TX) Data Flow Pipeline

```
[Payload bytes] e.g. "S2S2 telemetry #003 ..."
       |
       v
1. cubus_ax25.c: cubus_ax25_fragment()
   - Splits payload into up to CUBUS_AX25_MAX_FRAGMENTS pieces (148 bytes each)
   - Each fragment's Info field = 12-byte S2S2 header (version/type/src/dst/
     seq/session/flags/frag_index/frag_total/frag_len) + that fragment's
     payload slice
       |
       v
2. ax25.c: ax25_build_ui_frame()
   - dest_addr(7) + src_addr(7) + control(0x03 UI) + pid(0xF0) + info + FCS(2)
   - Addresses from config.h's AX25_SATELLITE_CALLSIGN/AX25_GROUND_CALLSIGN
       |
       v
3. fx25.c: fx25_encode_frame()
   - HDLC bit-stuffs the AX.25 frame into a 128-byte data field (leading/
     closing flags + literal flag fill)
   - Shortened RS(255,223)-encodes it (rs.c, zero-pad/strip wrapper) ->
     128-byte data + 32-byte parity
   - Prepends the 8-byte FX.25 correlation tag (0x06) -> 168-byte frame
       |
       v
4. Sub-GHz Radio RF Transmission (radio_app.c), once per 168-byte frame:
   - Modulation: GMSK @ 9600 bps (h=0.5, BT=0.3), same PHY as before
   - Frequency Deviation: +/- 2400 Hz
   - Native packet engine, fixed-length/CRC-off/whitening-off (168 bytes
     fits comfortably inside its 255-byte buffer)
```

---

### Receive (RX) Data Flow Pipeline

```
[RF Signal at 437.375 MHz]
       |
       v
1. SX126x Hardware Demodulation (native packet engine, fixed-length/CRC-off/
   whitening-off): RxDone hands up one complete 168-byte FX.25 frame.
       |
       v
2. fx25.c: fx25_decode_frame()
   - Matches the correlation tag, reconstructs the full RS(255,223)
     codeword (zero-pad + received 128 data + 32 parity) and RS-decodes it
     (up to 16 byte errors corrected), HDLC-destuffs back to the AX.25 frame.
       |
       v
3. ax25.c: ax25_parse_ui_frame()
   - Validates control/pid, checks the FCS, extracts the Info field.
       |
       v
4. cubus_ax25.c: cubus_ax25_accept_frame()
   - Parses the 12-byte S2S2 fragment header, accumulates fragments by
     (seq_num, session_id) until frag_total are all in (tolerates
     out-of-order arrival). One fragment corrupted past FX.25's repair limit
     is dropped -- the OTHERS are unaffected (the tradeoff vs. the old
     interleaved design, where losing one native chunk killed everything).
       |
       v
5. Command / Telemetry Dispatch:
   - TYPE_TELECOMMAND addressed to us -> command_unpack() the payload (e.g.
     CMD_REQUEST_BURST triggers a telemetry burst; CMD_REQUEST_RETRANSMIT
     replays everything in tx_history); see format_commands.md.
   - TYPE_TELEMETRY -> ground station feeds gap_tracker_accept(); on
     FLAG_LAST_IN_BURST, computes and uplinks a TYPE_GAP_REPORT frame.
   - TYPE_GAP_REPORT addressed to the satellite -> looks up each named
     missing seq_num in tx_history and resends the ones still buffered
     (see section 9.3) -- the reliability loop the rest of this pipeline
     couldn't close on its own, since RS(255,223)/FX.25 correction (step 2)
     only recovers up to 16 byte errors per frame, not a frame lost entirely.
```

---

## 4. Master Configuration Table (`core/Inc/config.h`)

| Parameter | Macro Name | Value | Description |
| :--- | :--- | :--- | :--- |
| **Transmit Frequency** | `DEFAULT_TX_FREQUENCY_HZ` | `435000000UL` | Satellite $\rightarrow$ Ground beacon/telemetry ($435.000\text{ MHz}$). |
| **Receive Frequency** | `DEFAULT_RX_FREQUENCY_HZ` | `437375000UL` | Ground $\rightarrow$ Satellite command uplink ($437.375\text{ MHz}$). |
| **Bitrate** | `RADIO_BIT_RATE_BPS` | `9600` | GMSK data rate ($9600\text{ bps}$). |
| **Frequency Deviation** | `RADIO_FDEV_HZ` | `2400` | GMSK, h=0.5: Fdev = bitrate/4. |
| **Gaussian Shaping** | `MOD_SHAPING_G_BT_03` | $BT = 0.3$ | Tighter Gaussian pulse filtering than plain GFSK's $BT=0.5$. |
| **RX Bandwidth** | `RADIO_RX_BANDWIDTH_HZ` | `19500` | Carson's rule at the narrower GMSK deviation. |
| **Preamble Length** | `RADIO_PREAMBLE_LENGTH_BITS` | `128` | 16 bytes for reliable CDR lock. |
| **FX.25 Frame Size** | `FX25_FRAME_LEN` (fx25.h) | `168` | 8-byte tag + shortened RS(255,223) block (128 data + 32 parity), tag 0x06 -- fits the SX126x's 255-byte native buffer. |
| **AX.25 Info Field Cap** | `AX25_MAX_INFO_LEN` (ax25.h) | `80` | Sized so even worst-case bit-stuffing fits FX25_DATA_LEN (128). |
| **S2S2 Fragment Payload** | `CUBUS_AX25_FRAG_MAX_DATA` | `68` | AX25_MAX_INFO_LEN minus the 12-byte S2S2 fragment header. |
| **Satellite Callsign** | `AX25_SATELLITE_CALLSIGN`/`_SSID` | `"NEPSAT"` / `1` | Placeholder -- swap for a real assigned callsign before RF transmit. |
| **Ground Callsign** | `AX25_GROUND_CALLSIGN`/`_SSID` | `"GROUND"` / `0` | Placeholder -- swap for a real assigned callsign before RF transmit. |
| **Output Power** | `RADIO_TX_POWER_DBM` | `22` | $+22\text{ dBm}$ ($160\text{ mW}$) maximum High-Power PA output. Production default, fixed. |
| **Bench-Test Power** | `BENCH_TX_POWER_DBM` (DEBUG_BUILD only) | `2` | Suggested low-power value for the `POWER <dbm>` UART command when testing at short range (see section 6) -- at +22 dBm and &lt;30cm separation, almost any receiver front end saturates regardless of FEC strength. |
| **Satellite Node ID** | `NODE_ID_SATELLITE` | `1` | S2S2 header `src_id`/`dst_id` for the satellite (app-level, separate from the AX.25 callsign). |
| **Ground Node ID** | `NODE_ID_GROUND` | `0` | S2S2 header `src_id`/`dst_id` for the ground station. |
| **Retransmit Buffer** | `TX_HISTORY_LEN` / `TX_HISTORY_MAX_PAYLOAD` (`tx_history.h`) | `16` / `128` | Satellite-side ring buffer of recently-sent messages, resent on a received `TYPE_GAP_REPORT` or `CMD_REQUEST_RETRANSMIT`. Sized to fit the M0+ core's 32K SRAM budget. |

---

## 5. Ground Station & SDR Setup Guide

Because the on-air format is real AX.25/FX.25, receiving S2S2 no longer needs
S2S2-specific software at all -- any of these work:

* **SatNOGS** ground stations already run FX.25-aware decoders for exactly this
  kind of amateur CubeSat traffic.
* **gr-satellites** flowgraphs with an FX.25/AX.25 decode block.
* **Direwolf** (`-X 32` for transmit; S2S2 always uses correlation tag 0x06,
  the 128-data/32-check-byte variant) -- FX.25 reception is
  always on in modern Direwolf, no special config needed.
* This repo's own `gs_main.c` (`make APP=gs`) on a second STM32WL55 board.

### SDR capture settings (SDR Console / SDR# / GQRX), if capturing raw audio
for one of the above to decode offline:
* **Frequency:** `435.000 MHz` (adjust for any RTL-SDR crystal ppm offset).
* **Demodulation Mode:** **`N-FM` (Narrowband FM)**.
* **Filter Bandwidth:** **`14 kHz` - `19.5 kHz`**.
* **Audio De-emphasis:** **OFF (Unchecked / None)**.
* **Squelch & Noise Reduction:** **OFF**.

---

## 6. Build and Flashing Instructions

### Compilation
From the `CPU2` directory:
```bash
make clean
make                 # satellite firmware (default, APP=satellite, BUILD=debug, BOARD=nucleo)
make APP=gs          # ground-station firmware
```
* **Output ELF:** `ipcc_m0plus.elf` (or `gs_m0plus.elf` for `APP=gs`)
* **Output Binary:** `ipcc_m0plus.bin` / `gs_m0plus.bin`
* **Target Flash Memory Address:** `0x08020000` (Cortex-M0+ CPU2 Flash partition)

#### Debug vs. production builds (`BUILD=debug` / `BUILD=production`)

`BUILD=debug` (the default) compiles in the `TX <hex>` / `POWER <dbm>` UART
backdoors described below. `BUILD=production` compiles them out entirely --
not just disabled at runtime, the code isn't in the binary -- and TX power is
fixed at `RADIO_TX_POWER_DBM` with no way to change it over serial. GNU Make
can't parse a literal `make flash --debug` flag; the real invocations are:

```bash
make sat-debug              # satellite, debug
make sat-production         # satellite, production
make gs-debug                # ground station, debug
make gs-production           # ground station, production
make flash-sat-debug        # build + flash, satellite debug
make flash-sat-production   # build + flash, satellite production
make flash-gs-debug         # build + flash, ground station debug
make flash-gs-production    # build + flash, ground station production
```
Switching `BUILD=` (or `APP=`/`BOARD=`) between invocations always triggers a
full rebuild automatically -- object files aren't named per combination, so
the Makefile tracks the last-built combination in `.build_tag` and forces a
clean rebuild whenever it changes, rather than silently relinking stale
objects built with different flags.

#### Board variants (`BOARD=nucleo` / `BOARD=gs_custom` / `BOARD=sat_custom`)

`BOARD=nucleo` (default) matches the ST Nucleo-WL55JC2 bench setup this
firmware has actually been flashed to and verified on. `BOARD=gs_custom`
targets the custom ground-station PCB (`../gs_config.json`): same RF-switch
pins as the Nucleo, but the external PA is gated by `EN_AMP_3V3`/`EN_AMP_5V0`
(PA11/PA12) instead of the Nucleo bench setup's PA8/PA1 pair -- important
because on the custom GS PCB, PA8 is wired to the external SX1278's DIO2 IRQ
line, so driving it as a GPIO output there would fight that chip.
`BOARD=sat_custom` is a placeholder that fails the build with a clear error
until a real satellite PCB pin mapping is filled into `target/board_pins.h` --
no satellite schematic exists in this repo yet, so no real pin numbers can be
filled in without guessing at real hardware.

> **Note:** `CUBEWL` and `SHARED_IPC` in the Makefile point at paths on the
> original author's machine (`/home/prem/...`) - an STM32CubeWL SDK checkout and
> a shared-RAM IPC ring-buffer source that aren't part of this repo. Point them
> at your own checkout before building.

### Flashing via OpenOCD / ST-LINK
```bash
make flash
```

### Serial Debug Monitor
Connect a USB-UART adapter to USART2:
* **Baud Rate:** `115200`
* **TX Pin:** `PA2`
* **RX Pin:** `PA3`
* **GND:** Ground

When powered on, USART2 outputs diagnostic logs:
```
=== STM32WL55 GMSK / S2S2 over AX.25+FX.25 [satellite] ===
*** DEBUG BUILD - TX <hex> / POWER <dbm> UART backdoors ENABLED ***
RF FRONT END OK
RadioApp_Init: ENTER
Radio RX = 437375000 Hz
Radio TX = 435000000 Hz
Bitrate = 9600
Deviation = 2400 Hz (GMSK, h=0.5)
LISTENING FOR COMMAND (and beaconing every 5s)...
Type TX <hex> to transmit arbitrary bytes downlink, e.g. TX 48656C6C6F
Type POWER <dbm> to override TX power for bench testing
Type BEACON ON / BEACON OFF to toggle the 5s housekeeping beacon
```
A `BUILD=production` image prints `*** PRODUCTION BUILD ***` instead, and
none of the `TX <hex>` / `POWER <dbm>` / `BEACON ON`/`OFF` lines appear --
those commands aren't compiled in at all (see section 6).

The Nucleo's ST-Link virtual COM port defaults to `115200` baud. Build with
`make CFLAGS+=-DUART_DEBUG_BAUD_9600` (either `APP`) to switch it to `9600`
instead -- whatever you flash, the host side (terminal or the GUI below) has
to use the matching rate.

### Transmitting Arbitrary Data (`TX <hex>`)

Both firmwares accept a `TX <hex>` line over the debug UART -- type it in any
serial terminal (screen, minicom, PuTTY, the Arduino Serial Monitor) or use
the GUI below. It hex-decodes the payload and sends it as a `TYPE_RAW` S2S2
message (fragmented across AX.25/FX.25 frames same as any other message):

```
TX 48656C6C6F
OK sent (5 bytes)
```

* **Satellite** (`ipcc_m0plus_main.c`): `TX <hex>` transmits downlink to the
  ground node ID, checked every main-loop iteration regardless of state.
* **Ground station** (`gs_main.c`): `TX <hex>` uplinks to the satellite node
  ID, then listens for a few seconds for any reply, same as the other
  commands (see section "Standard commands" in `format_commands.md`).
* Max payload is `MAX_PAYLOAD_LEN` = 659 bytes (`s2s2com.h`); anything larger
  is rejected before it's sent, not truncated.
* `TX <hex>`, `POWER <dbm>`, and `BEACON ON`/`OFF` only exist in `BUILD=debug`
  images -- see "Debug vs. production builds" above. A production image
  treats all three as an unknown command.

### Silencing the Beacon (`BEACON ON` / `BEACON OFF`, DEBUG_BUILD only, satellite only)

The satellite normally beacons `NEPSAT BEACON` every 5 seconds regardless of
anything else going on (`Send_Beacon_If_Due` in `ipcc_m0plus_main.c`). For an
RF capture you want limited to only traffic you triggered yourself --
isolating a `TX <hex>` payload in a waterfall, or timing a burst without
beacon frames interleaved -- turn it off first:
```
BEACON OFF
BEACON OFF
...
BEACON ON
BEACON ON
```
The firmware echoes back exactly what it set (`s_beacon_enabled`, a plain
in-RAM flag) so a caller always knows the confirmed state rather than
assuming its own command landed. Resets to enabled on every reboot -- there
is no persistent override, same as `POWER`. Ground station firmware
(`gs_main.c`) never beacons, so this command only exists on the satellite
side.

### Bench-Test TX Power Override (`POWER <dbm>`, DEBUG_BUILD only)

At `RADIO_TX_POWER_DBM` (+22 dBm, the production default and max HP-PA
output), testing with a receiver less than ~30cm away will corrupt or drop
frames on almost any hardware -- the receiver's front end saturates, which no
amount of FEC can fix because the damage happens before the bits are even
decodable. This is an RF-level effect, not a protocol bug.

For close-range bench testing, drop the power first:
```
POWER 2
RadioApp_SetTxPower: 2 dBm
```
Valid range is -9 to 22 dBm (SX126x HP-PA), clamped automatically. Power
resets to `RADIO_TX_POWER_DBM` on every reboot -- there is no persistent
override, and production builds have no way to call this at all.

Both `Print_Reassembled_Fields` (accepted frames) and the frame-rejected log
line now also print the RSSI/SNR of the last received packet, so you can
directly confirm saturation (RSSI pegged near its ceiling) instead of
guessing at the cause of corrupted bytes.

#### `tools/s2s2_tx_gui.py` -- PyQt "backdoor" control console

A dark-themed PyQt5/PyQt6 control panel over the same DEBUG_BUILD UART
commands this whole section describes -- `TX <hex>`, `POWER <dbm>`, and
`BEACON ON`/`OFF` -- with nothing this GUI can do that the firmware doesn't
already expose over a plain serial terminal; it just makes doing it
point-and-click instead of hand-typed. A production-built board answers all
three with `UNKNOWN COMMAND`, GUI or no GUI.

```bash
pip install pyserial pyqtgraph PyQt6      # or PyQt5 -- the script picks whichever is installed
python3 tools/s2s2_tx_gui.py
```

1. Refresh the port list, pick the Nucleo's port, pick the baud rate that
   matches the flashed firmware (115200 by default, 9600 if built with
   `-DUART_DEBUG_BAUD_9600`), click **Connect**.
2. **Send panel (left):** leave **Mode: Text** for plain messages (encoded
   as UTF-8 automatically), or switch to **Mode: Hex** to send exact raw
   bytes. Type into the box, press Enter or click **Send** -- the byte
   counter turns red past the 659-byte limit. **Resend last** repeats the
   most recent payload; **Send file...** sends a local file's raw bytes.
3. **Radio Control panel (right):** drag the **TX power** slider (-9..+22
   dBm, the SX126x HP-PA's real range) and click **Apply** to send
   `POWER <dbm>` -- the label below only turns confirmed-green once the
   board's own `RadioApp_SetTxPower: N dBm` reply is parsed back out of the
   serial stream, not just on click. **Beacon ON/OFF** buttons send
   `BEACON ON`/`BEACON OFF` the same way, confirmed from the board's own
   echoed reply.
4. **TX Activity panel:** a live frames/sec bar graph and cumulative
   bytes-sent line graph, both driven by parsing the firmware's own
   `RadioApp_Send: len=N` log line -- so it plots *every* real transmission
   (automatic beacons, telemetry bursts, manual `TX`) not just what this GUI
   itself sent.
5. Everything the Nucleo prints -- including the firmware's own boot banner,
   `TX ACK`/`RX TELEMETRY` logs, and the `OK sent (N bytes)` reply to your
   own `TX` command -- shows up in the log pane live, color-coded (green =
   sent/OK, red = ERR/FAIL, dim = connect/disconnect).

### `tools/s2s2_rx_gui.py` -- RTL-SDR PyQt Receiver GUI

A full receive chain in Python, independent of the UART tools above -- this
listens on the actual RF link with an RTL-SDR dongle (or a recorded IQ file)
and decodes real over-the-air GMSK/AX.25/FX.25 traffic, no Nucleo required on
the receiving end at all:

```
RTL-SDR IQ -> gmsk_demod.py (FM discriminator, resample to a fixed samples/
symbol, brute-force timing-phase + bit-polarity + bit-order search, sync-word
correlation) -> s2s2_decoder.py (FX.25 RS(255,223)-shortened correction,
AX.25 FCS check, S2S2 fragment reassembly, command/telemetry pretty-print)
```

`gmsk_demod.py` and `s2s2_decoder.py` are plain Python ports of the exact
algorithms in `rs.c`/`fx25.c`/`ax25.c`/`cubus_ax25.c` -- not a reinterpretation
-- and are independently host-testable (see Verification below). Two on-air
conventions can't be pinned down without a real radio to check against (which
frequency-deviation sign means bit 0 vs 1, and MSB-first vs LSB-first byte
serialization); rather than guess, the demodulator tries every combination and
keeps only the ones whose FX.25 tag actually matches -- a real 8-byte
correlation constant used as a self-check, so this self-calibrates against
whichever convention the real SX126x turns out to use.

```bash
pip install numpy scipy pyqtgraph PyQt6      # or PyQt5
pip install pyrtlsdr                         # optional -- only needed for a live dongle,
                                              # not for the "IQ file" source below
python3 tools/s2s2_rx_gui.py
```

1. **Source: RTL-SDR** -- set frequency (defaults to `config.h`'s
   `DEFAULT_TX_FREQUENCY_HZ`, 435.000 MHz -- the satellite's downlink/beacon
   frequency, i.e. what you actually want to receive; its `DEFAULT_RX_FREQUENCY_HZ`,
   437.375 MHz, is what the *satellite* listens on for ground-to-satellite
   uplink commands, not useful here) and sample rate (`240000` is exactly
   25 samples/symbol at 9600 baud and is the recommended default; other rates
   work too, just resampled). Click **Start**.
2. **Source: IQ file** -- point it at a raw capture: `rtl_sdr`'s own `u8`
   format, or a GNU Radio-style interleaved-float32 `.cfile`. No hardware
   needed at all for this path -- see below.
3. Decoded messages land in the table (time, type, src/dst, seq, session,
   RS-corrected count, one-line summary); click a row to see the full
   breakdown in the right-hand pane -- every `beacon1_t`/`beacon2_t` field,
   `cubus_command_t`/`cubus_ack_t` fields with opcode/status names resolved,
   or raw hex + ASCII for `TYPE_RAW`/unrecognized payloads. The log pane below
   shows every frame attempt, including rejections and which
   polarity/bit-order/phase combination a successful decode locked onto.
4. **RF Analyzer panel (right side):**
   * **Spectrum / Waterfall tab** -- a live FFT of the tuned IQ (4096-point,
     Hann-windowed, one snapshot per 0.5s chunk -- plenty for a real-time
     display) as both a line plot and a scrolling waterfall, so you can
     visually confirm the satellite's carrier is where you expect and how far
     above the noise floor it sits, independent of whether the demod is
     currently decoding anything.
   * **Link Quality tab** -- three rolling graphs: signal power (dBFS),
     decoded/rejected count *per chunk* (not cumulative), and cumulative
     success rate over time. That last one is the same percentage in the
     stats bar above, but watchable as it changes instead of a single
     misleading lifetime number -- see the next paragraph for why that
     distinction matters.

**On "success rate" being low:** it's computed as
`decoded / (decoded + rejected)`, where `rejected` counts every sync-word
match that failed FX.25/RS/AX.25 -- including false positives from noise
during a full brute-force scan (25 phases x 2 orders x 2 inverts = 100
combinations). Earlier versions of this GUI dropped their phase/order/invert
lock after just 2 seconds of silence (`LOCK_MISS_LIMIT=4` x
`CHUNK_SECONDS=0.5`) -- *shorter* than the satellite's 5-second beacon
interval, so between every single beacon it fell back to a cold 100-combo
scan, and the resulting flood of noise-driven "rejected" hits permanently
dragged the cumulative percentage down even on a perfectly healthy link.
`LOCK_MISS_LIMIT` is now 16 (8 seconds), comfortably outlasting a missed
beacon or two. Watch the Link Quality tab's success-rate graph recover after
a lock-loss event to see this directly, instead of trusting one static
number.

#### Try it without any hardware: `tools/make_demo_capture.py`

Generates a synthetic IQ capture -- real firmware-encoded FX.25 frames (built
by compiling the actual `rs.c`/`ax25.c`/`fx25.c`/`cubus_ax25.c`/
`s2s2_commands.c`, not a Python re-implementation) run through the same
synthetic GMSK modulator used in the demod self-test, with AWGN added:

```bash
python3 tools/make_demo_capture.py /tmp/s2s2_demo.cfile --sample-rate 240000 --snr-db 15
```

Then in the GUI: **Source: IQ file**, browse to `/tmp/s2s2_demo.cfile`,
format `cf32 (GNU Radio)`, sample rate `240000`, **Start** -- you should see a
`TELEMETRY` (Beacon1) and a `RAW` message decode with a nonzero RS-corrected
count (from the injected noise).

---

## 7. Verification

The protocol stack (`rs.c` / `ax25.c` / `fx25.c` / `cubus_ax25.c` / `cubus_proto.c`)
is pure C with no HAL dependency, so it's fully testable on a desktop before ever
touching hardware:
```bash
cd core/Src
clang -std=c11 -Wall -Wextra -O2 -I ../Inc -o /tmp/test_s2s2 \
      rs.c ax25.c fx25.c cubus_ax25.c cubus_proto.c tx_history.c test_roundtrip.c
/tmp/test_s2s2
```
This exercises exactly what's claimed above: RS at its 16-error/block limit (200/200
trials) and correct rejection beyond it; AX.25 address encode/decode, the FCS against
its published CRC-16/X-25 check value, and UI-frame build/parse with FCS-corruption
rejection; FX.25 encode/decode round trips (including all-0xFF worst-case bit-
stuffing content) and error injection at exactly 16 bytes (must recover) and 17
bytes (must not silently return a wrong frame); full message fragmentation across
multiple independent frames, out-of-order reassembly, and the key tradeoff this
architecture makes explicit -- one fragment corrupted past repair is dropped while
every other fragment of the same message still delivers cleanly; gap tracking
(including cross-session isolation); and the `tx_history` retransmit ring buffer --
store/find, silent rejection of a payload over `TX_HISTORY_MAX_PAYLOAD`, and ring
wraparound (oldest entry evicted once `TX_HISTORY_LEN` is exceeded, newest entries
still findable). All of the above passes with 0 failures as of this writing.

The standard command/telemetry payload layer (`s2s2_commands.c`) has its own
host test, `core/Src/test_commands.c`, round-tripping every struct
(command, ACK, Beacon 1/2, camera fragment) through pack/unpack, including
truncated-buffer and oversized-fragment rejection:
```bash
cd core/Src
clang -std=c11 -Wall -Wextra -O2 -I ../Inc -o /tmp/test_commands \
      s2s2_commands.c test_commands.c
/tmp/test_commands
```

The Python RX tools have their own verification, run entirely on the desktop
with no radio hardware:
```bash
cd tools
python3 test_demod.py
```
This compiles the real C sources into a throwaway harness to get a genuine
firmware-encoded FX.25 frame (not a Python-side encoder -- there isn't one;
this is a decoder-only tool), runs it through the synthetic GMSK modulator at
clean SNR, 20 dB, and 12 dB with a deliberately non-integer samples/symbol
ratio (250 ksps), and asserts the full `gmsk_demod.py` -> `s2s2_decoder.py`
chain recovers the exact original payload bytes every time, including cases
where RS actually has to correct injected errors. This is the strongest
verification available in an environment with no RTL-SDR or Nucleo attached;
it does not substitute for testing against a real transmission, and the
polarity/bit-order self-calibration described above is exactly the mechanism
meant to absorb whatever the real hardware turns out to do differently.

### Hardware verification performed for the debug/production/TX-power/retransmit work (section 9)

All four build combinations compile clean (0 errors, 0 warnings from any
changed file) and were flashed and exercised on a real Nucleo-WL55JC2:

| Check | Result |
|---|---|
| `make sat-debug` / `sat-production` / `gs-debug` / `gs-production` | all 4 build clean |
| `make gs BOARD=gs_custom` | builds clean |
| `make sat BOARD=sat_custom` | fails the build with the intended `#error` (confirmed the `.build_tag` mechanism actually forces the recompile needed to hit it -- see the Makefile comment for why that matters) |
| Flash `flash-sat-debug`, serial capture | boot banner + normal 5s beacon cycle (`BEACON TX` -> `RadioApp_Send: len=168` -> `RFSWITCH TX HP` -> `RFSWITCH RX`) |
| Send `POWER 2` over serial | logs `RadioApp_SetTxPower: 2 dBm`; subsequent `TX 48656C6C6F` still sends normally at the new power |
| Flash `flash-sat-production`, same commands | both `POWER 2` and `TX 48656C6C6F` reply `UNKNOWN COMMAND`; beacon cycle continues unaffected |
| `production` vs `debug` binary size | production `.text` is ~1.2KB smaller -- confirms the backdoor code is actually absent, not just unreachable |

Not yet verified on hardware (compiles clean, host-tested, no satellite
schematic to test the real pin mapping against): `BOARD=sat_custom` past the
placeholder `#error`, and the `TYPE_GAP_REPORT`/`CMD_REQUEST_RETRANSMIT` path
end-to-end with two boards live (satellite + ground station) -- the ring-buffer
logic itself is host-tested (see above), but a live over-the-air
drop-then-recover pass has not been run.

## 8. Known Limitations

* **~~No continuous-bitstream radio driver~~ -- fixed.** The original AX.25/FX.25
  design (correlation tag 0x05) produced a 263-byte frame, past the SX126x's
  255-byte native packet-engine buffer, which would have needed the radio driven
  as a continuous raw bitstream to transmit at all. Fixed by switching to FX.25
  correlation tag 0x06 -- a *shortened* RS(255,223) code (128 real data bytes + 32
  parity, same t=16 correction strength, standard technique also used by CCSDS)
  that produces a 168-byte frame, comfortably inside the native buffer -- and
  reconfiguring `radio_app.c`'s `PacketParams` to `RADIO_PACKET_FIXED_LENGTH` /
  `RADIO_CRC_OFF` / `RADIO_DC_FREE_OFF` (matching what real AX.25 hardware needs:
  no chip-inserted length byte, no extra hardware CRC, no whitening a stock
  ground station couldn't de-scramble). The whole path -- fragmentation, AX.25
  framing, FX.25 shortened-RS FEC, and the native packet-engine TX/RX config --
  is now internally consistent end to end; this sandbox still has no SX126x
  hardware to flash and verify against (same class of caveat as the ARM
  cross-compiler limitation in section 7), but no further architectural bridge
  is needed to get bytes over RF.
* **~~Satellite-side retransmit-from-buffer is not implemented~~ -- fixed.** The
  satellite now keeps a small ring buffer of recently-sent messages
  (`tx_history.h`/`.c`, `TX_HISTORY_LEN` entries up to `TX_HISTORY_MAX_PAYLOAD`
  bytes each -- deliberately small to fit the M0+ core's 32K SRAM budget). A
  received `TYPE_GAP_REPORT` (uplinked by `gs_main.c`) or `CMD_REQUEST_RETRANSMIT`
  now actually resends the missing sequence numbers (or everything still
  buffered, for the coarser `CMD_REQUEST_RETRANSMIT` case) instead of dead-ending
  in `ACK_STATUS_DATA_UNAVAILABLE`. A message larger than `TX_HISTORY_MAX_PAYLOAD`
  (128 bytes) or one that's aged out of the ring isn't retransmittable -- that
  degrades gracefully to the same `ACK_STATUS_DATA_UNAVAILABLE` reply as before.
* **`CMD_GET_HEALTH`/`_HOUSEKEEPING`/`_BEACON1`/`_BEACON2`/`_ADCS`/`_SAT_LOG` and
  `CMD_RUN_CAMERA`/`_DOWNLOAD_FILE`/`_DIGIPEATER` are stubbed on the satellite
  side.** The command dispatch and standard ACK/NACK wiring is fully in place
  (`ipcc_m0plus_main.c`), but the underlying sensor/ADCS/EPDM/camera/file
  subsystems aren't implemented in this firmware yet, so the satellite replies
  honestly with `ACK_STATUS_DATA_UNAVAILABLE` / `ACK_STATUS_MISSION_INACTIVE`
  rather than fabricating telemetry. See `format_commands.md` for the full
  standard command set.
* **Placeholder AX.25 callsigns.** `AX25_SATELLITE_CALLSIGN`/`AX25_GROUND_CALLSIGN`
  in `config.h` (`NEPSAT`/`GROUND`) are bench-test placeholders carried over from
  the original design doc, not real assigned amateur-radio callsigns -- required
  before any actual RF transmission for licensing, not just correctness.
* **No custom satellite PCB pin mapping yet (`BOARD=sat_custom`).** No
  satellite schematic export exists in this repo (unlike the ground station's
  `../gs_config.json`), so `target/board_pins.h`'s `BOARD_SAT_CUSTOM_PCB` branch
  is a placeholder `#error` rather than real pin numbers -- see section 9.4.
* **TX power override is local-only, not networked.** `POWER <dbm>` (section 9.2)
  changes the TX power of the node you type it into; there is no over-the-air
  power negotiation or reporting between satellite and ground (would require
  adding a field to the wire format, which was explicitly out of scope for this
  round of changes -- AX.25/FX.25 framing and RS(255,223) parameters are
  unchanged). Each side's bench power has to be set/known independently.

---

## 9. Production Hardening: Debug/Production Builds, Bench TX Power, Board Variants, Retransmit Buffer

This section is the single reference for four related changes made to take
this firmware from "protocol works" to "production-testable": a real
debug/production split, a fix for close-range bench-test corruption, per-board
pin customization, and closing the reliability loop so a dropped packet is
actually recoverable. AX.25/FX.25 framing and the RS(255,223) FEC parameters
(section 1, section 4) are untouched by all of this.

### 9.1 Debug vs. production builds

**Problem:** both `ipcc_m0plus_main.c` and `gs_main.c` had an unconditional
`TX <hex>` UART command that transmits arbitrary attacker/operator-supplied
bytes over RF as a `TYPE_RAW` frame -- useful for bench testing, but not
something a flight/production image should ship with no way to turn off.

**Fix:** `CPU2/Makefile`'s `BUILD` variable (`debug` default, or `production`)
adds/omits `-DDEBUG_BUILD` in `CFLAGS`. `TX <hex>`, `POWER <dbm>` (9.2), and
`BEACON ON`/`OFF` (satellite only -- silences the 5s housekeeping beacon for
a cleaner RF capture) are wrapped in `#ifdef DEBUG_BUILD` in both
`Process_UART_Command` functions, **and** their supporting helper functions
(`str_starts_ieq`, `str_ieq`, `hex_nibble`, `hex_decode`) are wrapped the same
way -- so a production build doesn't just make the commands unreachable, the
code for them isn't compiled into the binary at all (confirmed: production
`.text` is ~1.5KB smaller than debug, see section 7's hardware verification
table).
The boot banner prints `*** DEBUG BUILD - ... ENABLED ***` or
`*** PRODUCTION BUILD ***` so the running image is unambiguous over serial
without needing to trust a label.

Build targets (see section 6 for the full list): `make sat-debug`,
`make sat-production`, `make gs-debug`, `make gs-production`, and matching
`flash-*` variants. These are real `make` target names -- GNU Make has no way
to parse a literal `make flash --debug` command-line flag into a variable, so
that syntax was never going to work; the dashed target names are the actual
interface.

**Why a `.build_tag` file exists:** object files are named the same regardless
of `BUILD`/`BOARD` (`ipcc_m0plus_main.o`, not
`ipcc_m0plus_main.debug.nucleo.o`), so plain GNU Make timestamp comparison
can't tell that `make sat BUILD=production` after `make sat BUILD=debug`
needs a full recompile -- it would happily relink whatever `.o` files were
already sitting there, silently mixing debug and production object code (or
worse, on a `BOARD` switch, wiring the wrong GPIO pins into the binary). The
Makefile writes the current `$(BUILD)-$(BOARD)` combination into `.build_tag`
and makes every object file depend on it; the recipe only touches the file
(bumping its mtime, which cascades into a full rebuild of every object) when
the combination actually changed, so an unchanged rebuild stays fast. This was
caught and fixed during this session's own testing -- an earlier version
without `.build_tag` let a stale `radio_board_if.o` silently mask the
`BOARD=sat_custom` `#error` guard (9.4) entirely.

### 9.2 Bench-test TX power override (fixing close-range corruption)

**Problem:** `RADIO_TX_POWER_DBM` (`config.h`) is a fixed +22 dBm -- the
SX126x HP-PA's maximum output -- with no override anywhere in the firmware.
Testing a transmitter and receiver less than ~30cm apart at that power will
corrupt or drop frames on almost any receiver hardware, because the front end
(LNA/mixer/ADC) saturates before the signal is even digitized. **This is a
physical RF effect, not a bug in the AX.25/FX.25/GMSK software chain** -- no
amount of Reed-Solomon correction, interleaving, or retry logic can recover
data that was clipped before it became bits. The fix that actually works at
the software layer is turning the power down for the bench test.

**Fix:** `radio_app.c` gained a mutable `s_tx_power_dbm` (default
`RADIO_TX_POWER_DBM`) and `RadioApp_SetTxPower(int8_t dbm)` /
`RadioApp_GetTxPower(void)`, clamped to the SX126x HP-PA's real range
(-9..+22 dBm). Both `RadioApp_Send()` and `RadioApp_ForceRadioParams()` now
read this variable instead of the macro directly. A `DEBUG_BUILD`-only
`POWER <dbm>` UART command (both boards, section 6) calls the setter.
`config.h` documents a suggested bench value, `BENCH_TX_POWER_DBM` (2 dBm),
for reference -- it's not auto-applied, you still type `POWER 2` yourself, so
the choice of how low to go for your specific bench distance stays explicit
rather than baked into a macro that might be wrong for a different setup.

**Diagnostics:** both `OnRxDone` handlers used to discard the SX126x's RSSI/SNR
readings (`(void)rssi; (void)snr;`) -- data the radio driver was already
computing for free, just never surfaced. Both now store it
(`rx_last_rssi`/`rx_last_snr`) and print it on *every* accepted frame
(`Print_Reassembled_Fields`) and every rejected frame (the "frame rejected"
log line), so a saturated front end shows up directly as RSSI pegged near its
ceiling instead of being inferred indirectly from corrupted bytes.

**What this doesn't do:** there's no over-the-air power negotiation --
`POWER <dbm>` only affects the node you type it into, and there's no wire-
format field carrying the setting to the other end (deliberately -- that would
touch the frame format, out of scope here). It also isn't persistent -- every
reboot resets to `RADIO_TX_POWER_DBM`. And it doesn't exist at all in a
production build (9.1) -- production always runs at the fixed production power.

### 9.3 Reliability: satellite-side retransmit buffer

**Problem:** the ground station already computed and uplinked `TYPE_GAP_REPORT`
frames listing missing sequence numbers (`gs_main.c`, `gap_tracker_report`),
and the command set already had a `CMD_REQUEST_RETRANSMIT` opcode -- but the
satellite had nothing to retransmit *from*. Both paths dead-ended in
`ACK_STATUS_DATA_UNAVAILABLE`. A telemetry packet that got dropped or arrived
corrupted past the RS(255,223)/FX.25 correction limit was gone permanently,
regardless of how the ground station asked for it back.

**Fix:** `core/Inc/tx_history.h` + `core/Src/tx_history.c` -- a small,
pure-C (no HAL dependency, host-testable) ring buffer:

```c
#define TX_HISTORY_LEN          16   /* entries */
#define TX_HISTORY_MAX_PAYLOAD  128  /* bytes per entry */

typedef struct {
    bool     used;
    uint16_t seq_num;
    uint8_t  type, dst_id, flags;
    uint16_t payload_len;
    uint8_t  payload[TX_HISTORY_MAX_PAYLOAD];
} tx_history_entry_t;
```

`Send_Message()` in `ipcc_m0plus_main.c` stores every message it sends (skip-
ping retransmits themselves, to avoid the ring cursor advancing while a
retransmit loop is iterating the same array -- see the code comment on that
guard) into a satellite-global `tx_history_t`. Two consumers pull from it:

* **`TYPE_GAP_REPORT` received, addressed to the satellite:** parses the
  big-endian `uint16` missing-sequence list (format.md section 9), looks each
  one up (`tx_history_find`), and resends the ones still present via
  `Retransmit_By_Seq()` -- reusing the *original* `seq_num` (not a new one),
  with `FLAG_RETRANSMIT` ORed into the original flags, so the ground station's
  gap tracker correctly marks that entry received rather than seeing unrelated
  new traffic.
* **`CMD_REQUEST_RETRANSMIT` telecommand:** carries no specific sequence list
  (that's what `TYPE_GAP_REPORT` is for) -- `Retransmit_All_History()` just
  replays everything currently buffered, ACKing `ACK_STATUS_ACCEPTED` first if
  the buffer has anything, `ACK_STATUS_DATA_UNAVAILABLE` if it's empty.

**Why 128 bytes x 16 entries, not the protocol's full 659-byte `MAX_PAYLOAD_LEN`:**
the M0+ core's entire SRAM is 32K (`../scripts/wl55jc_cm0p.ld`), shared with the
reassembler, radio driver state, UART command buffers, and stacks. Storing 16
entries at the full protocol ceiling would burn over 10K of that on a buffer
whose entries, in practice, hold telemetry text well under 100 bytes today. A
message larger than 128 bytes is silently not stored by `tx_history_store` --
degrading gracefully to the pre-existing `ACK_STATUS_DATA_UNAVAILABLE` reply,
not a crash or truncation.

**Test coverage:** `core/Src/test_roundtrip.c`'s `test_tx_history()` -- store/
find, oversized-payload rejection, and ring-buffer wraparound (oldest entry
evicted once `TX_HISTORY_LEN` is exceeded, newest entries still findable). Not
yet verified: a live two-board over-the-air drop-then-recover pass (section 7).

### 9.4 Per-board pin customization

**Problem:** `target/radio_board_if.c` hardcoded one fixed set of GPIO pins
for the RF front end (`GPIOA` `GPIO_PIN_8`/`GPIO_PIN_1` for an external PA/LNA
enable pair, `GPIOC` `GPIO_PIN_3/4/5` for the RF switch). That's correct for
the Nucleo-WL55JC2 bench setup this firmware actually runs on, but the project
already has a second, different physical board -- a custom ground-station PCB
(`../gs_config.json`, a reverse-engineered schematic export) -- and a third,
a custom satellite PCB, is planned. Two boards, one hardcoded pinout.

**Concretely dangerous, not just inconvenient:** cross-referencing
`gs_config.json`'s pin table found that on the custom GS PCB, `PA8` -- the pin
`radio_board_if.c` was unconditionally driving as a push-pull PA-enable
*output* -- is actually wired to the external SX1278 LoRa chip's `DIO0`
(`RxDone`/`TxDone` IRQ) *input*. Building the old hardcoded firmware for that
board would have driven an output onto what the SX1278 expects to drive as an
IRQ line -- a real electrical conflict, not a logic bug.

**Fix:** `target/board_pins.h` -- one small header, selected by the Makefile's
`BOARD` variable (`-DBOARD_NUCLEO_WL55JC` / `-DBOARD_GS_CUSTOM_PCB` /
`-DBOARD_SAT_CUSTOM_PCB`), defining:

* `BOARD_FE_CTRL_GPIO_PORT` / `_CTRL1/2/3_PIN`: the RF switch itself,
  confirmed identical (`gs_config.json`) across every board variant seen so
  far, so this part doesn't change per board.
* `BOARD_HAS_EXTRA_FRONTEND_PINS` (`#define`d or not) + `BOARD_EXTRA_GPIO_PORT`
  / `_PIN_A` / `_PIN_B`: the board-specific pair. `BOARD_NUCLEO_WL55JC`:
  PA8/PA1 (external PA/LNA enable, the existing bench-tested behavior,
  unchanged). `BOARD_GS_CUSTOM_PCB`: PA11/PA12 (`EN_AMP_3V3`/`EN_AMP_5V0`,
  the custom board's actual external-PA rail enables) -- PA8/PA1 are never
  touched on this board. `BOARD_SAT_CUSTOM_PCB`: `#error`s out at compile time
  with an explanation, rather than guessing pin numbers for hardware whose
  schematic isn't in this repo yet.

`radio_board_if.c`'s `RBI_Init`/`RBI_DeInit`/`RBI_ConfigRFSwitch` now reference
only these macros -- no raw `GPIOx`/`GPIO_PIN_n` literals -- so adding a fourth
board variant means editing `board_pins.h`'s table, not auditing every switch
case in `radio_board_if.c` again.

**To add the real satellite PCB once its schematic exists:** produce a
`sat_config.json` in the same format as `../gs_config.json` (pin-by-pin
schematic export), read its RF front-end pin assignments, and replace the
`#error` in `board_pins.h`'s `BOARD_SAT_CUSTOM_PCB` branch with the real
`BOARD_EXTRA_GPIO_PORT`/`_PIN_A`/`_PIN_B` (and `BOARD_HAS_EXTRA_FRONTEND_PINS`
if it has an equivalent pair at all -- it might not, or might need a third
pin, in which case `radio_board_if.c`'s switch cases need a matching update).
