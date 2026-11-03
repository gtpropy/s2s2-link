# Mock satellite (Nucleo only)

Separate CPU2 firmware for a stock Nucleo-WL55JC that answers like the satellite with **made-up data**, so the ground station and Mission Control can be
tested in real mode (real UART, real radio) without CPU1, OBC, sensors or camera. Not flight firmware. It is built with the Nucleo front end only
(`CUBUS`, `FRONTEND`, `DIGI`, `DEBUG` are forced in `mocksat.mk`) and the flash guard refuses a CUBUS board.

```
make -C tests/mocksat                                   # build  -> build/mocksat/mocksat.bin (32 KB, 56 %)
make -C tests/mocksat flash SERIAL=<satellite ST-Link>  # CPU2 only, CPU1 stays (make probes lists the serials)
# back to the real satellite:  cd cpu2/satellite && make CUBUS=F flash SERIAL=<same probe>
```

What it sends: same code as the real satellite (`satellite_app.c`, `protocol.c`, radio layer), so frames are in the real format. Only the data is invented.

| Command | Answer |
|---|---|
| CW beacon, every `beacon_period_s` from the start of the previous one | CW1 / CW2 alternating, `S2S2 9N2SJ` + 8 bytes; values follow a compressed 10-minute orbit (62 % sunlit): battery 3.6-4.05 V charging in sun and discharging in eclipse, solar 0 in eclipse, temperatures, gyro, magnetometer |
| ping `53 01 1D D1 F5` | ACK, `ACK: PING OK (OBC_ONLINE)`, ACK |
| HEALTH SAT `53 01 1D D1 F2` (count N > 1: stored records) | ACK, HK packet(s), ACK |
| HK download `53 01 1D D1 F8` | ACK, N HK packets (max 12), ACK |
| files F1 F3 F4 (D1/D3), F6 F7 (D2/D4) | ACK, 3 text chunks, ACK |
| camera download `53 01 1D D2 F5`, dummy camera `53 04 CC 5E BE` | ACK, **8 fragments of a 952-byte JPEG** (about 8 s), ACK: the quick image test |
| run camera `53 04 CC 5E BD` / ADCS `53 03 ..` / EPDM `53 05 ..` / antenna `53 01 DE EF AA` | ACK, 1-3 s, ACK (nothing moves) |
| anything else / 0x53 frame not 13 bytes | NACK `EE 09 FF` / `EE 04 FF` |

Timing is the real one: a command is only heard in the listen window after a beacon, the reply starts 700 ms after the command, and the next beacon follows at once.
With AUTH on the receive replay floor is reset once on a rejection (a restarted ground station counts from 1; this image has no VCP for RESET). The tag is still checked,
and the ground station must be built from the same `generated/secrets.h` and AUTH setting.

Picture: `make_image.py` (Pillow) writes `mock_image.h` and `mock_image_preview.jpg`.
