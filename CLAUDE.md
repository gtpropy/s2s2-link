# S2S2 CubeSat Firmware -- Project Notes

Dual-core STM32WL55JC1 CubeSat digipeater/comms firmware. CPU1 (Cortex-M4,
`nuttx/`+`apps/` submodules) runs NuttX; CPU2 (Cortex-M0+, `JC2/`) runs the
bare-metal radio stack this file is mostly about.

## Where things live

- `JC2/` -- CPU2 (M0+) firmware. Base code is a third-party dual-core
  STM32WL55 AX.25/G3RUH implementation with real hardware-validated RF
  front-end fixes (RFO_HP vs RFO_LP, RF switch polarity, PA ramp timing --
  see `JC2/RF_FRONTEND_AND_TIMING_REPORT.md`). Keep that base code's own
  structure/naming untouched where possible; our additions are layered on
  top of it, inside its existing files, not as parallel modules.
- `JC2/Mission/protocol.c` -- the base's own AX.25 build/extract/verify
  functions, **plus our light FEC wrap** (see below).
- `JC2/Main_code/Main_Satellite/` -- satellite firmware. `satellite_main.c`
  is the real mission loop AND owns all CW1/2/ACK/Camera-fragment packing
  (`CwBeacon1_Pack`, `Satellite_BuildAndSendTelemetry`, etc. -- moved here
  from `satellite_app.c` since this is the file that actually drives the
  loop and owns the IPC structs they pack from). The satellite only
  downlinks CW1/CW2 (Morse) -- Beacon1/Beacon2 AX.25 binary packets are
  not built or sent, by design (see "Packet Format sheet compliance").
  `satellite_app.c` is
  the base repo's own radio/CW-keyer/RF-timing layer (`Satellite_Send_
  Packet_Timeout`, `Satellite_GFSK_Prepare`, the CW Morse keyer, etc.) --
  treat it as lower-level support code `satellite_main.c` calls into, not
  a place to add new packet-format logic. `satellite_main.c` also now
  implements the debug backdoor on UART1 that `tools/console.py`/
  `tx_gui.py`/`satellite_gui.py` already documented/expected but that
  never actually existed before (`Process_UART_Command`, polled from
  both the main loop and `M0_Listening_Delay`'s wait loop) -- commands:
  `TX <hex>`, `POWER <dbm>`, `TXCW <text>` (not yet wired -- see below),
  `BEACON ON/OFF`, `TESTCW`/`TESTBEACON1`/`TESTBEACON2` (one-off sends;
  Beacon1/2 AX.25 stays debug-only, the operational loop is still
  CW-only), `TESTDIGI <hex>` (decode self-test through the real FEC/auth
  pipeline, NOT a multi-hop relay -- no such feature exists here), and
  `SATMODE ON/OFF` (the real safety lockout: ON disables every other
  debug command, matching "behaves like a real deployed satellite").
  `MEMSTAT` does a real stack-painting high-water-mark measurement
  (`_ebss`/`_estack` from `scripts/wl55jc_cm0p.ld`), painted once early
  in `main()`. No `DEBUG_BUILD` compile-time strip (that production-
  build concept doesn't exist in this Makefile) -- always compiled in,
  `SATMODE` is the only lockout. **Not yet tested against real
  hardware** -- implemented and build-verified only, written directly
  from `tools/console.py`'s documented command/response-regex protocol
  plus the old (pre-swap) `ipcc_m0plus_main.c`'s reference implementation
  of the same backdoor, adapted to this codebase's actual function names
  (`Satellite_Send_Packet_Timeout`, `Protocol_ExtractFrame`/
  `VerifyPacket`/`DecodePayload`, `RadioApp_SetTxPower`). `TXCW <text>`
  (arbitrary free-text CW) is in console.py's vocabulary but not yet
  implemented -- would need a raw-string path into the Morse keyer
  separate from the structured CW1/CW2 hex builders.
- `JC2/Main_code/Main_GS/gs_main.c` -- ground-station firmware (UART
  console, command TX, telemetry RX/display + sheet-aware RX decode,
  `Print_Decoded_Sheet_Payload`).
- `JC2/ipc/Sring_buffer.h` -- CPU1<->CPU2 shared-SRAM2 ring buffers.
  `tx_packet_s`'s payload is a union of `beacon1_ipc_s`/`beacon2_ipc_s`,
  sheet-exact field-for-field mirrors of `Beacon1Packet_t`/`Beacon2Packet_t`
  (minus head/type/chk_crc) -- CPU1 fills every field it has a real sensor
  for, satellite_main.c copies them straight across with no scaling/
  guessing. `rx_command_s` carries ground commands the other way.
- `JC2/tools/` -- our own Python tooling (decoder, GUIs, test suites,
  fault injection, mission control dashboard). Not part of the base repo.
- `nuttx/`, `apps/` -- git submodules, CPU1/NuttX side. Builds now (see
  "Building CPU1" below) and has one new app, `apps/examples/s2s2sim`
  (`CONFIG_EXAMPLES_S2S2SIM`) -- a BENCH-TEST-ONLY task that calls the
  real `rb_ipc_init()`/`live_telem_update_b1()`/`live_telem_update_b2()`
  (compiled straight from `JC2/ipc/Sring_buffer.c`, single source of
  truth for the wire format) with SIMULATED sensor values, to exercise
  the CPU1->CPU2 IPCC + ring-buffer path end to end with no real sensor
  driver. Command dispatch semantics (what CPU1 does with a forwarded
  command byte) are still unknown/unverified (see "Known gaps") -- the
  sim app only feeds telemetry, it doesn't touch the command path.

## Build

```
cd JC2
make APP=satellite          # -> satellite.bin (flash @ 0x08032000)
make APP=gs                 # -> gs.bin
make AUTH=FALSE ...         # disable the FEC wrap's auth half (see below)
make probes                 # list connected ST-Link serials
make flash_sat SERIAL=<x>   # flash a specific probe -- REQUIRED if >1
make flash_gs SERIAL=<x>    #   ST-Link is connected, openocd can't guess
```

Toolchain resolution: `ARM_TOOLCHAIN_BIN` env var > `.toolchain_path` (written
by `../setup.sh`) > bare `arm-none-eabi-gcc` on PATH. Needed because
Homebrew's `arm-none-eabi-gcc` 16.x can't find its own newlib headers
("stdio.h: No such file or directory") -- the real ARM GNU Toolchain
install (e.g. `/Applications/ArmGNUToolchain/.../bin`) works fine.

### Building CPU1 (NuttX)

```
cd nuttx
PATH="/Applications/ArmGNUToolchain/15.3.rel1/arm-none-eabi/bin:/opt/homebrew/bin:$PATH"
unset CPATH C_INCLUDE_PATH CPLUS_INCLUDE_PATH OBJC_INCLUDE_PATH
./tools/configure.sh nucleo-wl55jc:demo
kconfig-tweak --file .config --enable CONFIG_EXAMPLES_S2S2SIM     # sim telemetry task
kconfig-tweak --file .config --enable CONFIG_ARCH_BOARD_ENABLE_CPU2  # boot CPU2 from CPU1
kconfig-tweak --file .config --enable CONFIG_BOARD_LATE_INITIALIZE
gmake olddefconfig
gmake -j4          # -> nuttx.bin
```

Three real environment bugs had to be fixed before this would build at
all (none were source bugs): (1) several symlinks under `nuttx/` and
`apps/` were absolute paths baked in under this project's old directory
name (`nuttxspace/reedsolomon/...`, from before it got renamed to
`s2s2-link`) -- `gmake distclean` + re-running `configure.sh` regenerates
them correctly; (2) macOS's system `make` is GNU Make 3.81 (2006), too
old for NuttX's Kconfig-driven Makefiles -- install a modern one via
`brew install make` (lands as `gmake`, doesn't touch `/usr/bin/make`);
(3) macOS has no `flock(1)` (Linux util-linux tool) -- `apps/`'s build
uses it to safely merge every app subdirectory's objects into the one
shared `libapps.a`, so without it the archive silently never gets
created (`gmake` exits 0, no error, just no file) -- `brew install flock`.

## The light FEC wrap (honest name: RS(255,223) + a keyed tag)

Every `Protocol_CreatePacket` payload gets wrapped, before AX.25 framing,
with a sequence counter + length byte, a keyed integrity tag, and
Reed-Solomon parity -- all inside `JC2/Mission/protocol.c`, transparent to
every call site. `Protocol_DecodePayload` (called from `gs_main.c` right
after the AX.25 FCS check passes) is the inverse.

**On purpose, the source code never says what this is.** Identifiers and
comments describe function, not algorithm, so a casual read of
`protocol.c` looks like ordinary packet plumbing, not "we added Reed-
Solomon and a MAC". If you're editing this code, the real names are:

| In the code (what you'll see) | What it actually is |
|---|---|
| `sealEncode` / `sealDecode` | RS(255,223) encode/decode, t=16 |
| `mixMul` / `mixDiv` / `mixPow` / `mixInv` | GF(256) arithmetic |
| `computeTag` | SipHash-2-4 keyed MAC |
| `PROTO_BLOCK_K/N/PARITY/T` | RS_K=223, RS_N=255, RS_PARITY=32, RS_T=16 |
| `PROTO_TAG_LEN` | MAC length (8 bytes) |
| `s_protoKey` | the real shared key, inlined as a byte array (no `secrets.h`) |

Wire layout per packet: `[counter(4)][len(1)][payload up to 107B][zero pad
to 112][tag(8)]` -> RS-encoded -> `152 bytes` total, inside the frame's
measured ~156-byte real capacity at `RADIO_FIXED_PACKET_LEN=200`.

**`AUTH=TRUE|FALSE`** (Makefile variable, default TRUE) toggles only the
tag+counter half (`#if AUTH_ENABLE` in `protocol.c`) -- the RS correction
stays on either way. `.auth_tag` tracks the last value and forces a clean
rebuild on change (object files don't encode it in their names).

Scoped `__attribute__((optimize("O3")))` on `sealEncode`/`sealDecode` only
-- not a global `-O3`/`-flto`, since the base firmware's RF/CW timing is
real, hand-tuned, hardware-validated code that a blanket optimization
bump could silently perturb.

## Packet Format sheet compliance

A 4-sheet spec (`Packet Format - Sheet{1,2,3,4}.csv`, not in this repo --
ask for them if revisiting this) defines the real on-air formats. Status:

- **Beacon1 / Beacon2** (32B/22B, `Beacon1Packet_t`/`Beacon2Packet_t` in
  `Mission/config.h`): struct + pack logic (`Beacon1_Pack`/`Beacon2_Pack`)
  exist in history but are **not used for TX** -- by design, the satellite
  only downlinks CW1/CW2 (see below), never the Beacon1/2 AX.25 binary
  packets. `BEACON1_HEAD`/`BEACON2_HEAD`/the structs stay defined (and
  `gs_main.c`'s `Print_Decoded_Sheet_Payload` can still decode one if it
  ever shows up), but nothing in `satellite_main.c` builds or sends one.
- **Command** (13B, `CommandPacket_t`, `command_pack` in `gs_main.c`):
  done. SAT-ID=0x53, MCU-ID, 3 opcode bytes (only `CMD_RUN_CAMERA`=0xCC and
  `CMD_DOWNLOAD_FILE`=0x1D have a documented literal opcode_b2/b3 in the
  sheet; everything else zero-fills them -- inventing undocumented values
  would be guessing). No separate counter/MAC in the struct -- the light
  FEC wrap above already adds one to every packet, so duplicating it here
  would be redundant overhead for no extra protection.
- **ACK/NACK** (3B, `AckPacket_t`, `Ack_Pack`/`Satellite_Send_Ack`, now in
  `satellite_main.c`): done. response_type derived from status
  (ACCEPTED/COMPLETED -> 0xAC, else 0xEE) rather than trusting the sheet's
  one inconsistent worked example (its "ACK command completed" row
  literally shows 0xEE). No caller yet (no command-reply path wired up).
- **Camera fragment** (`CameraFragment_t`, `Camera_Fragment_Pack`, now in
  `satellite_main.c`): struct + pack function done, reusing
  `AX25_CalculateCRC` for the CRC-16/X.25 trailer. **Nothing calls it** --
  no camera driver exists in this build.
- **CW1/CW2** (8B bit-packed -> 16 hex digits, `CwBeacon1_t`/`CwBeacon2_t`,
  `Satellite_BuildCw1Hex`/`Cw2Hex`, now in `satellite_main.c`): done,
  replacing the base repo's plain-ASCII CW message ("12.34V 5.67V...")
  with the real BIRDS4-style bit-packed+hex-encoded format, fed into the
  base repo's own (untouched) Morse keyer in `satellite_app.c`. CW1's
  `sol_total_i`/`sol_total_v`/`bat_v`/`bat_i` now copy straight from
  `beacon1_ipc_s`. CW2's `gyro_x/y/z`/`mag_x/y/z` copy straight from
  `beacon2_ipc_s`; its 5 solar-cell shadow/sunshine bits are *derived*
  (nonzero per-panel voltage -> "sunshine") rather than sheet-literal,
  since the sheet gives CW2 a boolean per panel but Beacon2 only measures
  a voltage -- an honest derivation from a real reading, not fabrication.
  Solar-cell order corrected to BIRDS4's own +X/-Y/-Z/+Y/+Z (the sheet's
  own table has a copy-paste "+Y" repeated twice). CW1's 2-bit
  `system_stat`/`mission_status` have no defined mapping from Beacon1's
  generic status bytes (sheet gives neither a value table) -- left zero.

`Satellite_BuildAndSendTelemetry` (`satellite_main.c`) is the one-stop
function that takes the sheet-exact `beacon1_ipc_s`/`beacon2_ipc_s` IPC
payloads and copies every real field straight onto the CW1/2 structs,
filling in the two CW hex strings -- called from `satellite_main.c`'s own
main loop. It does NOT build or send Beacon1/Beacon2 AX.25 packets --
CW1/CW2 (Morse) are the satellite's only downlink telemetry.

## Known gaps (don't assume these are solved)

- **RF frequencies were wrong on both ends -- fixed.** `gs_main.c`'s
  `GroundStationProfile` hardcoded both `txFrequency` and `rxFrequency`
  to 868 MHz (EU ISM band, not this project's UHF ham band, and not even
  different TX/RX values). Separately, `satellite_app.h`'s
  `SAT_CFG_DEFAULT_TX_FREQ_HZ`/`_RX_FREQ_HZ` were assigned
  `DEFAULT_TX_FREQUENCY_HZ`/`DEFAULT_RX_FREQUENCY_HZ` straight, but those
  two constants (`Mission/config.h`) are named from the GS's own side (GS
  TX=437.375/RX=435.000) -- so the satellite needs the OPPOSITE pairing,
  and had it backwards. Both fixed: GS now uses
  `DEFAULT_TX_FREQUENCY_HZ`/`_RX_FREQUENCY_HZ` directly (437.375/435.000),
  satellite now uses them swapped (435.000 TX / 437.375 RX). Matters for
  any real-RF test -- this would have put both ends on the wrong
  frequency, or the same frequency as each other, not two that talk.
- **CPU1/NuttX command dispatch is unverified.** The satellite's CPU2 side
  just forwards received command bytes raw to CPU1 over IPCC
  (`M0_Check_And_Forward_Command`) -- whatever NuttX does with them lives
  in `apps/`, never explored this project. The GS can now send correctly-
  framed Command packets with the full opcode set, but whether CPU1 acts
  on anything beyond the base repo's original 5 opcodes is unknown.
- **No fabricated telemetry defaults** -- this was a real bug, fixed: the
  base repo seeded `b1_data`/`b2_data` with hardcoded plausible-looking
  values (4.00V, 25.0°C, 1.00A...) that would transmit forever if CPU1
  never sent real data. Now zero-initialized (`struct beacon1_ipc_s`/
  `beacon2_ipc_s`) with `b1_valid`/`b2_valid` flags that only become true
  once a real IPC packet arrives.
- **Ring buffer is sheet-exact now, but CPU1 still has to fill it.** The
  IPC wire format (`beacon1_ipc_s`/`beacon2_ipc_s`, `ipc/Sring_buffer.h`)
  used to carry only 5 generic scaled-int16 values per beacon, which is
  why most Beacon1/2 fields were honestly zeroed -- there was nowhere for
  them to come from even if CPU1 measured them. The struct is now a
  field-for-field mirror of Beacon1Packet_t/Beacon2Packet_t, so every
  sheet field has a slot; whether it carries a *real* sensor reading is
  entirely down to whatever CPU1/NuttX code (`apps/`, unexplored this
  project) calls `live_telem_update_b1`/`_b2`. No such caller exists yet.
- **`MASTER.md` is stale** -- still describes the old FX.25/cubus_ax25
  architecture (pre-swap) and links to docs that no longer exist. Not
  rewritten yet; ask before trusting it for anything architecture-related.
- **Beacon2's richer fields have no sensor source.** Per-panel solar
  voltage/current and gyro/magnetometer would need CPU1-side ADCS work
  this project didn't touch.
- **GS-side sheet-aware RX decode.** `gs_main.c`'s `Process_Received_Frame`
  used to dump every decoded payload as raw ASCII (`c>=32&&c<=126 ? c :
  '.'`), which is wrong for Beacon1/Beacon2/ACK/Command -- those are
  binary structs, not text, so they printed as mostly dots.
  `Print_Decoded_Sheet_Payload` now checks the header byte (`BEACON1_HEAD`/
  `BEACON2_HEAD`/`ACK_RESPONSE_TYPE_*`/`SAT_ID_S2S2`) and prints real
  field values (verifying Beacon1/2's CHK_CRC), falling back to the old
  ASCII dump only for payloads that don't match a known layout.

## Testing pattern

No on-target test runner -- verify `protocol.c` changes host-side first:

```
clang -std=c11 -Wall -O2 -IMission -Iprotocol -Iprotocol/ax25 \
  -Iprotocol/g3ruh -IInc -o /tmp/roundtrip \
  Mission/protocol.c protocol/ax25/ax25.c protocol/ax25/crc16.c \
  protocol/g3ruh/g3ruh.c your_test.c
```

Build a real frame with `Protocol_CreatePacket`, decode with
`Protocol_ExtractFrame`+`Protocol_VerifyPacket`+`Protocol_DecodePayload`,
and check: clean round-trip, exact-replay rejection, correction up to
t=16 injected byte errors, correct refusal past t=16. Only flash to real
hardware after that passes.
