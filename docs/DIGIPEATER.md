# Digipeater

A digipeater listens on one channel and repeats other stations' packets, with the path field rewritten so every packet is
repeated a bounded number of times and never loops. Ours is a **software-defined digipeater**: the framing and the routing
policy are ordinary C running on the microcontroller (the GMSK modem itself is the SX126x), split into layers that know nothing about each other's hardware.

| What | Where |
|---|---|
| Routing engine (pure logic) | `cpu2/common/digipeater/digipeater.{h,c}` |
| Satellite glue (radio in -> engine -> radio out) | `cpu2/satellite/app/satellite_digi.{h,c}`, `Satellite_Send_Raw_Frame()` in `satellite_app.c` |
| Store-and-forward memory (hop budget, schedule, TTL) | `cpu2/common/digipeater/saf.{h,c}` |
| Journal in CPU1 flash + mailbox | `apps/examples/s2s2obc/s2s2obc_saf.c`, `cpu2/satellite/ipc/saf_shared.h` |
| PC tool (status, list, inject a frame) | `tools/testbench/saf_tool.py` |
| Test bench page "Digipeater + store and forward" | `tools/testbench/server.py` (`test_digi`, `/api/digi/*`), `index.html` |
| Re-framing of a received frame | `Protocol_CreateRawPacket()` in `mission/protocol.c` |
| Ground-station test traffic | console command `DIGI SRC>DEST,VIA:text` |
| Settings | TUI / `.s2s2.config` -> `generated/s2s2_config.h` (`S2S2_DIGI_*`) |
| Tests | `tests/host/` (`make test`) and the boot self-test on the chip |

## 1. Research: how digipeaters are built

| Source | What it gives us |
|---|---|
| Dire Wolf `digipeater.c` / `dedupe.c` | order of the checks, WIDEn-N handling, preemptive digipeating, never repeat a modified copy of the original (work on a copy), same-channel copies get the high priority queue; dedupe key = source + destination + info, **not** the path, 16-bit checksum, 25 entries, 30 s, entries expire lazily |
| aprsdigi (n2ygk) | duplicate window 28 s default, a loop check that kills packets already carrying our call, flooding aliases with overlapping footprints cause geometric duplication |
| AX.25 digipeater description (ax25.net) | do not repeat a frame whose path already has our call with the H bit set; replace the unused part of the path by our call with the H bit set; duplicate = checksum over origin, destination and info; hop limit |
| PACSAT digipeater discussion (AMSAT) | space precedent: hash of each packet, not repeated again for 28 s; FalconSat-3 was a full-duplex 9600 bit/s crossband digipeater, the ISS is a simplex one |
| APRS New-N paradigm | `WIDEn-N`: n = reach tier, N = hops left; each repeat decrements N, at 0 the entry is marked used (`*`); trace = the repeater puts its own call in front; `WIDE1-1` is the ground fill-in tier, satellites answer `WIDE2-N`; keep N <= 2 |
| ISS / ARISS practice | channel capacity is the limit (ALOHA, about 20 % load); a footprint covering a continent means few hops and strict duplicate suppression |

Findings that shaped the design:

1. The **path rule** is the same everywhere: look at the first via entry that is not yet used; repeat only if it names us, an alias of ours or an
   allowed `WIDEn-N`; mark it used with our call.
2. **Loop prevention has three layers**: the H bit (our call already used), never repeating our own source, and the duplicate cache.
3. **Duplicate detection must ignore the path** (the same packet reaches us by different routes) and tolerate trailing CR / blanks.
4. A **satellite must be stricter than a ground digipeater**: its footprint is huge, so it limits N, rate-limits globally and per station, and
   answers only the wide tier.
5. A single radio is **half duplex**: the TX queue must not talk over its own receive window or its beacon.
6. Everything must be **bounded**: fixed tables, no heap, injected clock.

## 2. The structure ("perfect software-defined digipeater")

```
        radio hardware (SX126x GMSK transceiver; an SDR in the tests)         <- the only hardware-specific layer
                |  radio packets (200 bytes)                   ^ radio packets (200 bytes)
        +-------v----------+                          +--------+---------+
        | demodulator      |                          | modulator        |    modem layer: bits <-> signal
        | GMSK 4800 bit/s: SX126x + G3RUH scrambler / NRZI in protocol.c  |
        +-------+----------+                          +--------^---------+
                |  HDLC bits                                   |
        +-------v----------+                          +--------+---------+
        | flag sync, de-stuff, FCS check                      | stuff, FCS, flags |   framing layer (protocol.c)
        +-------+----------+                          +--------^---------+
                |  frame body (addresses, control, PID, info)  |
                |                                              |
        +-------v----------------------------------------------+--------+
        |  Digi_Process(frame, now) -> REPEAT + new frame | DROP(reason)   |   policy layer (digipeater.c): pure function of
        |    parse -> UI check -> next unused via -> own source / deny /   |   (frame, time, small state). No hardware.
        |    loop -> rule match (call, alias, WIDEn-N, preempt) ->         |
        |    duplicate cache -> per-station interval -> token bucket ->    |
        |    size check -> rewrite path                                    |
        +-------+----------------------------------------------+--------+
                |  statistics, reasons (g_digi)                |  transmit request
        +-------v----------+                          +--------+---------+
        | telemetry / log   |                          | TX scheduler: guard time, one radio, back to RX |   system layer (satellite_digi.c)
        +------------------+                          +------------------+
        configuration: one generated header (TUI), engine gets a DigiConfig_t at start
```

Design rules behind it:

* **The engine is a pure function.** `Digi_Process(state, body, len, now_ms, out, outLen)` has no radio, no RTOS and no clock of its own, so it
  runs identically on the Cortex-M0+, in the host tests and in a future ground gateway. 4,067 host checks and the on-chip rule self-test use the same code.
* **Policy is data.** Call sign, aliases, tiers, N limit, trace, preemption, windows and rates are one struct (`DigiConfig_t`), set by the TUI.
* **Decisions are explainable.** Every drop has a reason code (`not-for-us`, `loop`, `duplicate`, `hops`, `rate`, ...) and a counter; the log line
  names the frame and the reason.
* **Fail closed.** Malformed address, non-UI, bad FCS, oversized result, empty bucket: not repeated.
* **State changes only on a repeat** (cache, station table, token), so a rejected frame cannot poison the limits.
* **Modem independence.** The engine only sees the frame body (address field through info, no flags, no FCS), so it does not care which modem delivered
  it: the satellite uses the GMSK path, the tests use the host build of the same framing code.

## 3. Modem: GMSK 4800 bit/s

The digipeater uses the satellite's GMSK modem and nothing else: the SX126x modulates / demodulates (GFSK, BT 0.5, 1.2 kHz deviation, 4800 bit/s, sync word
`93 0B 51 DE`) and `mission/protocol.c` does the bit layer (HDLC stuffing, NRZI, G3RUH scrambler, 20 leading flags, fixed 200-byte packet). Dire Wolf decodes it
(`-B 4800 -g`), which is how the host chain test checks the repeats. The AFSK1200 software modem that was written after the paper "AFSK APRS Digipeater"
(Dayarathna, V1.2: matched-filter correlators on an STM32G431) was removed again: this satellite is GMSK only. Its archive:
`../s2s2-link_removed_afsk_2026-10-10.tar.gz`. Two findings from that work are worth keeping if an AFSK front end is ever added: the paper's correlator window of 32 samples cannot
resolve single-bit tone changes (it must be one bit, 8 samples at 9.6 kHz), and the AX.25 CRC residue is `0xF0B8` (the paper also prints `0xF0D8`).

## 4. How it fits our packet format

* Frames are the Packet Format sheet's AX.25 UI frames: control `0x03`, PID `0xF0`, 20 leading flags, FCS, 1-5 closing flags, in the fixed 200-byte
  G3RUH radio packet at 4800 bit/s (435.000 MHz down, 437.375 MHz up).
* **Commands and sheet packets never take the digipeater path**: a frame without a via address goes to the command logic as before. A frame with a path is
  somebody else's traffic.
* Digipeated frames keep their source, destination and information untouched. **No integrity trailer** is added or removed (they are not our packets; AUTH
  stays a property of our own command / ACK / telemetry packets).
* Size: `DIGI_MAX_BODY = 160` bytes (addresses + control + PID + info); a repeat that grows by the 7-byte trace address must still fit the radio packet,
  otherwise it is dropped as `too-long`. Typical APRS position reports are 40-80 bytes.
* The ACK / NACK sheet reserves mission id `0x09` for the digipeater; no digipeater command exists in the command sheet, so enabling and the limits are
  a build-time setting (TUI), not an uplink command. Statistics are in RAM (`g_digi.stats`, readable over SWD) and on the debug UART.
* Uplink and downlink differ (437.375 / 435.000 MHz), so the satellite is a **crossband-style** digipeater: it receives on the uplink frequency and
  repeats on the downlink frequency, both GMSK 4800 bit/s. A standard 1200 baud AFSK APRS terminal (144.390 MHz) cannot talk to it; users need a GMSK
  / G3RUH capable radio or SDR on the two UHF frequencies.

### Rules, in the order they are applied

| # | Check | Result when it fails |
|---|---|---|
| 1 | digipeater enabled | `disabled` |
| 2 | address field parses (A-Z 0-9 call signs, extension bit only on the last address, <= 8 via) | `malformed` |
| 3 | UI frame, PID 0xF0 | `not-ui` |
| 4 | path not longer than `max_via` | `hops` |
| 5 | an unused via entry exists | `no-path` |
| 6 | source is not us / not denied | `own-source` / `denied` |
| 7 | our call is not already used in the path | `loop` |
| 8 | next unused entry is our call, the alias, or `WIDE2-N` with 1 <= N <= max (and optionally a later entry for preemption) | `not-for-us` / `hops` |
| 9 | not a duplicate (source + destination + info, 30 s) | `duplicate` |
| 10 | this station was not repeated in the last 10 s | `source-rate` |
| 11 | rewritten frame fits | `too-long` |
| 12 | token bucket (6 per minute, burst 2) | `rate` |

Rewrites: own call or alias -> `MYCALL*`; `WIDE2-2` -> `MYCALL*,WIDE2-1` (trace) or `WIDE2-1` (no trace); `WIDE2-1` -> `MYCALL*` (trace) or `WIDE2*`.
When the path is already full the trace call is left out instead of dropping the frame.

## 5. On the satellite

```
CW beacon (TX ~30 s) -> listen window (RX, 437.375 MHz)
                          frame decoded?
                           +- no path / not valid       -> command logic (unchanged)
                           +- path, bad FCS             -> ignored
                           +- path, good FCS            -> Digi_Process
                                                             +- drop    -> log reason, keep listening
                                                             +- repeat  -> 300 ms guard -> Satellite_Send_Raw_Frame (435.000 MHz, 5 V PA)
                                                                           -> radio back to RX
```

Limits you should know about:

* **Half duplex.** One radio, one frequency at a time: frames that arrive while the satellite is sending its CW beacon or a repeat are not heard.
  That is roughly a third of every 90 s cycle, so a station should repeat its beacon (APRS does anyway).
* The repeat takes 0.33 s of air time and the satellite's 5 V PA is on for it; the token bucket (6 per minute) is the power / heat budget.
* Counters (`g_digi.stats`): frames offered, repeated, one counter per drop reason. Boot self-test: `g_digiSelfTest == 0xA5A50002`.
* Flash: the digipeater costs about 5.1 KB; the satellite image is now 90.5 % of the 56 KB. `DIGI=FALSE` (TUI) removes it completely.

## 5b. Store and forward, and hopping

The satellite does not only repeat a frame once: the frame it accepts is **kept** and sent again later, so a station that was out of
range the first time (another continent, or just not in the footprint during that pass) still gets it.

```
 heard frame --Digi_Process--> REPEAT (path rewritten for our hop)
                                  +--> send now (S2S2_SAF_IMMEDIATE)
                                  +--> Saf_Store: keep the rewritten frame, hop budget = N of its first unused WIDEn-N
 later:  Sat_Digi_Service: Saf_NextDue -> send again -> Saf_Forwarded (count, next time = now + interval; gone after the last send)
 always: every change -> mailbox -> CPU1 journal /obc/saf.jrn (flash);  after a restart CPU1 plays the journal back
```

**Hopping.** A message carries a hop budget in its path, exactly as `WIDEn-N` does in APRS: `WIDE2-2` = two hops.

* A node that accepts the frame uses **one hop** (the engine rewrites the path: `NODE1*,WIDE2-1`) and stores that copy with the budget that is left.
* The next node that hears it (another satellite, a ground digipeater, or the same satellite's later send) can use the next hop (`NODE1,NODE2*`).
  When the budget is spent the frame can still be carried and heard, but nobody hops it any further.
* A node never takes back a frame that already carries its own call sign (loop check), so the number of hops in the whole network is bounded by the budget.
* The satellite's own re-sends are *carry* sends of the same frame (`max_forwards`, `forward_interval`, `ttl`); they do not use more hops.
* Send order: messages that still have hops left first (they can travel further), then those sent least often, then the oldest.
* When the table (12 messages) is full the message that has been sent most often is replaced, then the oldest.

`tests/host/test_saf.c` runs three nodes: `WIDE2-2` gives `NODE1-1*,WIDE2-1` at node 1 and `NODE1-1,NODE2-1*` at node 2; node 3 hears the second copy
and does nothing (no unused entry), node 1 hearing its own copy does nothing (loop), and later, when node 1 sends its stored copy again, node 3 takes it:
`NODE1-1,NODE3-1*`.

**Flash.** CPU2's own flash is its code, so the messages are written by CPU1 (NuttX, LittleFS in pages 72-99, the same store as the image):

* mailbox in SRAM2 `0x20008000` (`saf_shared.h`, one writer per field, sequence-number handshakes, either core may restart alone);
* journal record = 12-byte header + body + CRC-16, appended and `fsync`ed; a torn tail is ignored; the file is rewritten (compacted) at boot and past 16 KB;
* the image-store clean-up (`NEWFILE`) keeps `saf.*`;
* limits: no real-time clock, so a message's remaining life is saved but the time the satellite was off is not counted; a journal write costs one flash page program per change (a few per minute at most).

**Settings** (TUI "Store and forward"): on/off, send immediately too, sends per message (3), seconds between sends (900), lifetime in minutes (360).
Cost: +2.9 KB on the satellite (now 95 % of the 56 KB) and about 1 KB of CPU1 flash (`nuttx.bin` 144,140 of 147,456 bytes).

**Statistics in the message (test option, `S2S2_SAF_APPEND_STATS`).** Done in the satellite firmware (`append_stats()` in `satellite_digi.c`): the frame that is repeated right away
gets the state of the store appended to its text, cut if it would not fit the radio packet; the copy in the store stays unchanged:
`ridam and damri and ryan and nayr | MEMORY OBJECTS: #1 id=1 len=56 hops_left=0 forwarded=0 ttl_left=21600s | STORED 1 msg, 56 B` (read back from the satellite's transmit buffer over SWD).
`saf_tool.py inject` also prints the stored objects afterwards (`MEMORY OBJECTS: #n ...`, the stored bytes, `STORED n msg, N B`). It alters other stations' text: use it for tests only.

**Another meaning of "hopping"** is radio frequency hopping (the SX126x has LR-FHSS). That is a different layer (it changes how a packet is modulated, not how
a message travels) and is **not** implemented here.

**Test bench.** `make testbench`, section "Digipeater + store and forward" (needs only the Nucleo's VCP, no ground station): *Run the digipeater test* injects 8 frames and checks each decision
(alias and `WIDE2-2` repeated, duplicate, `WIDE1-1` fill-in, no path, own source, loop, hop limit), then lists the stored messages with hops left, times sent, time left and bytes; with the SDR
box ticked an RTL-SDR + Direwolf also checks that the repeats are decoded on 435.000 MHz with the rewritten path. The buttons *Inject this frame*, *Show the stored messages* and *Clear the journal*
do the same one at a time. The test frames use a `>` status text and the experimental destination `APZS2S`, so Dire Wolf's APRS checker has nothing to complain about (free text
or the generic destination `APRS` give harmless warnings such as `Unknown APRS Data Type Indicator` / `Use of "APRS" in the destination field is obsolete`: they are about the content, the frame itself decoded fine).
Run on the hardware: 8 of 8 cases as expected, 3 messages / 153 B in the journal.

## 6. Settings (TUI, "Satellite digipeater")

| Setting | Macro | Default |
|---|---|---|
| enable | `S2S2_DIGI_ENABLE` | on |
| alias besides our call (`ARISS`, empty = none) | `S2S2_DIGI_ALIAS` | `ARISS` |
| answer `WIDE2-N` | `S2S2_DIGI_WIDE` | on |
| highest N accepted | `S2S2_DIGI_MAX_N` | 2 |
| trace (insert our call) | `S2S2_DIGI_TRACE` | on |
| duplicate window | `S2S2_DIGI_DEDUPE_MS` | 30 s |
| repeats per minute / burst | `S2S2_DIGI_RATE_PER_MIN`, `S2S2_DIGI_BURST` | 6 / 2 |
| minimum time between two repeats of one station | `S2S2_DIGI_SRC_MIN_MS` | 10 s |
| TEST: append the store statistics to the text of the frame repeated right away (`STATS` in the TUI) | `S2S2_SAF_APPEND_STATS` | off |

Our call sign and SSID are the satellite's (`9N2SJ-1`). Try it from the ground station console: `DIGI K1ABC>APRS,ARISS:hello`,
`DIGI K1ABC>APRS,WIDE2-2:!4237.14N/07120.83W-test`; the repeat appears as `K1ABC>APRS,9N2SJ-1*,WIDE2-1:...` in the GS log.

## 7. Evidence

```
make test        runs, in tests/host:
  rt_auth          framing + integrity check round trip, every flipped bit rejected
  test_digipeater  4,067 checks: every rule above, WIDEn-N and trace, preemption, duplicates incl. millisecond wrap, rate limits, round trips of 2000 random frames
  digi_chain       sender packet -> satellite framing -> Digi_Process -> repeat packet -> 4800 bit/s G3RUH audio -> Dire Wolf decodes the 4 repeats
                   (alias, WIDE2-2 trace, own call, earlier hop kept); the duplicate, the WIDE1-1 and the direct frame are dropped
  test_saf         53 checks: store, duplicate refusal, priority by hop budget, interval, max sends, TTL incl. millisecond wrap, eviction, journal bookkeeping,
                   restore, and a three-node hop chain (budget 2: node 3 takes nothing from the second copy but takes the stored copy later)
on the chip      g_digiSelfTest = 0xA5A50002 (trace, duplicate, loop, alias, fill-in tier, own source, store schedule) on the Cortex-M0+
```

**On the air, with the hardware (2026-10-10, one Nucleo + RTL-SDR + Dire Wolf, no ground station):** the PC injected frames through CPU1 (`saf_tool.py inject`), the
satellite digipeated them on 435.000 MHz and Dire Wolf decoded them through the SDR (`docs/proof/saf_digipeater_direwolf.txt`):
`K2TEST>APRS,9N2SJ-1*,WIDE2-1:digipeated and stored, try 2` (path `WIDE2-2` rewritten with trace). The three stored messages were then in the CPU1 journal
(`saf_tool.py list`: ids, hop budget, TTL); after a **full reflash of both cores** the journal was still there, CPU2 restored all 3 (`s_restored = 3` over SWD), and the
messages were sent again from the store (interval set to 20 s for the test), decoded by Dire Wolf each time. The same run showed the per-station limit (`same station
too soon`) and the 30 s duplicate window working. The test messages were removed with `saf_tool.py clear`.

**Not done:** the ground-station side of the over-the-air test (see below). The ground station image with the `DIGI` command (and the new key) is built but could not be flashed (its ST-Link is
not connected), and the receive path of the satellite for frames with a via path has been exercised only through the host chain and the on-chip rule
self-test. Do `make flash` for both boards, then `DIGI K1ABC>APRS,ARISS:hello` on the GS console and watch for the repeat; the satellite debug UART
prints `[DIGI] REPEAT` / `not repeated (reason)`.

## Sources

* Dire Wolf source: [digipeater.c](https://github.com/wb2osz/direwolf/blob/master/src/digipeater.c), [dedupe.c](https://github.com/wb2osz/direwolf/blob/master/src/dedupe.c), [APRS Digipeaters document](https://cloud.xlxbra.net/direwolf/direwolf_git_v1.7/direwolf-doc_git/APRS-Digipeaters.pdf)
* [aprsdigi man page](https://www.mankier.com/8/aprsdigi): duplicate window and loop check
* [AX.25 layer 2 digipeater algorithm](https://ax25.net/digialgorithm.aspx)
* [AMSAT PACSAT digipeater discussion](https://mailman.amsat.org/hyperkitty/list/pacsat-dev@amsat.org/message/5NJZA6CPR2GZXVWIRBD3ZJO6AUKJWJML/attachment/4/Pacsat-digipeater-discussion.pdf)
* [APRS digipeaters, New-N paradigm (Argent Data)](https://argentdata.com/support/aprs_digipeaters/), [Fixing the 144.39 APRS network](https://www.aprs.org/fix14439.html), [digipeater setup](https://argentdata.com/support/digipeater_setup/)
