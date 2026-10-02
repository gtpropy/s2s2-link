#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * ============================================================
 * FREQUENCIES
 * ============================================================
 *
 * Satellite:
 *
 *     TX = 435.000 MHz
 *     RX = 437.375 MHz
 *
 * Ground station must use:
 *
 *     TX = 437.375 MHz
 *     RX = 435.000 MHz
 * ============================================================
 */

#define DEFAULT_TX_FREQUENCY_HZ        437375000UL
#define DEFAULT_RX_FREQUENCY_HZ        435000000UL

/*
 * ============================================================
 * DEFAULT AX.25 ADDRESS
 * ============================================================
 */

#define DEFAULT_SOURCE_CALLSIGN       "9NS2S2"
#define DEFAULT_SOURCE_SSID            1

#define DEFAULT_DEST_CALLSIGN         "GROUND"
#define DEFAULT_DEST_SSID              0

/*
 * ============================================================
 * GFSK / G3RUH
 * ============================================================
 */
// Full +14 dBm drive power: yields full +24.5 dBm on 3.3V PA and +27.0 dBm on 5V PA
#define RADIO_TX_POWER_DBM             14

/*
 * GFSK External Power Amplifier Selection:
 *   1: 5V External PA (PC3 / SI2) with 5V DC/DC Boost (PA0) [High Power Flight Mode: ~26.5 dBm]
 *   0: 3.3V External PA (PC2 / SO2) [Safe Bench / ST-Link USB Mode: ~24.5 dBm, <170 mA]
 */
#ifndef SAT_CFG_USE_5V_PA_FOR_GFSK
#define SAT_CFG_USE_5V_PA_FOR_GFSK     0
#endif

/*
 * GFSK / G3RUH data rate: 4800 bps (matched to Ground Station).
 */
#define RADIO_BIT_RATE_BPS             4800

/*
 * GFSK frequency deviation (+/- 1.2 kHz for 4800 bps, BT 0.5, h=0.5).
 */
#define RADIO_FDEV_HZ                  1200

/*
 * SX126x GFSK bandwidth.
 */
#define RADIO_RX_BANDWIDTH_HZ         23400

/*
 * 128 bits = 16 bytes.
 */
#define RADIO_PREAMBLE_LENGTH_BITS     128

#define RADIO_TX_TIMEOUT_MS            3000

/*
 * ============================================================
 * PHYSICAL PACKET LENGTH
 *
 * The SX126x packet is deliberately fixed length.
 *
 * This is NOT an AX.25 requirement.
 * It is our radio transport configuration.
 * ============================================================
 */

#define RADIO_FIXED_PACKET_LEN         200

/*
 * Number of HDLC flags before actual frame.
 * 24 flags = 192 bits = 40 ms audio lock time at 4800 baud (Direwolf optimal).
 */
#define PROTOCOL_LEADING_FLAG_COUNT    24

/*
 * ============================================================
 * RADIO CONFIG
 * ============================================================
 */

typedef struct
{
    uint32_t txFrequency;
    uint32_t rxFrequency;

    const char *sourceCallsign;
    uint8_t sourceSSID;

    const char *destCallsign;
    uint8_t destSSID;

    bool isSatelliteMode;

} RadioConfig_t;

/*
 * ============================================================
 * COMMANDS
 * ============================================================
 */

typedef enum
{
    CMD_NONE               = 0x00,
    CMD_REQUEST_BURST      = 0x01,
    CMD_ACK                = 0x02,
    CMD_NACK               = 0x03,
    CMD_HEARTBEAT          = 0x04,
    CMD_ABORT_MISSION      = 0x05,
    CMD_REQUEST_RETRANSMIT = 0x06,

    CMD_RUN_CAMERA         = 0xCC, /* literal opcode_b1 -- Packet Format sheet row 194 (b2=0x5E b3=0xBD, see command_pack()) */
    CMD_DOWNLOAD_FILE      = 0x1D, /* literal opcode_b1 -- sheet rows 222/228/259 (b2=0xD1, b3=target file number) */
    CMD_GET_HEALTH         = 0x12,
    CMD_GET_HOUSEKEEPING   = 0x13,
    CMD_GET_BEACON1        = 0x14,
    CMD_GET_BEACON2        = 0x15,
    CMD_GET_ADCS           = 0x16,
    CMD_DIGIPEATER         = 0x17,
    CMD_GET_SAT_LOG        = 0x18

} CommandOpcode_t;

/* Subsystem / data identifiers for Send_Command_Ex's 3-byte payload
   (gs_main.c) -- ground-console-side only, since the satellite's own
   command dispatch (CPU1/NuttX) is outside this build. */
typedef enum
{
    SUBSYS_OBC  = 0x01,
    SUBSYS_COM  = 0x02,
    SUBSYS_ADCS = 0x03,
    SUBSYS_CAM  = 0x04,
    SUBSYS_EPDM = 0x05
} CommandSubsystem_t;

typedef enum
{
    DATA_ID_HOUSEKEEPING = 0x01,
    DATA_ID_BEACON1      = 0x02,
    DATA_ID_BEACON2      = 0x03,
    DATA_ID_ADCS         = 0x04,
    DATA_ID_CAMERA_RGB   = 0x05,
    DATA_ID_SAT_LOG      = 0x0A,
    DATA_ID_DIGIPEATER   = 0x09,
    DATA_ID_GENERAL      = 0xFF
} CommandDataId_t;

/* Fixed protocol constant -- Packet Format sheet's SAT-ID byte, literal
   ASCII 'S', the same value in every worked command example. */
#define SAT_ID_S2S2 0x53

/*
 * ============================================================
 * AUTH TOGGLE
 *
 * AUTH=TRUE (default) or AUTH=FALSE at the Makefile command line
 * (`make AUTH=FALSE ...`) controls whether the light FEC wrap's keyed
 * tag + replay counter are actually computed/checked (see Mission/
 * protocol.c). The FEC/correction layer itself is always on either
 * way -- this only toggles the authentication half of it, for bench
 * testing against a peer that doesn't have the shared key yet.
 * ============================================================
 */
#ifndef AUTH_ENABLE
#define AUTH_ENABLE 1
#endif

/*
 * ============================================================
 * ACK / NACK -- Packet Format Sheet2 rows 1-75
 * ============================================================
 */

#define ACK_RESPONSE_TYPE_ACK  0xAC
#define ACK_RESPONSE_TYPE_NACK 0xEE

typedef enum
{
    ACK_STATUS_ACCEPTED         = 0x00,
    ACK_STATUS_COMPLETED        = 0x01,
    ACK_STATUS_PACKET_ERROR     = 0x02,
    ACK_STATUS_CRC_ERROR        = 0x03,
    ACK_STATUS_INCOMPLETE       = 0x04,
    ACK_STATUS_DATA_MISSING     = 0x05,
    ACK_STATUS_DATA_UNAVAILABLE = 0x06,
    ACK_STATUS_BUSY             = 0x07,
    ACK_STATUS_MISSION_INACTIVE = 0x08,
    ACK_STATUS_INVALID_COMMAND  = 0x09,
    ACK_STATUS_MEMORY_ERROR     = 0x0A,
    ACK_STATUS_TX_FAILED        = 0x0B
} AckStatus_t;

#define ACK_PAYLOAD_LEN 3

/* response_type (byte 0) is derived from status at pack time (see
   ack_pack() in satellite_app.c / gs_main.c): ACCEPTED/COMPLETED ->
   ACK_RESPONSE_TYPE_ACK, every error code -> ACK_RESPONSE_TYPE_NACK. */
typedef struct
{
    uint8_t status;   /* AckStatus_t */
    uint8_t data_id;  /* CommandDataId_t, DATA_ID_GENERAL if n/a */
} AckPacket_t;

/*
 * ============================================================
 * COMMAND -- Packet Format Sheet1/Sheet2, SAT-ID+MCU-ID+3-opcode scheme
 *
 * 13 bytes on the wire. No separate counter/MAC fields here -- the
 * light FEC wrap (Mission/protocol.c) already adds its own replay
 * counter + keyed tag to EVERY packet transparently, so duplicating
 * that inside the command payload itself would just be redundant
 * overhead for no extra protection.
 * ============================================================
 */

#define COMMAND_PAYLOAD_LEN 13

typedef struct
{
    uint8_t  sat_id;       /* SAT_ID_S2S2, set by command_pack() */
    uint8_t  mcu_id;       /* CommandSubsystem_t */
    uint8_t  opcode_b1;    /* CommandOpcode_t */
    uint8_t  opcode_b2;    /* command-specific, see CommandOpcode_t comment; 0 if n/a */
    uint8_t  opcode_b3;    /* command-specific, see CommandOpcode_t comment; 0 if n/a */
    uint16_t data_id;      /* CommandDataId_t, 0 if n/a */
    uint32_t address;      /* file/memory address for CMD_DOWNLOAD_FILE, 0 otherwise */
    uint16_t num_packets;  /* fragment count requested, 0 if n/a */
} CommandPacket_t;

/*
 * ============================================================
 * BEACON 1 -- Power, Battery, Thermal & System Health
 * Packet Format Sheet2 rows 109-138. HEAD/CHK_CRC bracket the payload
 * (HEAD mirrors the 0xB1 "Information Bytes" marker the sheet's AX.25-
 * framing example uses for Beacon1; CHK_CRC is CRC-8, poly 0x07, over
 * every preceding byte). TYPE is a 4-bit field the sheet never gives a
 * value table for -- kept as a full byte, default 1 (schema version).
 *
 * Only tim_hour/bat_v/sol_tot_v/bat_temp/ant_p_t/bpb_t have a real data
 * source today (the 5-value Beacon-1 IPC packet from CPU1, Sring_
 * buffer.h) -- every other field is honestly zeroed, not fabricated,
 * until real sensors/telemetry exist for them.
 * ============================================================
 */

#define BEACON1_HEAD 0xB1
#define BEACON1_PAYLOAD_LEN 32

typedef struct
{
    uint8_t  head;
    uint8_t  type;
    uint16_t tim_day;
    uint8_t  tim_hour;
    uint16_t bat_v;
    uint16_t bat_i;
    uint8_t  bat_temp;
    uint8_t  raw_v;
    uint8_t  raw_i;
    uint16_t sol_tot_v;
    uint8_t  sol_tot_i;
    uint8_t  unreg_i;
    uint8_t  main_3v3_i;
    uint8_t  msn_3v3_i;
    uint8_t  v5_i;
    uint8_t  adc_3v32_i;
    uint8_t  temp1;
    uint8_t  temp2;
    uint8_t  temp3;
    uint8_t  temp4;
    uint8_t  temp5;
    uint8_t  bpb_t;
    uint8_t  ant_p_t;
    uint8_t  obc_com_t;
    uint8_t  system_stat;
    uint8_t  mcu_stat;
    uint8_t  adc_stat;
    uint8_t  chk_crc;   /* set by beacon1_pack(), ignored on input to it */
} Beacon1Packet_t;

/*
 * ============================================================
 * BEACON 2 -- Solar Panel & ADCS
 * Packet Format Sheet2 rows 109-138 (right column). Same HEAD/TYPE/
 * CHK_CRC framing as Beacon 1. gyro_x/y/z and mag_x/y/z are 8-bit each
 * per the sheet (matches Sheet4's CW-beacon-2 fields).
 *
 * Only sol_tot_i/power_stat have a real data source today (the 5-value
 * Beacon-2 IPC packet); per-panel solar V/I and gyro/magnetometer have
 * no source in this build yet (ADCS/per-panel telemetry isn't wired up
 * CPU1-side) -- honestly zeroed rather than fabricated.
 * ============================================================
 */

#define BEACON2_HEAD 0xB2
#define BEACON2_PAYLOAD_LEN 22

typedef struct
{
    uint8_t  head;
    uint8_t  type;
    uint16_t tim_day;
    uint8_t  sol_p1_v;
    uint8_t  sol_p2_v;
    uint8_t  sol_p3_v;
    uint8_t  sol_p4_v;
    uint8_t  sol_p5_v;
    uint8_t  sol_p1_i;
    uint8_t  sol_p2_i;
    uint8_t  sol_p3_i;
    uint8_t  sol_p4_i;
    uint8_t  sol_p5_i;
    uint8_t  gyro_x;
    uint8_t  gyro_y;
    uint8_t  gyro_z;
    uint8_t  mag_x;
    uint8_t  mag_y;
    uint8_t  mag_z;
    uint8_t  power_stat;
    uint8_t  chk_crc;   /* set by beacon2_pack(), ignored on input to it */
} Beacon2Packet_t;

/*
 * ============================================================
 * CW1 / CW2 -- bit-packed Morse housekeeping, Packet Format Sheet4
 * (against BIRDS4_CW_Format_Timing.pdf's "project name + call sign +
 * housekeeping-as-hex-digits" framing). Distinct from Beacon1Packet_t/
 * Beacon2Packet_t above -- this is a much coarser set meant to be
 * copied by ear with no demodulator: pack into 8 raw bytes, hex-encode
 * as 16 uppercase ASCII digit characters, Morse-send "<project>
 * <callsign> <hex...>". Sheet4's own "SOLAR CELL" list repeats "+Y"
 * twice -- BIRDS4's own reference table lists +X/-Y/-Z/+Y/+Z, used
 * here as the corrected 5-cell set.
 * ============================================================
 */

#define CW_BEACON_PACKED_LEN 8
#define CW_BEACON_HEX_LEN    16

typedef struct
{
    uint8_t  system_stat;      /* 2-bit, low 2 bits used */
    uint8_t  mission_status;   /* 2-bit, low 2 bits used */
    uint8_t  time_after_reset; /* 4-bit, low 4 bits used (hours mod 16) */
    uint8_t  sol_total_i;
    uint16_t sol_total_v;
    uint16_t bat_v;
    uint16_t bat_i;
} CwBeacon1_t;

typedef struct
{
    uint8_t gyro_x;
    uint8_t gyro_y;
    uint8_t gyro_z;
    uint8_t mag_x;
    uint8_t mag_y;
    uint8_t mag_z;
    uint8_t sol_plus_x  : 1;
    uint8_t sol_minus_y : 1;
    uint8_t sol_minus_z : 1;
    uint8_t sol_plus_y  : 1;
    uint8_t sol_plus_z  : 1;
    uint8_t bat_temp;
} CwBeacon2_t;

/*
 * ============================================================
 * CAMERA FRAGMENT -- Packet Format Sheet3. Leading packet-type byte
 * (redundant with the outer data_id but keeps a standalone fragment
 * dump self-identifying) + trailing CRC-16/X.25 (exactly AX25_
 * CalculateCRC's own poly/init/final-XOR -- camera_fragment_pack()
 * calls that directly rather than duplicating it).
 *
 * Struct/pack only for now -- no camera driver exists in this build to
 * actually generate image data, so nothing calls this yet.
 * ============================================================
 */

#define CAMERA_FRAGMENT_PACKET_TYPE 0x08
#define CAMERA_FRAGMENT_HDR_LEN 8   /* packet_type + image_id + frag_idx(2) + total_frag(2) + image_type + ai_score */
#define CAMERA_FRAGMENT_CRC_LEN 2
#define CAMERA_FRAGMENT_MAX_DATA 128  /* Protocol_CreatePacket's real per-frame payload cap (107B) - 10B overhead, rounded down */

typedef enum
{
    IMAGE_TYPE_RGB  = 0x00,
    IMAGE_TYPE_NIR  = 0x01,
    IMAGE_TYPE_NDWI = 0x02
} ImageType_t;

typedef struct
{
    uint8_t  image_id;
    uint16_t fragment_index;
    uint16_t total_fragments;
    uint8_t  image_type;   /* ImageType_t */
    uint8_t  ai_score;
    uint16_t data_len;
    const uint8_t *data;
} CameraFragment_t;

#ifdef __cplusplus
}
#endif

#endif