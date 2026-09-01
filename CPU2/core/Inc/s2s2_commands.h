/*
 * s2s2_commands.h -- Standard command set and structured telemetry payloads
 * for the S2S2 protocol (cubus_proto).
 *
 * This is the S2S2-format replacement for the legacy AX.25 command/response
 * scheme (SAT-ID + 3-byte magic opcode sequences, per-MCU addressing baked
 * into the payload, a separate CRC-16 per sub-packet). None of that is
 * needed on top of cubus_proto: src_id/dst_id in the 10-byte header already
 * address the satellite, and the frame already carries RS(255,223) FEC +
 * CRC32 + a hardware CRC16 per radio chunk. What's kept from the original
 * design is the *meaning* of each field -- opcodes, subsystem IDs, mission/
 * data identifiers, and ACK/NACK status codes -- just carried in a flat,
 * byte-aligned payload instead of AX.25 framing.
 *
 * Wire layout, all multi-byte fields big-endian:
 *
 *   TYPE_TELECOMMAND payload : [opcode][subsystem][data_id][address(4)][num_packets(2)]
 *                              [counter(4)][mac(8)]  (21 bytes, cubus_command_t --
 *                              counter+mac are s2s2_auth.h's anti-spoof/anti-replay
 *                              layer; see that header before touching this format)
 *   TYPE_ACK payload         : [status][data_id][acked_seq(2)]                            (4 bytes, cubus_ack_t)
 *   TYPE_TELEMETRY payload   : [data_id][ ... one of the structs below ... ]
 *
 * Bare C: no malloc, no libc dependency beyond stdint.h / string.h.
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-30
*/
#ifndef S2S2_COMMANDS_H
#define S2S2_COMMANDS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =====================================================================
 * Standard command opcodes -- TYPE_TELECOMMAND payload byte 0.
 * ===================================================================== */

typedef enum {
    CMD_NONE                 = 0x00,
    CMD_REQUEST_BURST        = 0x01,
    CMD_HEARTBEAT            = 0x02,
    CMD_ABORT_MISSION        = 0x03,

    CMD_RUN_CAMERA           = 0x10,
    CMD_DOWNLOAD_FILE        = 0x11,
    CMD_GET_HEALTH           = 0x12,
    CMD_GET_HOUSEKEEPING     = 0x13,
    CMD_GET_BEACON1          = 0x14,
    CMD_GET_BEACON2          = 0x15,
    CMD_GET_ADCS             = 0x16,
    CMD_DIGIPEATER           = 0x17,
    CMD_GET_SAT_LOG          = 0x18,
    CMD_REQUEST_RETRANSMIT   = 0x19,
} cubus_cmd_t;

/* Subsystem / MCU IDs (unchanged from the original design). */
typedef enum {
    SUBSYS_OBC  = 0x01,
    SUBSYS_COM  = 0x02,
    SUBSYS_ADCS = 0x03,
    SUBSYS_CAM  = 0x04,
    SUBSYS_EPDM = 0x05,
} cubus_subsystem_t;

/* Mission / data identifiers -- what a GET_* command or a telemetry
   packet's payload actually contains. */
typedef enum {
    DATA_ID_HOUSEKEEPING = 0x01,
    DATA_ID_BEACON1       = 0x02,
    DATA_ID_BEACON2       = 0x03,
    DATA_ID_ADCS          = 0x04,
    DATA_ID_CAMERA_RGB    = 0x05,
    DATA_ID_CAMERA_NIR    = 0x06,
    DATA_ID_NDWI          = 0x07,
    DATA_ID_EPDM          = 0x08,
    DATA_ID_DIGIPEATER    = 0x09,
    DATA_ID_SAT_LOG       = 0x0A,
    DATA_ID_GENERAL       = 0xFF,
} cubus_data_id_t;

/* ACK/NACK status codes -- TYPE_ACK payload byte 0. */
typedef enum {
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
    ACK_STATUS_TX_FAILED        = 0x0B,
    ACK_STATUS_AUTH_FAILED      = 0x0C,  /* bad MAC or replayed/stale counter, see s2s2_auth.h */
} cubus_ack_status_t;

/* Human-readable status string, for logging. Never returns NULL. */
const char *ack_status_str(uint8_t status);

/* =====================================================================
 * Command packet -- TYPE_TELECOMMAND payload
 * ===================================================================== */

/* 9 base fields + 4-byte anti-replay counter + 8-byte SipHash-2-4 MAC
   (see s2s2_auth.h) -- every TYPE_TELECOMMAND payload is authenticated,
   no unsigned command variant exists. */
#define COMMAND_AUTH_LEN     12  /* counter(4) + mac(8) */
#define COMMAND_SIGNED_LEN   13  /* opcode..counter, what actually gets MACed */
#define COMMAND_PAYLOAD_LEN  21  /* 9 base + 12 auth */

typedef struct {
    uint8_t  opcode;        /* cubus_cmd_t */
    uint8_t  subsystem;     /* cubus_subsystem_t, 0 if not applicable */
    uint8_t  data_id;       /* cubus_data_id_t, 0 if not applicable */
    uint32_t address;       /* file/memory address for DOWNLOAD_FILE, 0 otherwise */
    uint16_t num_packets;   /* fragment count requested, 0 if not applicable */
    uint32_t counter;       /* anti-replay, ground-station-assigned, see s2s2_auth.h */
    uint8_t  mac[8];        /* SipHash-2-4 over src_id/dst_id + the fields above */
} cubus_command_t;

void command_pack(const cubus_command_t *cmd, uint8_t out[COMMAND_PAYLOAD_LEN]);
int  command_unpack(const uint8_t *buf, uint16_t len, cubus_command_t *out);

/* =====================================================================
 * ACK/NACK packet -- TYPE_ACK payload
 * ===================================================================== */

#define ACK_PAYLOAD_LEN 4

typedef struct {
    uint8_t  status;      /* cubus_ack_status_t */
    uint8_t  data_id;     /* what this ack is about, DATA_ID_GENERAL if n/a */
    uint16_t acked_seq;   /* seq_num of the telecommand being acknowledged */
} cubus_ack_t;

void ack_pack(const cubus_ack_t *ack, uint8_t out[ACK_PAYLOAD_LEN]);
int  ack_unpack(const uint8_t *buf, uint16_t len, cubus_ack_t *out);

/* =====================================================================
 * Beacon 1 -- Power, Battery, Thermal & System Health (data_id = DATA_ID_BEACON1)
 * ===================================================================== */

#define BEACON1_PAYLOAD_LEN 30

typedef struct {
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
} beacon1_t;

void beacon1_pack(const beacon1_t *b, uint8_t out[BEACON1_PAYLOAD_LEN]);
int  beacon1_unpack(const uint8_t *buf, uint16_t len, beacon1_t *out);

/* =====================================================================
 * Beacon 2 -- Solar Panel & ADCS (data_id = DATA_ID_BEACON2)
 * ===================================================================== */

#define BEACON2_PAYLOAD_LEN 25

typedef struct {
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
    uint16_t gyro_x;
    uint16_t gyro_y;
    uint16_t gyro_z;
    uint16_t mag_x;
    uint16_t mag_y;
    uint16_t mag_z;
    uint8_t  power_stat;
} beacon2_t;

void beacon2_pack(const beacon2_t *b, uint8_t out[BEACON2_PAYLOAD_LEN]);
int  beacon2_unpack(const uint8_t *buf, uint16_t len, beacon2_t *out);

/* =====================================================================
 * Camera fragment -- one fragment of a downlinked image
 * (data_id = DATA_ID_CAMERA_RGB / DATA_ID_CAMERA_NIR / DATA_ID_NDWI)
 *
 * cubus_proto's 659-byte MAX_PAYLOAD_LEN gives far more room per fragment
 * than AX.25's ~128-190 bytes did, and the frame already carries RS FEC +
 * CRC32, so the per-fragment CRC-16/X.25 from the original design is
 * dropped as redundant.
 * ===================================================================== */

#define CAMERA_FRAGMENT_HDR_LEN 7
#define CAMERA_FRAGMENT_MAX_DATA 512

typedef enum {
    IMAGE_TYPE_RGB  = 0x00,
    IMAGE_TYPE_NIR  = 0x01,
    IMAGE_TYPE_NDWI = 0x02,
} cubus_image_type_t;

typedef struct {
    uint8_t  image_id;
    uint16_t fragment_index;
    uint16_t total_fragments;
    uint8_t  image_type;    /* cubus_image_type_t */
    uint8_t  ai_score;
    uint16_t data_len;
    const uint8_t *data;    /* pack: points at caller's fragment bytes */
} camera_fragment_t;

typedef struct {
    uint8_t  image_id;
    uint16_t fragment_index;
    uint16_t total_fragments;
    uint8_t  image_type;
    uint8_t  ai_score;
    uint16_t data_len;
    uint8_t  data[CAMERA_FRAGMENT_MAX_DATA]; /* unpack: owns a copy */
} camera_fragment_unpacked_t;

/* out must have room for CAMERA_FRAGMENT_HDR_LEN + frag->data_len bytes.
   Returns the number of bytes written, or -1 if data_len > CAMERA_FRAGMENT_MAX_DATA. */
int camera_fragment_pack(const camera_fragment_t *frag, uint8_t *out);
int camera_fragment_unpack(const uint8_t *buf, uint16_t len, camera_fragment_unpacked_t *out);

#ifdef __cplusplus
}
#endif

#endif /* S2S2_COMMANDS_H */
