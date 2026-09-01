/*
 * ax25.c
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-21
 */

#include "ax25.h"
#include <string.h>
#include <ctype.h>

void ax25_addr_encode(const ax25_addr_t *addr, int is_last, int command_bit,
                       uint8_t out[AX25_ADDR_LEN]) {
    char padded[AX25_CALLSIGN_MAX];
    for (int i = 0; i < AX25_CALLSIGN_MAX; i++) {
        char c = addr->callsign[i];
        padded[i] = c ? (char)toupper((unsigned char)c) : ' ';
    }
    for (int i = 0; i < AX25_CALLSIGN_MAX; i++) {
        out[i] = (uint8_t)(padded[i] << 1);
    }
    uint8_t ssid_byte = (uint8_t)((addr->ssid & 0x0F) << 1);
    ssid_byte |= 0x60; /* reserved bits, conventionally 1 */
    if (command_bit) ssid_byte |= 0x80;
    if (is_last) ssid_byte |= 0x01;
    out[6] = ssid_byte;
}

void ax25_addr_decode(const uint8_t in[AX25_ADDR_LEN], ax25_addr_t *addr,
                       int *is_last, int *command_bit) {
    for (int i = 0; i < AX25_CALLSIGN_MAX; i++) {
        addr->callsign[i] = (char)(in[i] >> 1);
    }
    addr->callsign[AX25_CALLSIGN_MAX] = 0;
    for (int i = AX25_CALLSIGN_MAX - 1; i >= 0 && addr->callsign[i] == ' '; i--) {
        addr->callsign[i] = 0;
    }
    addr->ssid = (uint8_t)((in[6] >> 1) & 0x0F);
    if (is_last) *is_last = in[6] & 0x01;
    if (command_bit) *command_bit = (in[6] >> 7) & 0x01;
}

uint16_t ax25_fcs(const uint8_t *data, uint16_t len) {
    uint16_t fcs = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        fcs ^= data[i];
        for (int b = 0; b < 8; b++) {
            if (fcs & 1) fcs = (uint16_t)((fcs >> 1) ^ 0x8408);
            else fcs = (uint16_t)(fcs >> 1);
        }
    }
    return (uint16_t)(fcs ^ 0xFFFF);
}

int ax25_build_ui_frame(const ax25_addr_t *dest, const ax25_addr_t *src,
                         const uint8_t *info, uint16_t info_len,
                         uint8_t out[AX25_MAX_FRAME_LEN]) {
    if (info_len > AX25_MAX_INFO_LEN) return -1;

    uint16_t pos = 0;
    ax25_addr_encode(dest, 0, 1, &out[pos]); pos += AX25_ADDR_LEN;
    ax25_addr_encode(src, 1, 0, &out[pos]);  pos += AX25_ADDR_LEN;
    out[pos++] = AX25_CTRL_UI;
    out[pos++] = AX25_PID_NO_L3;
    if (info_len > 0) memcpy(&out[pos], info, info_len);
    pos = (uint16_t)(pos + info_len);

    uint16_t fcs = ax25_fcs(out, pos);
    out[pos++] = (uint8_t)(fcs & 0xFF);
    out[pos++] = (uint8_t)((fcs >> 8) & 0xFF);

    return (int)pos;
}

int ax25_parse_ui_frame(const uint8_t *frame, uint16_t frame_len,
                         ax25_addr_t *dest, ax25_addr_t *src,
                         uint8_t *info_out, uint16_t *info_len_out, uint16_t max_info_len) {
    const uint16_t min_len = AX25_ADDR_LEN * 2 + 1 + 1 + 2;
    if (frame_len < min_len) return -1;

    ax25_addr_decode(&frame[0], dest, NULL, NULL);
    int src_is_last = 0;
    ax25_addr_decode(&frame[AX25_ADDR_LEN], src, &src_is_last, NULL);
    if (!src_is_last) return -1; /* S2S2 uses no repeaters -- source must terminate the address chain */

    uint16_t pos = AX25_ADDR_LEN * 2;
    if (frame[pos] != AX25_CTRL_UI) return -1;
    pos++;
    if (frame[pos] != AX25_PID_NO_L3) return -1;
    pos++;

    uint16_t info_len = (uint16_t)(frame_len - pos - 2);
    if (info_len > max_info_len) return -1;

    uint16_t computed_fcs = ax25_fcs(frame, (uint16_t)(pos + info_len));
    uint16_t received_fcs = (uint16_t)(frame[pos + info_len] | (frame[pos + info_len + 1] << 8));
    if (computed_fcs != received_fcs) return -1;

    if (info_len > 0) memcpy(info_out, &frame[pos], info_len);
    *info_len_out = info_len;
    return 0;
}
