# S2S2 Standard Command Set (converted from `packet_format_AX.25.json`)

Source: `packet_format_AX.25.json` (from `Packet_Format.xlsx`), the legacy
AX.25/G3RUH packet-format spec. This document is the S2S2/cubus_proto
equivalent -- same command/response *meaning*, carried in the new frame
format instead of AX.25 UI frames. Implementation: `core/Inc/s2s2_commands.h`
+ `core/Src/s2s2_commands.c`.

## Why the AX.25 layout doesn't carry over as-is

AX.25 needed Destination/Source callsign-SSID pairs, a Control/PID byte
pair, and its own CRC-16 because the frame itself carried no addressing or
integrity beyond that. `cubus_proto` already has all of it in the 10-byte
header (`src_id`/`dst_id`/`seq_num`/`session_id`) plus RS(255,223) FEC +
CRC32 over the whole frame + a hardware CRC16 per radio chunk. So the
conversion drops:

- Start/End flags (`0x7E`), Control Field (`0x03`), PID Field (`0xF0`) --
  framing the SX126x's hardware sync-word + CRC + `cubus_proto`'s own ASM
  already does.
- 7-byte callsign-SSID addresses -- replaced by the 1-byte `src_id`/`dst_id`
  already in the header (`NODE_ID_GROUND` / `NODE_ID_SATELLITE`).
- The 3-byte per-subsystem "magic" opcode sequences (e.g. `CC 5E BD` for
  Run Camera) -- replaced by one canonical opcode byte per command
  (`cubus_cmd_t`).
- The `0xAC` / `0xEE` ACK-vs-NACK marker byte -- redundant with
  `TYPE_ACK` already being a distinct packet type; success/failure is
  conveyed by the status code alone.
- Per-sub-packet CRC-16/X.25 (Camera Fragment) -- redundant with the
  frame-level CRC32 + RS FEC.
- Sub-byte bitfield packing in the beacon payloads (`TYPE` at 4 bits, etc.)
  -- `cubus_proto`'s 659-byte payload budget makes byte-alignment cheap;
  keeping it byte-aligned removes a class of packing bugs for one spare
  nibble.

Everything else -- opcodes, subsystem IDs, mission/data identifiers, ACK/NACK
status codes, and beacon field layouts -- is preserved.

## Command packet (`TYPE_TELECOMMAND`)

Payload = `cubus_command_t`, 9 bytes, big-endian multi-byte fields:

| Byte(s) | Field       | Notes |
|---------|-------------|-------|
| 0       | opcode      | `cubus_cmd_t` |
| 1       | subsystem   | `cubus_subsystem_t`, 0 if n/a |
| 2       | data_id     | `cubus_data_id_t`, 0 if n/a |
| 3-6     | address     | file/memory address (DOWNLOAD_FILE only) |
| 7-8     | num_packets | fragment count requested |

Set `FLAG_ACK_REQUESTED` in the header to get a `TYPE_ACK` reply.

### Standard opcodes (`cubus_cmd_t`)

| Opcode | Name | Was (AX.25 sheet) |
|--------|------|--------------------|
| 0x00 | `CMD_NONE` | -- |
| 0x01 | `CMD_REQUEST_BURST` | (S2S2-specific, not in the AX.25 sheet) |
| 0x02 | `CMD_HEARTBEAT` | (S2S2-specific) |
| 0x03 | `CMD_ABORT_MISSION` | -- |
| 0x10 | `CMD_RUN_CAMERA` | Camera Command (`53 04 CC 5E BD ...`) |
| 0x11 | `CMD_DOWNLOAD_FILE` | OBC Download (`53 01 1D D2 F5 ...`) |
| 0x12 | `CMD_GET_HEALTH` | HEALTH SAT (`53 01 1D D1 F2 ...`) |
| 0x13 | `CMD_GET_HOUSEKEEPING` | House Keeping (`53 01 1D D1 F8 ...`) |
| 0x14 | `CMD_GET_BEACON1` | Beacon 1 request |
| 0x15 | `CMD_GET_BEACON2` | Beacon 2 request |
| 0x16 | `CMD_GET_ADCS` | -- |
| 0x17 | `CMD_DIGIPEATER` | Digipeater mission |
| 0x18 | `CMD_GET_SAT_LOG` | Satellite log |
| 0x19 | `CMD_REQUEST_RETRANSMIT` | (ties into the S2S2 gap tracker; not in the AX.25 sheet) |

### Subsystem IDs (`cubus_subsystem_t`, unchanged from the MCU ID table)

`SUBSYS_OBC=1, SUBSYS_COM=2, SUBSYS_ADCS=3, SUBSYS_CAM=4, SUBSYS_EPDM=5`

### Mission/data identifiers (`cubus_data_id_t`, unchanged from Byte 2 table)

`DATA_ID_HOUSEKEEPING=0x01, BEACON1=0x02, BEACON2=0x03, ADCS=0x04, CAMERA_RGB=0x05, CAMERA_NIR=0x06, NDWI=0x07, EPDM=0x08, DIGIPEATER=0x09, SAT_LOG=0x0A, GENERAL=0xFF`

## ACK/NACK packet (`TYPE_ACK`)

Payload = `cubus_ack_t`, 4 bytes: `[status][data_id][acked_seq (2, BE)]`.
`acked_seq` echoes the `seq_num` of the telecommand being acknowledged, so
the ground station can match replies to requests.

### Status codes (`cubus_ack_status_t`, unchanged from Byte 1 table)

`0x00` accepted, `0x01` completed, `0x02` packet error, `0x03` CRC error,
`0x04` incomplete packet, `0x05` data missing, `0x06` data unavailable,
`0x07` satellite busy, `0x08` mission inactive, `0x09` invalid command,
`0x0A` memory/file error, `0x0B` transmission failed.
`ack_status_str()` gives the human-readable string for logging.

## Beacon 1 -- power/battery/thermal/system health (`data_id = BEACON1`)

`beacon1_t`, 30 bytes, all fields byte-aligned (vs. the original's
`TYPE`-at-4-bits packing). Same fields as the AX.25 sheet's Beacon 1 layout
minus `HEAD` (redundant with the frame header) and `CHK_CRC` (redundant
with frame CRC32): `type, tim_day, tim_hour, bat_v, bat_i, bat_temp, raw_v,
raw_i, sol_tot_v, sol_tot_i, unreg_i, main_3v3_i, msn_3v3_i, v5_i,
adc_3v32_i, temp1..temp5, bpb_t, ant_p_t, obc_com_t, system_stat, mcu_stat,
adc_stat`.

## Beacon 2 -- solar panel & ADCS (`data_id = BEACON2`)

`beacon2_t`, 25 bytes: `tim_day, sol_p1_v..sol_p5_v, sol_p1_i..sol_p5_i,
gyro_x, gyro_y, gyro_z, mag_x, mag_y, mag_z, power_stat`.

## Camera fragment (`data_id = CAMERA_RGB` / `CAMERA_NIR` / `NDWI`)

`camera_fragment_t`: `image_id(1), fragment_index(2), total_fragments(2),
image_type(1), ai_score(1), image_data(variable, up to
CAMERA_FRAGMENT_MAX_DATA=512)`. No per-fragment CRC-16 (redundant with the
frame-level FEC/CRC32); `cubus_proto`'s larger payload budget also means
fragments can carry more image data per frame than AX.25's ~128-190 bytes.

## Firmware wiring

- Satellite (`ipcc_m0plus_main.c`, `STATE_PROCESS_COMMAND`): unpacks the
  command, dispatches on `cmd.opcode`, and replies with a `TYPE_ACK` frame
  whenever `FLAG_ACK_REQUESTED` is set. `CMD_GET_HEALTH` /
  `_HOUSEKEEPING` / `_BEACON1` / `_BEACON2` / `_ADCS` / `_SAT_LOG` reply
  `ACK_STATUS_DATA_UNAVAILABLE` -- the sensor/file subsystems aren't wired
  up in this firmware yet, so it reports that honestly instead of
  fabricating telemetry. `CMD_RUN_CAMERA` / `_DOWNLOAD_FILE` /
  `_DIGIPEATER` reply `ACK_STATUS_MISSION_INACTIVE` for the same reason.
  `CMD_REQUEST_RETRANSMIT` replies `ACK_STATUS_ACCEPTED` and resends every
  message currently held in the satellite's `tx_history` ring buffer
  (`ACK_STATUS_DATA_UNAVAILABLE` only if the buffer is empty) -- see the
  architecture README section 9 ("Reliability: Satellite-Side Retransmit
  Buffer"). A received `TYPE_GAP_REPORT` (not a telecommand -- see format.md
  section 9) is handled the same way but resends only the specific missing
  sequence numbers it names, looked up in the same buffer.
- Ground station (`gs_main.c`): the interactive UART CLI has one command
  per opcode (`COMMAND`, `HEARTBEAT`, `HEALTH`, `HOUSEKEEPING`, `BEACON1`,
  `BEACON2`, `ADCS`, `CAMERA`, `DOWNLOAD`, `SATLOG`, `DIGIPEATER`,
  `RETRANSMIT`, `ABORT`), and decodes/prints any `TYPE_ACK` reply with its
  status string.

## Verification

`core/Src/test_commands.c` (host-buildable, no HAL deps) round-trips every
struct above through its pack/unpack functions, including truncated-buffer
rejection and the camera fragment's oversize-reject path:

```
clang -std=c11 -Wall -Wextra -O2 -I core/Inc -o /tmp/test_commands core/Src/s2s2_commands.c core/Src/test_commands.c
/tmp/test_commands
```

All cases pass.
