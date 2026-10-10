# STM32WL55 GFSK AX.25 G3RUH (9600 Baud) Firmware Documentation

## 1. System Architecture Overview

The **STM32WL55JC** is a dual-core SoC consisting of:
1. **CPU1 (Cortex-M4):** Runs the main application / RTOS (NuttX OS).
2. **CPU2 (Cortex-M0+):** Dedicated standalone real-time Sub-GHz radio coprocessor handling all RF PHY, GFSK modulation, AX.25 framing, and G3RUH scrambling/descrambling.

This documentation details the **CPU2 (Cortex-M0+)** firmware located in:
`/home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2`

```
+----------------------------------------------------------------------------------------------------+
|                                    SATELLITE FIRMWARE (CPU2 CM0+)                                  |
|                                                                                                    |
|   +--------------------------+       +----------------------------+       +--------------------+   |
|   |  ipcc_m0plus_main.c      | ----> |      core/protocol.c       | ----> |  core/radio_app.c  |   |
|   |  - State Machine         |       |  - AX.25 UI Frame Builder  |       |  - Pure G3RUH GFSK |   |
|   |  - Beacon Timer (5s)     |       |  - CRC-16 / FCS (LSB)      |       |  - 9600 baud, BT0.5|   |
|   |  - 50x Telemetry Burst   |       |  - Bit Stuffing (5x 1s)    |       |  - Fdev = 4800 Hz  |   |
|   |  - Command Dispatcher    |       |  - NRZ-I Encoding          |       |  - +22 dBm (RFO_HP)|   |
|   +--------------------------+       |  - G3RUH Scrambling        |       +--------------------+   |
|                                      +----------------------------+                 |              |
|                                                                                     v              |
|                                                                           +--------------------+   |
|                                                                           | target/            |   |
|                                                                           | radio_board_if.c   |   |
|                                                                           | - Nucleo PC3/4/5   |   |
|                                                                           | - Custom PA8/PA1   |   |
|                                                                           +--------------------+   |
+-------------------------------------------------------------------------------------|--------------+
                                                                                      | (435.000 MHz)
                                                                                      v
+----------------------------------------------------------------------------------------------------+
|                                  GROUND STATION / SDR RECEIVER                                     |
|                                                                                                    |
|   +--------------------------+       +----------------------------+       +--------------------+   |
|   |  RTL-SDR / SDR Console   | ----> |  VB-Audio Virtual Cable    | ----> |  UZ7HO SoundModem  |   |
|   |  - N-FM / Flat Audio     |       |  - 48000 Hz / 16-bit Mono  |       |  - G3RUH 9600bd    |   |
|   |  - De-emphasis: OFF      |       |  - Wide Baseband Audio     |       |  - CRC Check OK    |   |
|   |  - 18-20 kHz Filter BW   |       +----------------------------+       |  - Decoded Text    |   |
|   +--------------------------+                                            +--------------------+   |
+----------------------------------------------------------------------------------------------------+
```

---

## 2. Directory Structure & Key Files

### Primary Entry Points
* [ipcc_m0plus_main.c](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/ipcc_m0plus_main.c): **Main entry point for Satellite CPU2 firmware**. Manages the main execution loop, state machine (`STATE_IDLE_RX`, `STATE_PROCESS_COMMAND`, `STATE_TRANSMIT_BURST`), 5-second beacon timer, telemetry burst dispatcher, and UART2 diagnostic logs.
* [gs_m0plus_main.c](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/gs_m0plus_main.c): Standalone Ground Station testing firmware entry point.

### Protocol & Framing Subsystem (`core/`)
* [core/Inc/config.h](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/core/Inc/config.h): **Master Configuration Header**. Defines operating frequencies (`435.000 MHz` TX, `437.375 MHz` RX), Callsigns (`NEPSAT` / `GROUND`), Bitrate (`9600 bps`), Deviation (`4800 Hz`), Preamble length, and packet sizes.
* [core/Src/protocol.c](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/core/Src/protocol.c) / [core/Inc/protocol.h](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/core/Inc/protocol.h): **Complete AX.25 + G3RUH Engine**. Implements `Protocol_CreatePacket()` (TX pipeline) and `Protocol_ExtractFrame()` (RX pipeline).
* [core/Src/ax25.c](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/core/Src/ax25.c) / [core/Inc/ax25.h](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/core/Inc/ax25.h): AX.25 Unnumbered Information (UI) frame builder, address field bit-shifter (SSID/HDLC), and frame parser.
* [core/Src/crc16.c](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/core/Src/crc16.c) / [core/Inc/crc16.h](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/core/Inc/crc16.h): Standard CCITT-16 / X.25 reflected table-driven CRC calculation (Residue `0xF0B8`).
* [core/Src/g3ruh.c](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/core/Src/g3ruh.c) / [core/Inc/g3ruh.h](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/core/Inc/g3ruh.h): G3RUH hardware self-synchronizing scrambler and descrambler ($1 + x^{12} + x^{17}$).
* [core/Src/radio_app.c](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/core/Src/radio_app.c) / [core/Inc/radio_app.h](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/core/Inc/radio_app.h): Radio application layer interfacing `protocol.c` with the low-level Sub-GHz radio hardware.

### Hardware & RF Board Interface (`target/` & `bsp/`)
* [target/radio_board_if.c](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/target/radio_board_if.c): Radio Board Interface layer. Drives the RF switches:
  * Nucleo-WL55JC board: `PC4` (`FE_CTRL1`), `PC5` (`FE_CTRL2`), `PC3` (`FE_CTRL3`).
  * Custom Satellite PCB: `PA8` (PA Enable), `PA1` (LNA Enable).
  * Manages TCXO power (`PB0` / `VDD_TCXO`) and SMPS DCDC power converter.

### Low-Level Radio Driver (`radio_driver/` & `subghz/`)
* [radio_driver/radio_driver.c](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/radio_driver/radio_driver.c): SX126x low-level register interface (`SUBGRF_SetTxParams`, `SUBGRF_SetPaConfig`, `SUBGRF_SetModulationParams`, `Radio_SMPS_Set`).
* [radio_driver/radio.c](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/radio_driver/radio.c): High-level Semtech radio API wrapper.
* [uart_debug.c](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/uart_debug.c) / [uart_debug.h](file:///home/prem/Desktop/nuttxspace/nuttx/boards/arm/stm32wl5/nucleo-wl55jc/CPU2/uart_debug.h): Direct non-blocking USART2 output (PA2/PA3 @ 115200 baud) for live serial diagnostics.

---

## 3. Detailed Data Flow & Protocol Operation

### Transmit (TX) Data Flow Pipeline

```
[Plaintext Payload] e.g. "NEPSAT BEACON"
       |
       v
1. AX.25 UI Frame Assembly:
   - Destination Address (7 bytes): "GROUND" shifted left 1 + SSID 0x60
   - Source Address (7 bytes):      "NEPSAT" shifted left 1 + SSID 0x63 (last address bit set)
   - Control Byte (1 byte):         0x03 (Unnumbered Information UI)
   - Protocol Identifier (1 byte):  0xF0 (No layer 3 protocol)
   - Information Field:             "NEPSAT BEACON"
   - CRC-16 / FCS (2 bytes):        CCITT-16 calculated over Header + PID + Payload
       |
       v
2. Serialization & Bit Stuffing:
   - Leading Preamble: 64 flags (0x7E = 01111110) sent LSB-first
   - Opening Flag: 0x7E
   - Payload + FCS: Serialized LSB-first. 
     * If five consecutive '1' bits occur, a '0' bit is automatically inserted.
   - Closing Flag: 0x7E
   - Cyclic Padding: Remaining buffer space padded with continuous 0x7E flag patterns.
       |
       v
3. NRZ-I Encoding:
   - Bit '0' -> Causes a logic transition (toggle).
   - Bit '1' -> Maintains previous logic level (no transition).
       |
       v
4. G3RUH Polynomial Scrambler:
   - Polynomial: 1 + x^12 + x^17
   - Feedback XOR: bit11 ^ bit16
   - Scrambled Output: input_bit ^ feedback
   - Shift Register: (SR << 1) | scrambled_bit
       |
       v
5. MSB-First Byte Packing:
   - Stream of scrambled bits is packed 8 bits per byte into the SX126x FIFO.
       |
       v
6. Sub-GHz Radio RF Transmission:
   - Modulation: GFSK @ 9600 bps, BT = 0.5
   - Frequency Deviation: +/- 4800 Hz
   - Hardware Sync Word: Disabled (0 bits) -> Pure audio G3RUH bitstream on air.
   - RF Switch: PC3=HIGH, PC4=LOW, PC5=HIGH / PA8=HIGH (High Power PA +22 dBm).
```

---

### Receive (RX) Data Flow Pipeline

```
[RF Signal at 437.375 MHz]
       |
       v
1. SX126x Hardware Demodulation:
   - Continuous GFSK reception into RX FIFO buffer (160/200 bytes).
       |
       v
2. G3RUH Polynomial Descrambler:
   - Taps: bit11 ^ bit16 from 17-bit shift register.
   - Descrambled Bit: raw_bit ^ bit11 ^ bit16.
       |
       v
3. NRZ-I Decoding:
   - If current_descrambled_bit == previous_descrambled_bit -> decoded bit is '1'.
   - If current_descrambled_bit != previous_descrambled_bit -> decoded bit is '0'.
       |
       v
4. HDLC Flag Detector & Frame Synchronization:
   - Scans bitstream for 0x7E (01111110).
   - When 0x7E is found, enters RX_DATA mode.
       |
       v
5. Bit De-Stuffing:
   - Tracks consecutive '1's. When five '1's are followed by a '0', the '0' is discarded.
   - When five '1's are followed by a '1' (0x7E), signals End-of-Frame.
       |
       v
6. FCS Validation:
   - Accumulates CRC-16 CCITT over all received bytes.
   - Standard Residue Check: Valid frame MUST have CRC Residue = 0xF0B8.
       |
       v
7. Command Execution:
   - Extracts payload command opcode (e.g., Opcode 0x01 triggers 50x Telemetry Burst).
```

---

## 4. Master Configuration Table (`core/Inc/config.h`)

| Parameter | Macro Name | Value | Description |
| :--- | :--- | :--- | :--- |
| **Transmit Frequency** | `DEFAULT_TX_FREQUENCY_HZ` | `435000000UL` | Satellite $\rightarrow$ Ground Station Beacon / Telemetry ($435.000\text{ MHz}$). |
| **Receive Frequency** | `DEFAULT_RX_FREQUENCY_HZ` | `437375000UL` | Ground Station $\rightarrow$ Satellite Command Uplink ($437.375\text{ MHz}$). |
| **Bitrate** | `RADIO_BIT_RATE_BPS` | `9600` | Standard high-speed satellite GFSK data rate ($9600\text{ bps}$). |
| **Frequency Deviation** | `RADIO_FDEV_HZ` | `4800` | $\pm 4.8\text{ kHz}$ ($9.6\text{ kHz}$ peak-to-peak tone spacing, $h=1.0$). |
| **Gaussian Shaping** | `MOD_SHAPING_G_BT_05` | $BT = 0.5$ | Gaussian pulse filtering for minimal spectral splatter. |
| **Leading Flag Count** | `PROTOCOL_LEADING_FLAG_COUNT` | `64` | 64 flags ($53\text{ ms}$ at 9600 baud) for UZ7HO audio PLL/DCD lock. |
| **Physical Packet Size**| `RADIO_FIXED_PACKET_LEN` | `200` | Fixed SX126x transfer size in bytes. |
| **Output Power** | `RADIO_TX_POWER_DBM` | `22` | $+22\text{ dBm}$ ($160\text{ mW}$) maximum High-Power PA output. |
| **Source Callsign** | `RADIO_CALLSIGN_SOURCE` | `"NEPSAT"` | Satellite Callsign (SSID 1). |
| **Dest Callsign** | `RADIO_CALLSIGN_DEST` | `"GROUND"` | Ground Station Callsign (SSID 0). |

---

## 5. Ground Station & SDR Setup Guide

To decode the satellite's transmissions on a PC:

### 1. SDR Software (SDR Console / SDR# / GQRX)
* **Frequency:** `435.000 MHz` (adjust for any RTL-SDR crystal ppm offset).
* **Demodulation Mode:** **`N-FM` (Narrowband FM)**.
* **Filter Bandwidth:** Set to **`18 kHz` – `20 kHz`**.
* **Audio De-emphasis:** **OFF (Unchecked / None)** *(Essential: do not use voice de-emphasis)*.
* **Audio High-Cut (Low-Pass):** Set to **`12 kHz` or higher** (allows full 9600 baud audio harmonics).
* **Squelch & Noise Reduction:** **OFF**.
* **Audio Output Device:** Route audio to **VB-Audio Virtual Cable**.

### 2. Windows Sound Control Panel
* Set **CABLE Input** and **CABLE Output** to **`16 bit, 48000 Hz`** (or `96000 Hz`).

### 3. UZ7HO High-Speed SoundModem
* **Modem Type:** Select **`FSK G3RUH 9600bd`** on Channel A.
* **Audio Input:** Select **CABLE Output (VB-Audio Virtual Cable)**.
* **DCD Threshold:** Set slider to $\approx 30\% - 40\%$.
* **Verification:** The waterfall will display uniform audio noise across 0–12 kHz. During transmission, the **`DCD A`** indicator will turn RED and decoded AX.25 packets will print in the log window.

---

## 6. Build and Flashing Instructions

### Compilation
From the `CPU2` directory:
```bash
make clean
make
```
* **Output ELF:** `ipcc_m0plus.elf`
* **Output Binary:** `ipcc_m0plus.bin`
* **Target Flash Memory Address:** `0x08020000` (Cortex-M0+ CPU2 Flash partition)

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
=== STM32WL55 AX.25 + G3RUH COMMAND->BURST START [rev8: RadioApp-only TX/RX] ===
RF FRONT END OK
RadioApp_Init: ENTER
Radio RX = 437375000 Hz
Radio TX = 435000000 Hz
Bitrate = 9600
Deviation = 4800 Hz
LISTENING FOR COMMAND (and beaconing every 5s)...
```
