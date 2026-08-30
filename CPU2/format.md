# Custom Packet Protocol — Packet Format Reference

Matches the tested implementation in `cubus_proto.c` / `cubus_proto.py`. Not AX.25 — a custom framing built for a 500–700 byte payload target with concatenated Reed-Solomon FEC and selective retransmission.

## 1. On-Air Frame Layout

```
Byte:    0    1    2    3        4  ...  768        769  770  771  772
       ┌────────────┬──────────────────────────────┬──────────────────┐
       │  ASM (4B)  │  Interleaved RS body (765B)   │   CRC32 (4B)     │
       └────────────┴──────────────────────────────┴──────────────────┘

Total on-air frame size: 773 bytes
```

The RS body is 3 interleaved RS(255,223) codewords (255 × 3 = 765 bytes). Before RS encoding, the 765 bytes of protected data are a 669-byte **info section**:

```
Info section (669 bytes, RS-protected as a whole):
┌──────────────┬───────────────────────────────┐
│ Header (10B) │  Payload (up to 659B, padded)  │
└──────────────┴───────────────────────────────┘
```

The header is inside the FEC-protected zone (not sent raw), so header corruption gets corrected the same way payload corruption does.

## 2. Attached Sync Marker (ASM)

| Bytes | Value |
|---|---|
| 4 | `0x1A 0xCF 0xFC 0x1D` |

Fixed pattern the receiver searches for to find frame boundaries before attempting to decode.

## 3. Header (10 bytes, inside the RS-protected info section)

| Offset | Field | Size | Notes |
|---|---|---|---|
| 0 | Version (high nibble) + Type (low nibble) | 1B | version=1 currently; type = one of §5 |
| 1 | Source ID | 1B | |
| 2 | Destination ID | 1B | |
| 3–4 | Sequence number | 2B | big-endian uint16, wraps at 65536, scoped per session |
| 5–6 | Payload length | 2B | big-endian uint16, actual payload bytes (excludes zero-padding) |
| 7 | Flags | 1B | bitfield, see §6 |
| 8–9 | Session ID | 2B | big-endian uint16, groups packets from one pass/transaction |

## 4. Payload

- **Max usable payload: 659 bytes** (669-byte info section − 10-byte header)
- Shorter payloads are zero-padded to fill the 669-byte info section; `payload_len` in the header tells the receiver where real data ends
- If more room is needed, increasing `NUM_RS_BLOCKS` from 3 to 4 raises the ceiling to 892 bytes info capacity (mirrors EIRSAT-1's frame size) at the cost of a larger on-air frame

## 5. Packet Types

| Value | Name | Use |
|---|---|---|
| `0x0` | `TYPE_TELEMETRY` | Satellite → ground data |
| `0x1` | `TYPE_TELECOMMAND` | Ground → satellite command |
| `0x2` | `TYPE_ACK` | Acknowledgment |
| `0x3` | `TYPE_GAP_REPORT` | Ground → satellite: list of missing sequence numbers |

## 6. Flags (bitfield)

| Bit | Name | Meaning |
|---|---|---|
| `0x01` | `FLAG_ACK_REQUESTED` | Sender wants explicit ack |
| `0x02` | `FLAG_LAST_IN_BURST` | Last packet in this transmission burst |
| `0x04` | `FLAG_PRIORITY` | High-priority — send redundant copies rather than waiting on a gap report |
| `0x08` | `FLAG_RETRANSMIT` | This packet is a retransmission (not first send) |

## 7. Forward Error Correction

- **Scheme:** 3× interleaved RS(255,223), depth-3 interleaving
- **Per-block correction:** up to 16 byte errors out of every 255
- **Whole-frame correction:** up to 48 byte errors total, spread across the frame (interleaving means a physical-layer burst error gets distributed across all 3 blocks instead of overwhelming one)
- Tested against 200 random trials at exactly the 16-error/block limit — 100% exact recovery; correctly detects and rejects frames beyond that limit

## 8. Integrity Check

- **CRC32** (IEEE 802.3 / zlib polynomial `0xEDB88320`), computed over the 669-byte info section
- Checked *after* RS correction, as a backstop against the rare case where RS "corrects" a block to the wrong (but valid-looking) codeword — if CRC fails, the frame is rejected outright rather than trusted

## 9. Reliability Layer (Selective Retransmission)

Not part of the frame itself — built on top of `seq_num` + `session_id`:

1. Ground station tracks received sequence numbers per session (bitset, no dynamic allocation, `gap_tracker_t` in `s2s2com.h`)
2. At end of pass (or mid-pass), it computes a **gap report**: sequence numbers in `[lowest_seen, highest_seen]` that were never received. Deliberately *not* `[0, highest_seen]` — `seq_num` is one counter shared across every message type (beacons, ACKs, telecommands, telemetry) on both boards, so a telemetry burst almost never starts at 0; scanning from 0 would falsely flag every sequence number used by unrelated prior traffic as "missing" (`gap_tracker_report`, `s2s2com.h`).
3. Ground uplinks the gap report as a `TYPE_GAP_REPORT` packet, addressed to the satellite
4. Satellite retransmits only those specific sequence numbers, looked up in its `tx_history` onboard ring buffer (`tx_history.h`/`.c`) — not the whole session. Entries resent this way carry `FLAG_RETRANSMIT` set (see §6) and reuse their *original* `seq_num`, so the ground station's gap tracker marks the right entry received rather than treating the resend as new traffic.
5. The `TYPE_TELECOMMAND` opcode `CMD_REQUEST_RETRANSMIT` (see format_commands.md) is a coarser sibling of step 3-4: it carries no specific sequence list, so instead of a targeted lookup the satellite just replays everything still in the `tx_history` buffer. Useful when the ground station wants a resend but hasn't (yet) computed a gap report.

This means a dropped packet gets recovered without re-sending everything, and a corrupted-but-within-FEC-limits packet needs no retransmission at all.

**Buffer limits (by design, not a bug):** `tx_history` holds `TX_HISTORY_LEN` (16) entries of up to `TX_HISTORY_MAX_PAYLOAD` (128) bytes each on the satellite — deliberately far below `MAX_PAYLOAD_LEN` (659) to fit the M0+ core's 32K total SRAM budget alongside everything else it needs (reassembler, radio driver state, stacks). A message larger than 128 bytes, or one old enough to have been evicted by newer traffic (ring buffer, oldest overwritten first), simply isn't retransmittable — the satellite replies `ACK_STATUS_DATA_UNAVAILABLE` for that one, the same reply it always gave before this buffer existed, so callers don't need new error handling for the "not available" case. Real telemetry payloads sent by this firmware today (beacon text, burst telemetry strings) are well under the 128-byte cap.

## 10. Size Summary

| Quantity | Value |
|---|---|
| Max payload | 659 bytes |
| Header | 10 bytes |
| Info section (header + payload, pre-FEC) | 669 bytes |
| RS parity overhead | 96 bytes (32 × 3 blocks) |
| ASM + CRC overhead | 8 bytes |
| **Total on-air frame** | **773 bytes** |
| Time on air @ 9600 bps | ≈ 0.64 s/frame |
| Time on air @ 1200 bps | ≈ 5.15 s/frame |

## 11. Reference Implementation

- `rs.h` / `rs.c` — RS(255,223) codec (Berlekamp-Massey, Chien search, Forney)
- `cubus_proto.h` / `cubus_proto.c` — framing, header pack/unpack, CRC32, interleaving, gap tracking
- `cubus_packet_protocol.py` — equivalent Python reference implementation
- `s2s2_link.h` — link-budget-driven baud/FEC selection (placeholders pending real S2S2 link budget numbers)

Both C and Python implementations produce byte-identical frames for the same input and were tested against the same channel-simulation scenario (16-errors-per-block recovery, full packet drop → gap report).