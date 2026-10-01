#include "satellite_app.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "Sring_buffer.h"
#include "uart_debug.h"
#include "protocol.h"
#include "ax25.h"
#include "radio.h"
#include "radio_app.h"

#define ABS(x) ((x) < 0 ? -(x) : (x))

/*
 * ----------------------------------------------------------------------------
 * SATELLITE RX LISTENING DURATION CONFIGURATION
 * ----------------------------------------------------------------------------
 * User requirement: "once send then stuck so please one listen is every 30 min"
 * Standard CubeSat sequence: Complete CW1 -> Listen RX 30 min -> Complete CW2 -> Listen RX 30 min
 * 30 minutes = 30 * 60 * 1000 = 1,800,000 ms.
 * (For quick bench testing, can be temporarily set to e.g. 30UL * 1000UL for 30s)
 */
#ifndef SAT_LISTEN_DURATION_MS
#define SAT_LISTEN_DURATION_MS (30UL * 60UL * 1000UL)
#endif

static char cwBeaconlive_msg1[64];
static char cwBeaconlive_msg2[64];

/*
 * ============================================================================
 * HELPER: DRAIN TELEMETRY PACKETS FROM M4 VIA SHARED SRAM2 RING BUFFER
 * ============================================================================
 */
static void M0_Drain_Telemetry(int16_t b1[5], int16_t b2[5])
{
    struct tx_packet_s pkt;

    if (ipcc_m0_received(IPCC_CH_BEACON))
    {
        ipcc_m0_clear(IPCC_CH_BEACON);
    }

    while (!rb_tx_empty())
    {
        if (rb_tx_read(&pkt))
        {
            if (pkt.id == TELEM_ID_B1)
            {
                b1[0] = pkt.data[0];
                b1[1] = pkt.data[1];
                b1[2] = pkt.data[2];
                b1[3] = pkt.data[3];
                b1[4] = pkt.data[4];
                uart1_printf("[M0+ IPC] Received B1: %d.%02dV %d.%02dV %d.%dC %d.%dC %d.%dC\r\n",
                             b1[0] / 100, (int)ABS(b1[0] % 100),
                             b1[1] / 100, (int)ABS(b1[1] % 100),
                             b1[2] / 10,  (int)ABS(b1[2] % 10),
                             b1[3] / 10,  (int)ABS(b1[3] % 10),
                             b1[4] / 10,  (int)ABS(b1[4] % 10));
            }
            else if (pkt.id == TELEM_ID_B2)
            {
                b2[0] = pkt.data[0];
                b2[1] = pkt.data[1];
                b2[2] = pkt.data[2];
                b2[3] = pkt.data[3];
                b2[4] = pkt.data[4];
                uart1_printf("[M0+ IPC] Received B2: %d.%02dA %d.%02dA %d.%02dA %d %d\r\n",
                             b2[0] / 100, (int)ABS(b2[0] % 100),
                             b2[1] / 100, (int)ABS(b2[1] % 100),
                             b2[2] / 100, (int)ABS(b2[2] % 100),
                             (int)b2[3],
                             (int)b2[4]);
            }
        }
    }
}

/*
 * ============================================================================
 * HELPER: PROCESS AND FORWARD TELECOMMAND TO M4
 * ============================================================================
 */
static void M0_Check_And_Forward_Command(void)
{
    if (rx_done_flag)
    {
        rx_done_flag = 0;
        uart1_printf(">>> [M0+ RX] Telecommand Received (%u Bytes) <<<\r\n", rx_frame_size);
        if (rx_frame_size >= CMD_PAYLOAD_LEN)
        {
            struct rx_command_s rx_cmd;
            rx_cmd.len = CMD_PAYLOAD_LEN;
            memcpy(rx_cmd.cmd, (const void *)rx_frame_buffer, CMD_PAYLOAD_LEN);
            rx_cmd.crc16 = 0;
            rb_rx_write(&rx_cmd);
            ipcc_m0_send(IPCC_CH_COMMAND);
            uart1_puts(">>> [M0+ RX] Forwarded Telecommand to M4 via IPCC Channel 3 <<<\r\n");
        }
        RadioApp_StartRx();
    }
    else if (rx_timeout_flag || rx_error_flag)
    {
        rx_timeout_flag = 0;
        rx_error_flag = 0;
        RadioApp_StartRx();
    }
}

/*
 * ============================================================================
 * HELPER: ACTIVE RX LISTENING WITH TELECOMMAND FORWARDING TO M4
 * - Deterministic CPU2 cycle delay ensures the loop never hangs or gets stuck.
 * - Continuously drains M4 telemetry to prevent SRAM2 ring buffer overflow.
 * - Emits periodic status heartbeats so ground/test operator sees countdown.
 * ============================================================================
 */
static void M0_Listening_Delay(uint32_t delay_ms, int16_t b1[5], int16_t b2[5])
{
    RadioApp_StartRx();
    uart1_printf("\r\n>>> [M0+] ENTERING RX LISTEN MODE (Duration: %lu min %lu s) <<<\r\n",
                 (unsigned long)(delay_ms / 60000UL),
                 (unsigned long)((delay_ms % 60000UL) / 1000UL));

    uint32_t remaining = delay_ms;
    uint32_t last_heartbeat = 0;
    const uint32_t chunk_ms = 50;
    const uint32_t heartbeat_interval = (delay_ms > 60000UL) ? 60000UL : 10000UL;

    while (remaining > 0)
    {
        M0_Check_And_Forward_Command();
        M0_Drain_Telemetry(b1, b2);

        uint32_t step = (remaining > chunk_ms) ? chunk_ms : remaining;
        CPU2_Delay_Ms(step);
        remaining -= step;
        last_heartbeat += step;

        /* Periodic heartbeat countdown so operator knows receiver is actively listening */
        if (last_heartbeat >= heartbeat_interval)
        {
            last_heartbeat = 0;
            if (remaining > 0)
            {
                uart1_printf(">>> [M0+ RX] Active Listening for Telecommands... (%lu min %lu s remaining) <<<\r\n",
                             (unsigned long)(remaining / 60000UL),
                             (unsigned long)((remaining % 60000UL) / 1000UL));
            }
        }
    }

    uart1_puts(">>> [M0+] RX Listen Window Completed. Proceeding to next beacon transmission. <<<\r\n\r\n");
}

/*
 * ============================================================================
 * MAIN ENTRY POINT (CORTEX-M0+ / CPU2)
 * ============================================================================
 */
int main(void) {
    /*
     * ------------------------------------------------------------------------
     * STAGE 1: Low-Level CPU2 Hardware & Bus Initialization
     * ------------------------------------------------------------------------
     */
    Satellite_Hardware_Init();

    /*
     * ------------------------------------------------------------------------
     * STAGE 2: Satellite Subsystem & Application Initialization
     * ------------------------------------------------------------------------
     */
    Satellite_Init(NULL);

    /* IPCC Initial Handshake with Cortex-M4 */
    IPCC_Handshake();

    /* Retrieve active configuration pointer for runtime parameters and delays */
    const SatelliteConfig_t *cfg = Satellite_GetConfig();

    uint32_t cycle = 0;

    /* Default initial values (used if M4 hasn't populated ring buffer yet) */
    int16_t b1_data[5] = { 400, 400, 250, 250, 250 }; /* 4.00V, 4.00V, 25.0C, 25.0C, 25.0C */
    int16_t b2_data[5] = { 100, 100, 100, 1, 2 };     /* 1.00A, 1.00A, 1.00A, Flag1=1, Flag2=2 */

    /* Arm radio in RX listening mode immediately */
    RadioApp_StartRx();

    /*
     * ------------------------------------------------------------------------
     * STAGE 3: Satellite Mission Executive Loop
     * Sequence: CW1 -> listen -> CW2 -> listen (continuous repeat)
     * ------------------------------------------------------------------------
     */
    while (1) {
        cycle++;

        /* 1. Update latest telemetry from M4 shared ring buffer */
        M0_Drain_Telemetry(b1_data, b2_data);
        M0_Check_And_Forward_Command();

        /* 2. Format and Transmit COMPLETE CW1 (Voltage + Temperature) */
        snprintf(cwBeaconlive_msg1, sizeof(cwBeaconlive_msg1),
                 "%d.%02dV %d.%02dV %d.%dC %d.%dC %d.%dC",
                 b1_data[0] / 100, (int)ABS(b1_data[0] % 100),
                 b1_data[1] / 100, (int)ABS(b1_data[1] % 100),
                 b1_data[2] / 10,  (int)ABS(b1_data[2] % 10),
                 b1_data[3] / 10,  (int)ABS(b1_data[3] % 10),
                 b1_data[4] / 10,  (int)ABS(b1_data[4] % 10));

        uart1_printf("\r\n>>> [CYCLE #%lu] TRANSMITTING COMPLETE CW1: \"%s\" <<<\r\n",
                     (unsigned long)cycle, cwBeaconlive_msg1);
        Satellite_Send_CW_Beacon(cycle,
                                 "CW1",
                                 cfg->cwBeaconCallsign,
                                 cwBeaconlive_msg1,
                                 cfg->morseUnitMs,
                                 cfg->cwTuningCarrierDurationMs,
                                 cfg->cwTuningPostDelayMs,
                                 cfg->cwInterStringDelayMs);

        /* 3. Listen RX after CW1 (30 Minutes) */
        uart1_puts(">>> [M0+] ENTERING RX LISTEN MODE AFTER CW1 <<<\r\n");
        M0_Listening_Delay(SAT_LISTEN_DURATION_MS, b1_data, b2_data);

        /* 4. Update latest telemetry from M4 shared ring buffer */
        M0_Drain_Telemetry(b1_data, b2_data);
        M0_Check_And_Forward_Command();

        /* 5. Format and Transmit COMPLETE CW2 (Currents + Status Flags) */
        snprintf(cwBeaconlive_msg2, sizeof(cwBeaconlive_msg2),
                 "%d.%02dA %d.%02dA %d.%02dA %d %d",
                 b2_data[0] / 100, (int)ABS(b2_data[0] % 100),
                 b2_data[1] / 100, (int)ABS(b2_data[1] % 100),
                 b2_data[2] / 100, (int)ABS(b2_data[2] % 100),
                 (int)b2_data[3],
                 (int)b2_data[4]);

        uart1_printf("\r\n>>> [CYCLE #%lu] TRANSMITTING COMPLETE CW2: \"%s\" <<<\r\n",
                     (unsigned long)cycle, cwBeaconlive_msg2);
        Satellite_Send_CW_Beacon(cycle,
                                 "CW2",
                                 cfg->cwBeaconCallsign,
                                 cwBeaconlive_msg2,
                                 cfg->morseUnitMs,
                                 cfg->cwTuningCarrierDurationMs,
                                 cfg->cwTuningPostDelayMs,
                                 cfg->cwInterStringDelayMs);

        /* 6. Listen RX after CW2 (30 Minutes) */
        uart1_puts(">>> [M0+] ENTERING RX LISTEN MODE AFTER CW2 <<<\r\n");
        M0_Listening_Delay(SAT_LISTEN_DURATION_MS, b1_data, b2_data);
    }

    return 0;
}