# Digipeater Mission (DPM) Architecture

Design for `CMD_DIGIPEATER` — currently a stub in `ipcc_m0plus_main.c`
(`case CMD_RUN_CAMERA: case CMD_DOWNLOAD_FILE: case CMD_DIGIPEATER:` all
fall into "mission not implemented on this build", `ACK_STATUS_MISSION_INACTIVE`).
This document is the plan for actually implementing it. Nothing here is
built yet — this is the design to build against.

## 1. What "digipeater" means for this satellite

`s2s2com.h` already says it: S2S2 is "a 1U amateur UHF CubeSat digipeater
(same class as S2S)". That's a real, well-known amateur-satellite service —
any ham on the ground uplinks a standard AX.25 UI frame with the satellite's
callsign in the digipeater path, the satellite repeats it back down on the
same or a different frequency, other hams receive it. This is **not** the
same thing as the S2S2 command/telemetry link. It is a distinct feature that
happens to share the same radio and the same AX.25/FX.25 framing.

This distinction drives the whole design, because of one fact about the
current RX path:

```
STATE_PROCESS_COMMAND:
    cubus_ax25_accept_frame(&s_reassembler, frame_copy)
```

`cubus_ax25_accept_frame()` does FX.25 tag/RS validation, AX.25 FCS
validation, **and** requires the Info field to parse as a well-formed
12-byte S2S2 fragment header (version/type/src_id/dst_id/seq/session/flags/
frag_index/frag_total/frag_len — see `cubus_ax25.h`). A real ham's uplink
packet has none of that; its Info field is arbitrary text/APRS/whatever.
It will hit `-1` ("frame rejected ... malformed") every time.

**So digipeating cannot be bolted onto the existing RX path.** It needs its
own, lower-level frame path that stops at "valid FX.25 RS decode + valid
AX.25 FCS" and goes no further into S2S2-specific parsing. The S2S2
command/telemetry link and the digipeater service both sit on top of
`fx25.c`/`ax25.c`, but they fork *below* `cubus_ax25.c`, not above it.

```
                         FX.25 RS(255,128) decode (fx25_decode)
                                       |
                         AX.25 FCS + address parse (ax25_parse_ui_frame)
                                       |
                    +------------------+------------------+
                    |                                      |
         Info field = S2S2 fragment              Info field = arbitrary
         header (12-byte magic fields               user traffic
         parse cleanly, dst callsign                (destination is NOT
         == our link's fixed dest/src)              the fixed S2S2 link
                    |                                addresses, or a
             cubus_ax25_accept_frame                 digipeater address
             (existing S2S2 path,                    field lists our
              unchanged)                             callsign, H-bit=0)
                                                             |
                                                    Digipeater Mission path
                                                    (new, this document)
```

## 2. Why the existing 2-address `ax25.h` model isn't enough

`ax25.h` today only builds/parses `dest + src` (2 addresses, no path field)
— correct for S2S2's own point-to-point link, useless for digipeating,
where the frame looks like:

```
[dest][src][digi 1][digi 2]...[digi N]  control  pid  info  fcs
```

...and up to 8 total address fields are allowed by the AX.25 spec (dest +
src + up to 6 digipeaters). A real digipeater does **not** just check "is
this addressed to me and can I forward it" — it walks the digipeater
address list looking for its own callsign with the **H-bit** (bit 7 of that
address's SSID byte, the "has-been-repeated" flag — distinct from the C/R
command bit used on dest/src) still clear. That's the actual repeat
condition, not a match on the destination field.

### 2.1 New parsing needed (extends `ax25.h`, does not change it)

Add, in a new `ax25_digi.h`/`ax25_digi.c` pair (kept separate from
`ax25.h` deliberately — S2S2's own link never uses more than 2 addresses,
so the existing `ax25_build_ui_frame`/`ax25_parse_ui_frame` stay exactly as
they are for the S2S2 path):

```c
#define AX25_MAX_DIGIS        6   /* AX.25 spec max: 8 total addr fields - dest - src */
#define AX25_MAX_ADDR_FIELDS  (2 + AX25_MAX_DIGIS)

typedef struct {
    ax25_addr_t addr;
    bool        has_been_repeated;   /* H-bit, digipeater addresses only */
} ax25_digi_addr_t;

typedef struct {
    ax25_addr_t      dest;
    ax25_addr_t      src;
    ax25_digi_addr_t digis[AX25_MAX_DIGIS];
    uint8_t          num_digis;
    uint8_t          control;
    uint8_t          pid;
    const uint8_t   *info;
    uint16_t         info_len;
} ax25_full_frame_t;

/* Parses a variable-address-count frame (2..8 address fields). Does NOT
   assume the fixed 2-address S2S2 shape - unlike ax25_parse_ui_frame. */
int ax25_digi_parse(const uint8_t *frame, uint16_t frame_len, ax25_full_frame_t *out);

/* Re-serializes out (after mutating exactly one digi's has_been_repeated
   flag - see 3.3) back into a transmittable frame + recomputed FCS. */
int ax25_digi_rebuild(const ax25_full_frame_t *f, uint8_t *out, uint16_t max_out, uint16_t *len_out);
```

This is additive: `AX25_MAX_FRAME_LEN` (currently sized for 2 addresses,
80-byte info) already has headroom eaten by S2S2's 12-byte fragment header,
which digipeated frames don't carry — a foreign frame with up to 6 extra
7-byte digi fields (42 bytes) and a shorter info field still comfortably
fits inside one 128-byte FX.25 tag-0x06 data field. No FX.25/RS parameter
changes needed.

## 3. Data flow (satellite side)

```
 M0+ radio RX
      |
 RxDone -> fx25_decode() into raw AX.25 bytes  (reuse fx25.c as-is)
      |
 ax25_digi_parse()  -- fails? drop silently, same as any other bad frame.
      |
 Is DPM mission active?  ---- no ----> not our packet, ignore (mission
      |                                inactive == satellite is plain S2S2
      yes                              node only, per CMD_ABORT_MISSION)
      |
 Duplicate check (digi_dup_table) ---- hit ----> drop, log "DUP"
      |
      miss
      |
 Walk f.digis[]: find first entry where
   addr == AX25_SATELLITE_CALLSIGN/SSID AND has_been_repeated == false
      |
   not found --------------------------------> not for us, drop
      |
   found at index i
      |
 Set f.digis[i].has_been_repeated = true      (mark OUR hop used, leave
      |                                        every other digi entry
      |                                        exactly as received --
      |                                        that's what lets a chain
      |                                        of real digipeaters work)
 ax25_digi_rebuild() -> recompute FCS
      |
 fx25_encode() -> new 168-byte FX.25 frame     (reuse fx25.c as-is)
      |
 Record in digi_dup_table (so our OWN retransmit doesn't loop back through
      |                     us again if we hear our own downlink, and so a
      |                     second copy of the same uplink within the TTL
      |                     window is suppressed)
      |
 RadioApp_Send()  (same call TX/beacon/ack already use)
      |
 Log line over UART2 (uart2_printf) - src/dest/digi-hop/rssi/snr/len.
 (Flash-backed persistent logging is a real dependency, not built yet --
 see 6. Not Implemented. Do not fabricate a flash write here.)
      |
 RadioApp_ResumeRx()
```

Nothing here needs cross-core IPCC signaling: unlike the old V1.4
(deleted) two-project split, this firmware's whole application lives in
`ipcc_m0plus_main.c` — there is no separate M4 application image in this
repo to notify. "Log to flash" in the legacy diagram becomes "log over
UART2" today, same as every other event in this firmware.

## 4. Data structures

Sized for the WL55's limited RAM — small, fixed, static allocation, no
malloc, matching every other buffer in this file (`s_tx_history`,
`s_reassembler`, etc.).

```c
/* One in-flight digipeat operation. Only one at a time needed - this
   radio is half-duplex and DPM reuses the existing single rx_chunk_buffer
   / STATE_PROCESS_COMMAND pump, it does not need its own ring of pending
   frames the way tx_history does for retransmit. */
typedef struct {
    ax25_full_frame_t frame;
    int16_t  rx_rssi;
    int8_t   rx_snr;
    uint32_t rx_time_ms;
} digi_descriptor_t;

static digi_descriptor_t s_digi_rx;             /* decoded inbound frame, scratch */
static uint8_t           s_digi_tx_buf[FX25_FRAME_LEN]; /* re-encoded outbound frame */

/* Duplicate suppression. Classic APRS-digipeater approach: hash the parts
   of the frame that identify "the same transmission", keep a short TTL
   (a few seconds - long enough to catch a single uplink's normal fx.25
   redundancy/retries, short enough that legitimately repeated traffic
   later in the pass isn't blocked forever). */
#define DIGI_DUP_TABLE_LEN   16   /* mirrors TX_HISTORY_LEN's sizing logic */
#define DIGI_DUP_TTL_MS      8000

typedef struct {
    bool     used;
    uint32_t hash;        /* src addr+ssid, dest addr+ssid, control, pid, info_len, FCS */
    uint32_t seen_at_ms;
} digi_dup_entry_t;

typedef struct {
    digi_dup_entry_t entries[DIGI_DUP_TABLE_LEN];
    int              next_slot;   /* ring cursor, same eviction pattern as tx_history */
} digi_dup_table_t;

static digi_dup_table_t s_digi_dup;

static bool s_digipeater_enabled = false;   /* CMD_DIGIPEATER / CMD_ABORT_MISSION toggle */
```

`hash` is the frame's AX.25 FCS (already computed, already a 16-bit CRC of
everything meaningful) combined with `info_len` — cheap, no extra CRC pass
needed, good enough for an 8-second window on a link this slow (9600 bps).

## 5. Mission state integration

`CMD_RUN_CAMERA` / `CMD_DOWNLOAD_FILE` / `CMD_DIGIPEATER` were grouped as
one stub because they're all "missions" in the GS CLI (`gs_main.c`'s
`MISSION >` menu, `CMD_ABORT_MISSION` stops whichever is active). DPM
should follow that same shape rather than being a standalone always-on
feature:

```c
case CMD_DIGIPEATER:
    s_digipeater_enabled = true;
    uart2_puts("CMD_DIGIPEATER: digipeater mission ACTIVE\r\n");
    if (want_ack) Send_Ack(ACK_STATUS_ACCEPTED, cmd.data_id, cmd_seq);
    app_state = STATE_IDLE_RX;
    break;

case CMD_ABORT_MISSION:
    s_digipeater_enabled = false;   /* was: no-op besides the ACK */
    uart2_puts("CMD_ABORT_MISSION\r\n");
    if (want_ack) Send_Ack(ACK_STATUS_COMPLETED, cmd_seq);
    app_state = STATE_IDLE_RX;
    break;
```

`STATE_PROCESS_COMMAND` needs the fork from section 1 added right where it
currently calls `cubus_ax25_accept_frame()` unconditionally:

```c
int accept_rc = cubus_ax25_accept_frame(&s_reassembler, frame_copy);
if (accept_rc < 0 && s_digipeater_enabled) {
    /* Not an S2S2 fragment for us - if DPM is active, give the digipeater
       path a chance at the same raw bytes before giving up on the frame. */
    Try_Digipeat(frame_copy, rx_chunk_size, rx_last_rssi, rx_last_snr);
}
```

`Try_Digipeat()` is where sections 2–4 live. This ordering matters: S2S2
command/telemetry traffic is always tried first and unaffected by whether
DPM is active, so ground-station control of the satellite never competes
with or gets starved by digipeater traffic.

Beaconing (`Send_Beacon_If_Due`, `BEACON ON/OFF`) is unrelated and keeps
running independently of `s_digipeater_enabled` — the two features don't
interact.

## 6. Not implemented / explicit dependencies

Being direct about what this design assumes exists vs. doesn't, so nothing
here gets presented as done when it isn't:

- **Flash-backed packet log.** `CMD_GET_SAT_LOG` already replies honestly
  with `ACK_STATUS_DATA_UNAVAILABLE` ("requested data not available on this
  build") because there is no flash log store in this firmware yet. DPM's
  UART line-logging is real and buildable today; a persistent flash log of
  every digipeated frame is not, until that store exists. Do not wire
  `CMD_GET_SAT_LOG` to fabricated digipeater stats before that lands.
- **Regulatory/path-length policy.** Real amateur digipeater services
  commonly refuse to repeat frames whose full digi path is already
  ambiguous/oversized, or enforce a max number of digis they'll process
  (this design caps at `AX25_MAX_DIGIS=6`, the AX.25 spec limit, but a
  license-specific policy on top of that — e.g. only ever repeat exactly
  one hop — is a callsign/regulatory decision, not a firmware default to
  invent here).
- **`BOARD_SAT_CUSTOM_PCB`** is still an unfilled stub (`#error` in
  `board_pins.h`) — none of this is board-pin-dependent, so DPM works
  identically once that board lands; noted only for completeness.

## 7. Build flag

Not a `DEBUG_BUILD` feature — digipeating is a real production mission
capability (like `CMD_HEARTBEAT`/`CMD_REQUEST_BURST`), not a bench-test
backdoor like `TX <hex>`/`POWER <dbm>`. Ships in both debug and production
builds, gated only by `s_digipeater_enabled` at runtime via
`CMD_DIGIPEATER`/`CMD_ABORT_MISSION`, exactly like every other mission
opcode already is.

## 8. Verification plan (once built)

1. Host-testable in isolation: `ax25_digi_parse`/`ax25_digi_rebuild`
   round-trip against hand-built multi-address frames (0, 1, 6 digis;
   own-callsign-not-present; own-callsign-present-but-H-bit-already-set),
   alongside the existing `core/Src/test_roundtrip.c` style tests — no
   radio needed.
2. Duplicate suppression: feed the same frame twice inside/outside the
   TTL window, confirm exactly one retransmit for the first case, two for
   the second.
3. Bench test with two SDRs/TNCs standing in for two ham stations: station
   A uplinks a UI frame with path `NEPSAT-0`, confirm station B receives
   an identical frame with `NEPSAT-0`'s H-bit set and every other field
   byte-identical; confirm `CMD_ABORT_MISSION` stops repeats; confirm
   `CMD_DIGIPEATER` re-enables it.
4. Confirm zero interference: run a normal S2S2 burst/retransmit/gap-report
   cycle while DPM is active, confirm behavior is identical to DPM being
   off (the fork in section 5 only engages on frames that already failed
   S2S2 parsing).
