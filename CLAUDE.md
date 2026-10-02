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
- `JC2/Main_code/Main_Satellite/` -- satellite firmware (`satellite_main.c`
  = mission loop, `satellite_app.c` = radio/CW/beacon/telemetry functions).
- `JC2/Main_code/Main_GS/gs_main.c` -- ground-station firmware (UART
  console, command TX, telemetry RX/display).
- `JC2/ipc/Sring_buffer.h` -- CPU1<->CPU2 shared-SRAM2 ring buffers
  (`tx_packet_s` telemetry, `rx_command_s` commands). Only carries 5
  generic scaled-int16 values per beacon -- see "Known gaps" below.
- `JC2/tools/` -- our own Python tooling (decoder, GUIs, test suites,
  fault injection, mission control dashboard). Not part of the base repo.
- `nuttx/`, `apps/` -- git submodules, CPU1/NuttX side. **Not explored or
  modified this project** -- command dispatch semantics on CPU1 are
  unknown/unverified (see "Known gaps").

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

- **Beacon1** (32B, `Beacon1Packet_t` in `Mission/config.h`,
  `Beacon1_Pack`/`Satellite_Send_Beacon1` in `satellite_app.c`): done.
  HEAD=0xB1, CHK_CRC=CRC-8 poly 0x07. Only `tim_hour`/`bat_v`/`sol_tot_v`/
  `ant_p_t`/`bat_temp`/`bpb_t`/`bat_i` have a real data source (from the
  5-value IPC packets) -- everything else is honestly zeroed, not
  fabricated, until real sensors exist.
- **Beacon2** (22B, `Beacon2Packet_t`): done, same framing (HEAD=0xB2).
  Only `power_stat` has a real source (packed from the IPC flag bytes) --
  per-panel solar V/I and gyro/magnetometer have no data source at all in
  this build (ADCS/per-panel telemetry isn't wired up CPU1-side).
- **Command** (13B, `CommandPacket_t`, `command_pack` in `gs_main.c`):
  done. SAT-ID=0x53, MCU-ID, 3 opcode bytes (only `CMD_RUN_CAMERA`=0xCC and
  `CMD_DOWNLOAD_FILE`=0x1D have a documented literal opcode_b2/b3 in the
  sheet; everything else zero-fills them -- inventing undocumented values
  would be guessing). No separate counter/MAC in the struct -- the FEC
  wrap above already adds one to every packet, so duplicating it here
  would be redundant overhead for no extra protection.
- **ACK/NACK** (3B, `AckPacket_t`, `Ack_Pack`/`Satellite_Send_Ack`): done.
  response_type derived from status (ACCEPTED/COMPLETED -> 0xAC, else
  0xEE) rather than trusting the sheet's one inconsistent worked example
  (its "ACK command completed" row literally shows 0xEE).
- **Camera fragment** (`CameraFragment_t`, `Camera_Fragment_Pack`): struct
  + pack function done, reusing `AX25_CalculateCRC` for the CRC-16/X.25
  trailer. **Nothing calls it** -- no camera driver exists in this build.
- **CW1/CW2** (8B bit-packed -> 16 hex digits, `CwBeacon1_t`/`CwBeacon2_t`,
  `Satellite_BuildCw1Hex`/`Cw2Hex`): done, replacing the base repo's
  plain-ASCII CW message ("12.34V 5.67V...") with the real BIRDS4-style
  bit-packed+hex-encoded format, fed into the base repo's own (untouched)
  Morse keyer. Solar-cell order corrected to BIRDS4's own +X/-Y/-Z/+Y/+Z
  (the sheet's own table has a copy-paste "+Y" repeated twice).

`Satellite_BuildAndSendTelemetry` (`satellite_app.c`) is the one-stop
function that maps the 5-value IPC telemetry onto both Beacon1/2 and
CW1/2 and sends everything -- called from `satellite_main.c`'s main loop.

## Known gaps (don't assume these are solved)

- **CPU1/NuttX command dispatch is unverified.** The satellite's CPU2 side
  just forwards received command bytes raw to CPU1 over IPCC
  (`M0_Check_And_Forward_Command`) -- whatever NuttX does with them lives
  in `apps/`, never explored this project. The GS can now send correctly-
  framed Command packets with the full opcode set, but whether CPU1 acts
  on anything beyond the base repo's original 5 opcodes is unknown.
- **No fabricated telemetry defaults** -- this was a real bug, fixed: the
  base repo seeded `b1_data`/`b2_data` with hardcoded plausible-looking
  values (4.00V, 25.0°C, 1.00A...) that would transmit forever if CPU1
  never sent real data. Now zero-initialized with `b1_valid`/`b2_valid`
  flags that only become true once a real IPC packet arrives.
- **`MASTER.md` is stale** -- still describes the old FX.25/cubus_ax25
  architecture (pre-swap) and links to docs that no longer exist. Not
  rewritten yet; ask before trusting it for anything architecture-related.
- **Beacon2's richer fields have no sensor source.** Per-panel solar
  voltage/current and gyro/magnetometer would need CPU1-side ADCS work
  this project didn't touch.
- **`gs_main.c`'s `Main_code/Main_GS/GS_app.c`** is dead code (not in any
  Makefile `C_SRCS` list) -- the real, built GS logic is in `gs_main.c`.

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
