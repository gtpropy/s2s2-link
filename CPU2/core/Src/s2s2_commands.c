/*
 * s2s2_commands.c
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-30
 */

#include "s2s2_commands.h"
#include "ax25.h" /* ax25_fcs() -- reused for camera_fragment's CRC-16/X.25 */
#include <string.h>

const char *ack_status_str(uint8_t status) {
    switch (status) {
        case ACK_STATUS_ACCEPTED:         return "command accepted";
        case ACK_STATUS_COMPLETED:        return "command completed";
        case ACK_STATUS_PACKET_ERROR:     return "packet error";
        case ACK_STATUS_CRC_ERROR:        return "CRC/checksum error";
        case ACK_STATUS_INCOMPLETE:       return "incomplete packet";
        case ACK_STATUS_DATA_MISSING:     return "requested data missing";
        case ACK_STATUS_DATA_UNAVAILABLE: return "requested data unavailable";
        case ACK_STATUS_BUSY:             return "satellite busy";
        case ACK_STATUS_MISSION_INACTIVE: return "mission inactive";
        case ACK_STATUS_INVALID_COMMAND:  return "invalid command";
        case ACK_STATUS_MEMORY_ERROR:     return "memory/file error";
        case ACK_STATUS_TX_FAILED:        return "transmission failed";
        case ACK_STATUS_AUTH_FAILED:      return "authentication failed (bad MAC or replay)";
        default:                          return "unknown status";
    }
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}
static uint16_t get_u16(const uint8_t *p) { return (uint16_t)((p[0] << 8) | p[1]); }
static uint32_t get_u32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* CRC-8, poly 0x07, init 0x00, no reflect, no final XOR -- used only for
   beacon1_t/beacon2_t's CHK_CRC (see s2s2_commands.h). */
static uint8_t crc8(const uint8_t *data, uint16_t len) {
    uint8_t crc = 0x00;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* ---- Command ---- */

void command_pack(const cubus_command_t *cmd, uint8_t out[COMMAND_PAYLOAD_LEN]) {
    out[0] = SAT_ID_S2S2;
    out[1] = cmd->mcu_id;
    out[2] = cmd->opcode_b1;
    if (cmd->opcode_b1 == CMD_RUN_CAMERA) {
        out[3] = 0x5E;
        out[4] = 0xBD;
    } else if (cmd->opcode_b1 == CMD_DOWNLOAD_FILE) {
        out[3] = 0xD1;
        out[4] = cmd->opcode_b3; /* target file number, caller-supplied */
    } else {
        out[3] = cmd->opcode_b2;
        out[4] = cmd->opcode_b3;
    }
    put_u16(&out[5], cmd->data_id);
    put_u32(&out[7], cmd->address);
    put_u16(&out[11], cmd->num_packets);
    put_u32(&out[13], cmd->counter);
    memcpy(&out[17], cmd->mac, 8);
}

int command_unpack(const uint8_t *buf, uint16_t len, cubus_command_t *out) {
    if (len < COMMAND_PAYLOAD_LEN) return -1;
    out->sat_id = buf[0];
    out->mcu_id = buf[1];
    out->opcode_b1 = buf[2];
    out->opcode_b2 = buf[3];
    out->opcode_b3 = buf[4];
    out->data_id = get_u16(&buf[5]);
    out->address = get_u32(&buf[7]);
    out->num_packets = get_u16(&buf[11]);
    out->counter = get_u32(&buf[13]);
    memcpy(out->mac, &buf[17], 8);
    return 0;
}

/* ---- ACK ---- */

void ack_pack(const cubus_ack_t *ack, uint8_t out[ACK_PAYLOAD_LEN]) {
    out[0] = (ack->status == ACK_STATUS_ACCEPTED || ack->status == ACK_STATUS_COMPLETED)
                 ? ACK_RESPONSE_TYPE_ACK : ACK_RESPONSE_TYPE_NACK;
    out[1] = ack->status;
    out[2] = ack->data_id;
}

int ack_unpack(const uint8_t *buf, uint16_t len, cubus_ack_t *out) {
    if (len < ACK_PAYLOAD_LEN) return -1;
    out->status = buf[1];
    out->data_id = buf[2];
    return 0;
}

/* ---- Beacon 1 ---- */

void beacon1_pack(const beacon1_t *b, uint8_t out[BEACON1_PAYLOAD_LEN]) {
    uint8_t *p = out;
    *p++ = BEACON1_HEAD;
    *p++ = b->type;
    put_u16(p, b->tim_day); p += 2;
    *p++ = b->tim_hour;
    put_u16(p, b->bat_v); p += 2;
    put_u16(p, b->bat_i); p += 2;
    *p++ = b->bat_temp;
    *p++ = b->raw_v;
    *p++ = b->raw_i;
    put_u16(p, b->sol_tot_v); p += 2;
    *p++ = b->sol_tot_i;
    *p++ = b->unreg_i;
    *p++ = b->main_3v3_i;
    *p++ = b->msn_3v3_i;
    *p++ = b->v5_i;
    *p++ = b->adc_3v32_i;
    *p++ = b->temp1;
    *p++ = b->temp2;
    *p++ = b->temp3;
    *p++ = b->temp4;
    *p++ = b->temp5;
    *p++ = b->bpb_t;
    *p++ = b->ant_p_t;
    *p++ = b->obc_com_t;
    *p++ = b->system_stat;
    *p++ = b->mcu_stat;
    *p++ = b->adc_stat;
    *p = crc8(out, (uint16_t)(p - out));
}

int beacon1_unpack(const uint8_t *buf, uint16_t len, beacon1_t *out) {
    if (len < BEACON1_PAYLOAD_LEN) return -1;
    const uint8_t *p = buf;
    out->head = *p++;
    out->type = *p++;
    out->tim_day = get_u16(p); p += 2;
    out->tim_hour = *p++;
    out->bat_v = get_u16(p); p += 2;
    out->bat_i = get_u16(p); p += 2;
    out->bat_temp = *p++;
    out->raw_v = *p++;
    out->raw_i = *p++;
    out->sol_tot_v = get_u16(p); p += 2;
    out->sol_tot_i = *p++;
    out->unreg_i = *p++;
    out->main_3v3_i = *p++;
    out->msn_3v3_i = *p++;
    out->v5_i = *p++;
    out->adc_3v32_i = *p++;
    out->temp1 = *p++;
    out->temp2 = *p++;
    out->temp3 = *p++;
    out->temp4 = *p++;
    out->temp5 = *p++;
    out->bpb_t = *p++;
    out->ant_p_t = *p++;
    out->obc_com_t = *p++;
    out->system_stat = *p++;
    out->mcu_stat = *p++;
    out->adc_stat = *p++;
    out->chk_crc = *p++;
    return 0;
}

/* ---- Beacon 2 ---- */

void beacon2_pack(const beacon2_t *b, uint8_t out[BEACON2_PAYLOAD_LEN]) {
    uint8_t *p = out;
    *p++ = BEACON2_HEAD;
    *p++ = b->type;
    put_u16(p, b->tim_day); p += 2;
    *p++ = b->sol_p1_v;
    *p++ = b->sol_p2_v;
    *p++ = b->sol_p3_v;
    *p++ = b->sol_p4_v;
    *p++ = b->sol_p5_v;
    *p++ = b->sol_p1_i;
    *p++ = b->sol_p2_i;
    *p++ = b->sol_p3_i;
    *p++ = b->sol_p4_i;
    *p++ = b->sol_p5_i;
    *p++ = b->gyro_x;
    *p++ = b->gyro_y;
    *p++ = b->gyro_z;
    *p++ = b->mag_x;
    *p++ = b->mag_y;
    *p++ = b->mag_z;
    *p++ = b->power_stat;
    *p = crc8(out, (uint16_t)(p - out));
}

int beacon2_unpack(const uint8_t *buf, uint16_t len, beacon2_t *out) {
    if (len < BEACON2_PAYLOAD_LEN) return -1;
    const uint8_t *p = buf;
    out->head = *p++;
    out->type = *p++;
    out->tim_day = get_u16(p); p += 2;
    out->sol_p1_v = *p++;
    out->sol_p2_v = *p++;
    out->sol_p3_v = *p++;
    out->sol_p4_v = *p++;
    out->sol_p5_v = *p++;
    out->sol_p1_i = *p++;
    out->sol_p2_i = *p++;
    out->sol_p3_i = *p++;
    out->sol_p4_i = *p++;
    out->sol_p5_i = *p++;
    out->gyro_x = *p++;
    out->gyro_y = *p++;
    out->gyro_z = *p++;
    out->mag_x = *p++;
    out->mag_y = *p++;
    out->mag_z = *p++;
    out->power_stat = *p++;
    out->chk_crc = *p++;
    return 0;
}

/* ---- Camera fragment ---- */

int camera_fragment_pack(const camera_fragment_t *frag, uint8_t *out) {
    if (frag->data_len > CAMERA_FRAGMENT_MAX_DATA) return -1;
    uint8_t *p = out;
    *p++ = CAMERA_FRAGMENT_PACKET_TYPE;
    *p++ = frag->image_id;
    put_u16(p, frag->fragment_index); p += 2;
    put_u16(p, frag->total_fragments); p += 2;
    *p++ = frag->image_type;
    *p++ = frag->ai_score;
    if (frag->data_len > 0) memcpy(p, frag->data, frag->data_len);
    p += frag->data_len;
    uint16_t total = (uint16_t)(CAMERA_FRAGMENT_HDR_LEN + frag->data_len);
    uint16_t crc = ax25_fcs(out, total);
    p[0] = (uint8_t)(crc >> 8);
    p[1] = (uint8_t)crc;
    return total + CAMERA_FRAGMENT_CRC_LEN;
}

int camera_fragment_unpack(const uint8_t *buf, uint16_t len, camera_fragment_unpacked_t *out) {
    if (len < CAMERA_FRAGMENT_HDR_LEN + CAMERA_FRAGMENT_CRC_LEN) return -1;
    if (buf[0] != CAMERA_FRAGMENT_PACKET_TYPE) return -1;
    uint16_t body_len = (uint16_t)(len - CAMERA_FRAGMENT_CRC_LEN);
    uint16_t received_crc = get_u16(&buf[body_len]);
    if (ax25_fcs(buf, body_len) != received_crc) return -1;

    const uint8_t *p = &buf[1];
    out->image_id = *p++;
    out->fragment_index = get_u16(p); p += 2;
    out->total_fragments = get_u16(p); p += 2;
    out->image_type = *p++;
    out->ai_score = *p++;
    uint16_t data_len = (uint16_t)(body_len - CAMERA_FRAGMENT_HDR_LEN);
    if (data_len > CAMERA_FRAGMENT_MAX_DATA) return -1;
    out->data_len = data_len;
    if (data_len > 0) memcpy(out->data, p, data_len);
    return 0;
}
