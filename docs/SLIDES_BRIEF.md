# PDR Slides Brief: Slippers2Sat-2 COM Subsystem

Scope: Communications (COM) subsystem, satellite to ground station.
OBC / CPU1 is out of scope, except as the source of telemetry through the shared ring buffer.

Status key:
- **Done** = in code, built, matches the sheets.
- **Placeholder** = value needed for the slide, not yet defined. Mark TBD on the slide.
- **Pending** = decided, not yet in code.
- **Conflict** = sheets or code disagree. Needs a decision before PDR.

---

## 1. Identity
- **Project name (slides):** Slippers2Sat-2. CW project name: S2S2 (Sheet 4).
- **Satellite callsign:** 9NS2S2 (confirmed).
- **Ground station:** GROUND.
- **Hardware:** STM32WL55JC1 on the satellite board (Nucleo-WL55JC) and on the ground station board.
- **Radio core:** the STM32WL55's Cortex-M0+ (CPU2) runs the Sub-GHz radio, packet framing, and Morse keyer.

---

## 2. RF link (Done)

| | Uplink (GS to Satellite) | Downlink (Satellite to GS) |
|---|---|---|
| Frequency | 437.375 MHz | 435.313 MHz |
| Bandwidth | 8.5 kHz | 8.5 kHz |
| Modulation | GMSK | GMSK |
| Data rate | 4800 bps | 4800 bps |

- **Radio:** integrated Sub-GHz transceiver (SX126x class). RF switch (RF-SW) selects TRANSMIT, RECEIVE, or OFF.
- **Modulation detail:** GMSK = GFSK with h = 0.5 and BT = 0.5. The chip's API says "GFSK"; the signal is GMSK.
- **Deviation:** ±1200 Hz (h = 0.5 at 4800 bps).
- **Chip RX filter:** 9.7 kHz, the nearest SX126x setting at or above 8.5 kHz. No 8.5 kHz setting exists.
- **Packet:** fixed-length radio packet, 200 bytes, 24 leading flags for clock lock.
- **TX power:** 14 dBm drive, 3.3 V external PA, about 24.5 dBm at the antenna.
- **Ground receive:** RTL-SDR, `rtl_fm`, and Direwolf configured for G3RUH at 4800 bps.

---

## 3. Link budget (Placeholder)
From `JC2/tools/fault_injection_suite.py`. Bench-pessimistic assumptions.
- **TX:** 22 dBm + 0 dBi (whip). **Conflict:** firmware gives about 24.5 dBm at the antenna. Pick one.
- **RX:** 0 dBi bench antenna. A real ground station would add about +10 dBi.
- **Noise figure:** 6 dB. **RX bandwidth:** 9.7 kHz.
- **Path loss:** Friis free-space only.
- Label the slide as a placeholder.

---

## 4. Beacon cycle (Done)
1. CW1 transmitted
2. 90-second mission receive window
3. CW2 transmitted
4. 90-second mission receive window
5. Repeat

Each beacon is 8 bytes, packed to 16 hex characters and sent as Morse. Operational downlink is CW only.

---

## 5. CW1 (Sheet 4, 8 bytes)

| Byte | Field | Bits | Status | Notes |
|---|---|---|---|---|
| 1 | SYSTEM STATUS | 2 | Placeholder | 01 = SAFE, 10 = LOW POWER, 11 = NORMAL (00 undefined). Driven by battery status. **No thresholds exist in code or CLAUDE.md.** |
| 1 | MISSION STATUS | 2 | Conflict | 00 = RESERVED, 01 = CAM, 10 = ADCS, 11 = EPDM. Source: OBC via ring buffer. **Code subsystem IDs (ADCS 0x03, CAM 0x04, EPDM 0x05) don't match the sheet's 2-bit values.** One mapping table needed. |
| 1 | TIME AFTER RESET | 4 | Conflict | Hours, mod 16. Code uses CPU2 uptime; agreed definition is mission elapsed time. Confirm the source. |
| 2 | SOL_TOTAL_I | 8 | Done | |
| 3-4 | SOL_TOTAL_V | 16 | Done | |
| 5-6 | BAT_V | 16 | Done | |
| 7-8 | BAT_I | 16 | Done | |

---

## 6. CW2 (Sheet 4, 8 bytes)

| Byte | Field | Bits | Status |
|---|---|---|---|
| 1-3 | Gyro X, Y, Z | 8 each | Done |
| 4-6 | Magneto X, Y, Z | 8 each | Done |
| 7 | SOLAR CELL (5 bits) | 5 | Derived. Order +X, +Y, -Z, +Y, +Z (Sheet 4). |
| 8 | BAT_TEMP | 8 | Done |

- **Solar bits:** 0 = shadow, 1 = sunshine. Derived from panel voltage (nonzero = sunshine). Label "derived" on slides. Accepted for PDR.
- Sheet 4 lists +Y for cells 2 and 4. Code follows the sheet.

---

## 7. Telemetry source (Done in code, no real OBC yet)
- Telemetry arrives through the shared SRAM2 ring buffer (`ipc/Sring_buffer.h`).
- Beacon 1 and Beacon 2 field layouts match Sheet 2 byte for byte.
- Real OBC data is not connected yet. Current values are simulated on the bench.

---

## 8. Commands (Sheet 1/2)

**Done:**
- 13-byte command packet: SAT-ID, MCU-ID, opcode B1, B2, B3, DATA_ID (2), ADDRESS (4), NUM_PACKETS (2).
- Camera run: opcode 0xCC, B2 0x5E, B3 0xBD.
- File download: opcode 0x1D, B2 = directory (D1 or D2), B3 = file number. Console commands added: HEALTH (F2), FLAGS (F1), RSVTABLE (F4), SATLOG (F3), CAMFILE (F5), EPDM (F6), ADCS (F7).

Removed: every opcode and command path not in the sheets (REQUEST_BURST, HEARTBEAT, ABORT, RETRANSMIT, HOUSEKEEPING, BEACON1, BEACON2, DIGIPEATER, DOWNLOAD without a file, the 1-byte command). Opcode enum is now only 0xCC and 0x1D.

Sheet 2 lists file F8 with no name in the table. Not implemented.

---

## 9. Data IDs (Sheet 2, Done)
HK 0x01, Beacon 1 0x02, Beacon 2 0x03, ADCS 0x04, Camera/RGB 0x05, NIR 0x06, NDWI 0x07, EPDM 0x08, Digipeater 0x09, Satellite log 0x0A, General 0xFF.

---

## 10. ACK / NACK (Sheet 2)
- Response byte: 0xAC = ACK, 0xEE = NACK/error.
- Status codes 0x00 to 0x0B: accepted, completed, packet error, CRC error, incomplete, data missing, data unavailable, busy, mission inactive, invalid command, memory error, transmission failed.
- **Conflict:** Sheet 2's "ACK command completed" example shows 0xEE. The code sends 0xAC for completed, which matches the response-type table. Confirm.

---

## 11. Camera fragment (Sheet 3, Done in code, no driver yet)
- Fields: packet type 0x08, image ID (1), fragment index (2), total fragments (2), image type (RGB, NIR, NDWI), AI score (1), image data (128 baseline), CRC-16/X.25 (2).
- Overhead 10 bytes. Full-field data 246 bytes (after testing).
- Status: format defined, no camera driver in this build. Show as "defined, hardware pending".

---

## 12. Light FEC wrap (approved wording: "integrity check")
- Each packet gets a sequence counter, length byte, error-correction parity, and keyed integrity check.
- Corrects up to 16 byte errors per 255-byte block.
- Wire size 152 bytes. Integrity check can be disabled for bench tests (`AUTH=FALSE`). Default on.
- Do not use "Reed-Solomon", "RS", "authentication", "auth", or "light FEC wrap" on slides.

---

## 13. Verification status
# PDR Slides Brief: Slippers2Sat-2 COM Subsystem

Scope: Communications (COM) subsystem, satellite to ground station.
OBC / CPU1 is out of scope, except as the source of telemetry through the shared ring buffer.

Status key:
- **Done** = in code, built, matches the sheets.
- **Placeholder** = value needed for the slide, not yet defined. Mark TBD on the slide.
- **Pending** = decided, not yet in code.
- **Conflict** = sheets or code disagree. Needs a decision before PDR.

---

## 1. Identity
- **Project name (slides):** Slippers2Sat-2. CW project name: S2S2 (Sheet 4).
- **Satellite callsign:** 9NS2S2 (confirmed).
- **Ground station:** GROUND.
- **Hardware:** STM32WL55JC1 on the satellite board (Nucleo-WL55JC) and on the ground station board.
- **Radio core:** the STM32WL55's Cortex-M0+ (CPU2) runs the Sub-GHz radio, packet framing, and Morse keyer.

---

## 2. RF link (Done)

| | Uplink (GS to Satellite) | Downlink (Satellite to GS) |
|---|---|---|
| Frequency | 437.375 MHz | 435.313 MHz |
| Bandwidth | 8.5 kHz | 8.5 kHz |
| Modulation | GMSK | GMSK |
| Data rate | 4800 bps | 4800 bps |

- **Radio:** integrated Sub-GHz transceiver (SX126x class). RF switch (RF-SW) selects TRANSMIT, RECEIVE, or OFF.
- **Modulation detail:** GMSK = GFSK with h = 0.5 and BT = 0.5. The chip's API says "GFSK"; the signal is GMSK.
- **Deviation:** ±1200 Hz (h = 0.5 at 4800 bps).
- **Chip RX filter:** 9.7 kHz, the nearest SX126x setting at or above 8.5 kHz. No 8.5 kHz setting exists.
- **Packet:** fixed-length radio packet, 200 bytes, 24 leading flags for clock lock.
- **TX power:** 14 dBm drive, 3.3 V external PA, about 24.5 dBm at the antenna.
- **Ground receive:** RTL-SDR, `rtl_fm`, and Direwolf configured for G3RUH at 4800 bps.

---

## 3. Link budget (Placeholder)
From `JC2/tools/fault_injection_suite.py`. Bench-pessimistic assumptions.
- **TX:** 22 dBm + 0 dBi (whip). **Conflict:** firmware gives about 24.5 dBm at the antenna. Pick one.
- **RX:** 0 dBi bench antenna. A real ground station would add about +10 dBi.
- **Noise figure:** 6 dB. **RX bandwidth:** 9.7 kHz.
- **Path loss:** Friis free-space only.
- Label the slide as a placeholder.

---

## 4. Beacon cycle (Done)
1. CW1 transmitted
2. 90-second mission receive window
3. CW2 transmitted
4. 90-second mission receive window
5. Repeat

Each beacon is 8 bytes, packed to 16 hex characters and sent as Morse. Operational downlink is CW only.

---

## 5. CW1 (Sheet 4, 8 bytes)

| Byte | Field | Bits | Status | Notes |
|---|---|---|---|---|
| 1 | SYSTEM STATUS | 2 | Placeholder | 01 = SAFE, 10 = LOW POWER, 11 = NORMAL (00 undefined). Driven by battery status. **No thresholds exist in code or CLAUDE.md.** |
| 1 | MISSION STATUS | 2 | Conflict | 00 = RESERVED, 01 = CAM, 10 = ADCS, 11 = EPDM. Source: OBC via ring buffer. **Code subsystem IDs (ADCS 0x03, CAM 0x04, EPDM 0x05) don't match the sheet's 2-bit values.** One mapping table needed. |
| 1 | TIME AFTER RESET | 4 | Conflict | Hours, mod 16. Code uses CPU2 uptime; agreed definition is mission elapsed time. Confirm the source. |
| 2 | SOL_TOTAL_I | 8 | Done | |
| 3-4 | SOL_TOTAL_V | 16 | Done | |
| 5-6 | BAT_V | 16 | Done | |
| 7-8 | BAT_I | 16 | Done | |

---

## 6. CW2 (Sheet 4, 8 bytes)

| Byte | Field | Bits | Status |
|---|---|---|---|
| 1-3 | Gyro X, Y, Z | 8 each | Done |
| 4-6 | Magneto X, Y, Z | 8 each | Done |
| 7 | SOLAR CELL (5 bits) | 5 | Derived. Order +X, +Y, -Z, +Y, +Z (Sheet 4). |
| 8 | BAT_TEMP | 8 | Done |

- **Solar bits:** 0 = shadow, 1 = sunshine. Derived from panel voltage (nonzero = sunshine). Label "derived" on slides. Accepted for PDR.
- Sheet 4 lists +Y for cells 2 and 4. Code follows the sheet.

---

## 7. Telemetry source (Done in code, no real OBC yet)
- Telemetry arrives through the shared SRAM2 ring buffer (`ipc/Sring_buffer.h`).
- Beacon 1 and Beacon 2 field layouts match Sheet 2 byte for byte.
- Real OBC data is not connected yet. Current values are simulated on the bench.

---

## 8. Commands (Sheet 1/2)

**Done:**
- 13-byte command packet: SAT-ID, MCU-ID, opcode B1, B2, B3, DATA_ID (2), ADDRESS (4), NUM_PACKETS (2).
- Camera run: opcode 0xCC, B2 0x5E, B3 0xBD.
- File download: opcode 0x1D, B2 = directory (D1 or D2), B3 = file number. Console commands added: HEALTH (F2), FLAGS (F1), RSVTABLE (F4), SATLOG (F3), CAMFILE (F5), EPDM (F6), ADCS (F7).

Removed: every opcode and command path not in the sheets (REQUEST_BURST, HEARTBEAT, ABORT, RETRANSMIT, HOUSEKEEPING, BEACON1, BEACON2, DIGIPEATER, DOWNLOAD without a file, the 1-byte command). Opcode enum is now only 0xCC and 0x1D.

Sheet 2 lists file F8 with no name in the table. Not implemented.

---

## 9. Data IDs (Sheet 2, Done)
HK 0x01, Beacon 1 0x02, Beacon 2 0x03, ADCS 0x04, Camera/RGB 0x05, NIR 0x06, NDWI 0x07, EPDM 0x08, Digipeater 0x09, Satellite log 0x0A, General 0xFF.

---

## 10. ACK / NACK (Sheet 2)
- Response byte: 0xAC = ACK, 0xEE = NACK/error.
- Status codes 0x00 to 0x0B: accepted, completed, packet error, CRC error, incomplete, data missing, data unavailable, busy, mission inactive, invalid command, memory error, transmission failed.
- **Conflict:** Sheet 2's "ACK command completed" example shows 0xEE. The code sends 0xAC for completed, which matches the response-type table. Confirm.

---

## 11. Camera fragment (Sheet 3, Done in code, no driver yet)
- Fields: packet type 0x08, image ID (1), fragment index (2), total fragments (2), image type (RGB, NIR, NDWI), AI score (1), image data (128 baseline), CRC-16/X.25 (2).
- Overhead 10 bytes. Full-field data 246 bytes (after testing).
- Status: format defined, no camera driver in this build. Show as "defined, hardware pending".

---

## 12. Light FEC wrap (approved wording: "integrity check")
- Each packet gets a sequence counter, length byte, error-correction parity, and keyed integrity check.
- Corrects up to 16 byte errors per 255-byte block.
- Wire size 152 bytes. Integrity check can be disabled for bench tests (`AUTH=FALSE`). Default on.
- Do not use "Reed-Solomon", "RS", "authentication", "auth", or "light FEC wrap" on slides.

---

## 13. Verification status

| Item | Status |
|---|---|
| Satellite and GS radio firmware build | Done |
| Radio firmware flashed to both boards | Done, verified on both |
| Light FEC wrap | Host round-trip tests only |
| Sheet-based command and file-download changes | Built, not run on hardware |
| Satellite debug command interface on UART | Built, **not run on hardware** |
| GS to satellite RF link | **Not tested** |
| End-to-end test, integrity check on | **Not run** |
| Direwolf + SDR capture | **Not run** |

| Item | Status |
|---|---|
| Satellite and GS radio firmware build | Done |
| Radio firmware flashed to both boards | Done, verified on both |
| Light FEC wrap | Host round-trip tests only |
| Sheet-based command and file-download changes | Built, not run on hardware |
| Satellite debug command interface on UART | Built, **not run on hardware** |
| GS to satellite RF link | **Not tested** |
| End-to-end test, integrity check on | **Not run** |
| Direwolf + SDR capture | **Not run** |
