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
 * Wire layout, all multi-byte fields big-endian. Updated 2026-09-30 against
 * "Packet Format" (SAT-ID/MCU-ID/3-byte-opcode command scheme, response-
 * type-prefixed ACK/NACK, HEAD/CHK_CRC-framed beacons) -- see per-struct
 * comments below for exactly what changed and, where the source sheet was
 * ambiguous or only gave 1-2 worked examples, what was assumed and why:
 *
 *   TYPE_TELECOMMAND payload : [sat_id][mcu_id][opcode_b1][opcode_b2][opcode_b3]
 *                              [data_id(2)][address(4)][num_packets(2)]
 *                              [counter(4)][mac(8)]  (25 bytes, cubus_command_t --
 *                              counter+mac are s2s2_auth.h's anti-spoof/anti-replay
 *                              layer; see that header before touching this format)
 *   TYPE_ACK payload         : [response_type][status][data_id]                    (3 bytes, cubus_ack_t --
 *                              response_type (0xAC ack / 0xEE nack) is DERIVED from
 *                              status at pack time, not stored separately; acked_seq
 *                              dropped -- this project's command protocol is
 *                              synchronous (one in-flight command at a time), so the
 *                              outer cubus_packet_t header's own seq_num is enough)
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
 * Standard command opcodes -- TYPE_TELECOMMAND payload's opcode_b1.
 *
 * The source sheet's command wire format has 3 opcode bytes (opcode_b1/
 * b2/b3), but only two commands have a documented literal example triple
 * (Packet Format Sheet2 rows 194/222/228/259): RUN_CAMERA = CC 5E BD, and
 * DOWNLOAD_FILE = 1D D1/D2 F2/F5/F8 (b3 there tracks the target file
 * number, e.g. F2=sat_health.txt -- not a fixed per-command constant).
 * Every other command has no documented triple at all, so it keeps its
 * historical single-byte value in opcode_b1 with b2/b3 zero-filled --
 * inventing undocumented triples for those would be guessing, not
 * implementing the spec. command_pack() below fills b2/b3 for RUN_CAMERA/
 * DOWNLOAD_FILE automatically; see its comment for exactly what it sends. */
typedef enum {
    CMD_NONE                 = 0x00,
    CMD_REQUEST_BURST        = 0x01,
    CMD_HEARTBEAT            = 0x02,
    CMD_ABORT_MISSION        = 0x03,

    CMD_RUN_CAMERA           = 0xCC, /* literal opcode_b1, see comment above */
    CMD_DOWNLOAD_FILE        = 0x1D, /* literal opcode_b1, see comment above */
    CMD_GET_HEALTH           = 0x12,
    CMD_GET_HOUSEKEEPING     = 0x13,
    CMD_GET_BEACON1          = 0x14,
    CMD_GET_BEACON2          = 0x15,
    CMD_GET_ADCS             = 0x16,
    CMD_DIGIPEATER           = 0x17,
    CMD_GET_SAT_LOG          = 0x18,
    CMD_REQUEST_RETRANSMIT   = 0x19,
} cubus_cmd_t;

/* Fixed protocol constant -- Packet Format sheet's SAT-ID byte, literal
   ASCII 'S' (0x53), the same value in every worked command example. */
#define SAT_ID_S2S2 0x53

/* Subsystem / MCU IDs (unchanged from the original design -- these
   already match the sheet's MCU_ID examples: OBC=1, CAM=4). */
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

/* 13 base fields (sat_id, mcu_id, 3 opcode bytes, data_id(2), address(4),
   num_packets(2)) + 4-byte anti-replay counter + 8-byte SipHash-2-4 MAC
   (see s2s2_auth.h) -- every TYPE_TELECOMMAND payload is authenticated,
   no unsigned command variant exists. */
#define COMMAND_AUTH_LEN     12  /* counter(4) + mac(8) */
#define COMMAND_BASE_LEN     13  /* sat_id..num_packets, before auth */
#define COMMAND_SIGNED_LEN   17  /* sat_id..counter, what actually gets MACed */
#define COMMAND_PAYLOAD_LEN  25  /* 13 base + 12 auth */

typedef struct {
    uint8_t  sat_id;         /* SAT_ID_S2S2, set by command_pack() */
    uint8_t  mcu_id;         /* cubus_subsystem_t, 0 if not applicable */
    uint8_t  opcode_b1;      /* cubus_cmd_t */
    uint8_t  opcode_b2;      /* command-specific parameter, see cubus_cmd_t comment; 0 if n/a */
    uint8_t  opcode_b3;      /* command-specific parameter, see cubus_cmd_t comment; 0 if n/a */
    uint16_t data_id;        /* cubus_data_id_t, 0 if not applicable */
    uint32_t address;        /* file/memory address for DOWNLOAD_FILE, 0 otherwise */
    uint16_t num_packets;    /* fragment count requested, 0 if not applicable */
    uint32_t counter;        /* anti-replay, ground-station-assigned, see s2s2_auth.h */
    uint8_t  mac[8];         /* SipHash-2-4 over sat_id/mcu_id + the fields above */
} cubus_command_t;

/* Sets out->sat_id = SAT_ID_S2S2 always. For CMD_RUN_CAMERA, forces
   opcode_b2/b3 to the literal documented example (0x5E/0xBD) regardless
   of what cmd->opcode_b2/b3 were set to -- there's only one known-correct
   triple for that command. For CMD_DOWNLOAD_FILE, forces opcode_b2 to the
   documented default (0xD1); opcode_b3 (target file number) is passed
   through from cmd->opcode_b3 since no fixed value exists for it (see
   cubus_cmd_t comment) -- caller sets cmd->opcode_b3 to the target file
   number before calling. Every other opcode passes opcode_b2/b3 through
   unchanged (0 unless the caller has a reason to set them). */
void command_pack(const cubus_command_t *cmd, uint8_t out[COMMAND_PAYLOAD_LEN]);
int  command_unpack(const uint8_t *buf, uint16_t len, cubus_command_t *out);

/* =====================================================================
 * ACK/NACK packet -- TYPE_ACK payload
 * ===================================================================== */

#define ACK_RESPONSE_TYPE_ACK  0xAC
#define ACK_RESPONSE_TYPE_NACK 0xEE

#define ACK_PAYLOAD_LEN 3

/* acked_seq dropped (was byte 3 of the old 4-byte payload) -- see
   s2s2_commands.h's top-of-file wire-layout comment for why. */
typedef struct {
    uint8_t  status;      /* cubus_ack_status_t */
    uint8_t  data_id;     /* what this ack is about, DATA_ID_GENERAL if n/a */
} cubus_ack_t;

/* out[0] (response_type) is derived from status, not stored in the
   struct: ACK_STATUS_ACCEPTED/COMPLETED -> ACK_RESPONSE_TYPE_ACK
   (0xAC), everything else (every error code) -> ACK_RESPONSE_TYPE_NACK
   (0xEE). This matches the status table's own accepted/completed-vs-
   error split rather than one inconsistent worked example in the source
   sheet (its "ACK command completed" row literally shows byte0=0xEE). */
void ack_pack(const cubus_ack_t *ack, uint8_t out[ACK_PAYLOAD_LEN]);
int  ack_unpack(const uint8_t *buf, uint16_t len, cubus_ack_t *out);

/* =====================================================================
 * Beacon 1 -- Power, Battery, Thermal & System Health (data_id = DATA_ID_BEACON1)
 * ===================================================================== */

/* HEAD/CHK_CRC bracket the payload (Packet Format Sheet2 rows 111/138) --
   HEAD is a fixed per-beacon marker (mirrors the 0xB1/0xB2 "Information
   Bytes" value the same sheet's AX.25-framing example uses for Beacon1/2,
   so a standalone dump of this payload is still self-identifying), TYPE
   is a 4-bit field the sheet doesn't define a value table for (kept as a
   full byte on the wire per the sheet's own byte numbering; defaults to
   schema version 1 -- TODO: confirm the real TYPE enum), and CHK_CRC is
   CRC-8 (poly 0x07, init 0x00) over every preceding payload byte -- see
   beacon_crc8() in s2s2_commands.c. */
#define BEACON1_HEAD 0xB1
#define BEACON1_PAYLOAD_LEN 32

typedef struct {
    uint8_t  head;    /* BEACON1_HEAD, set by beacon1_pack() */
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
    uint8_t  chk_crc; /* set by beacon1_pack(), ignored on input to it */
} beacon1_t;

void beacon1_pack(const beacon1_t *b, uint8_t out[BEACON1_PAYLOAD_LEN]);
int  beacon1_unpack(const uint8_t *buf, uint16_t len, beacon1_t *out);

/* =====================================================================
 * Beacon 2 -- Solar Panel & ADCS (data_id = DATA_ID_BEACON2)
 * ===================================================================== */

/* Same HEAD/TYPE/CHK_CRC framing as Beacon 1 above. gyro_x/y/z and
   mag_x/y/z are now 8-bit each (was 16-bit) -- Packet Format Sheet2's
   Beacon-2 byte table lists them as 8-bit, matching Sheet4's CW-beacon-2
   gyro/magneto fields, a real precision reduction from the previous
   design, not a typo. */
#define BEACON2_HEAD 0xB2
#define BEACON2_PAYLOAD_LEN 22

typedef struct {
    uint8_t  head;    /* BEACON2_HEAD, set by beacon2_pack() */
    uint8_t  type;    /* see beacon1_t.type comment -- same caveat applies */
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
    uint8_t  chk_crc; /* set by beacon2_pack(), ignored on input to it */
} beacon2_t;

void beacon2_pack(const beacon2_t *b, uint8_t out[BEACON2_PAYLOAD_LEN]);
int  beacon2_unpack(const uint8_t *buf, uint16_t len, beacon2_t *out);

/* =====================================================================
 * Camera fragment -- one fragment of a downlinked image
 * (data_id = DATA_ID_CAMERA_RGB / DATA_ID_CAMERA_NIR / DATA_ID_NDWI)
 *
 * Packet Format Sheet3 adds a leading packet-type byte (0x08, redundant
 * with the outer data_id but kept so a standalone fragment dump is still
 * self-identifying) and a trailing CRC-16/X.25 -- previously dropped here
 * as redundant with the outer frame's RS FEC/CRC32, restored now that the
 * sheet asks for it explicitly. CRC-16/X.25 is exactly ax25.c's own
 * ax25_fcs() algorithm (same poly/init/final-XOR) -- camera_fragment_pack/
 * unpack call it directly rather than duplicating the implementation.
 * ===================================================================== */

#define CAMERA_FRAGMENT_PACKET_TYPE 0x08

#define CAMERA_FRAGMENT_HDR_LEN 8   /* packet_type + image_id + frag_idx(2) + total_frag(2) + image_type + ai_score */
#define CAMERA_FRAGMENT_CRC_LEN 2
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

/* out must have room for CAMERA_FRAGMENT_HDR_LEN + frag->data_len +
   CAMERA_FRAGMENT_CRC_LEN bytes (out[0] = CAMERA_FRAGMENT_PACKET_TYPE,
   last 2 bytes = ax25_fcs() over everything before them). Returns the
   number of bytes written, or -1 if data_len > CAMERA_FRAGMENT_MAX_DATA. */
int camera_fragment_pack(const camera_fragment_t *frag, uint8_t *out);
/* Verifies the trailing CRC-16/X.25 and the leading packet-type byte;
   returns -1 on either mismatch, same as a truncated/malformed buffer. */
int camera_fragment_unpack(const uint8_t *buf, uint16_t len, camera_fragment_unpacked_t *out);

#ifdef __cplusplus
}
#endif

#endif /* S2S2_COMMANDS_H */
