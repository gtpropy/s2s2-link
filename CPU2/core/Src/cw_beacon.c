/*
 * cw_beacon.c -- see cw_beacon.h.
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-31
 */
#include "cw_beacon.h"
#include "radio_app.h"
#include <stdint.h>
#include <ctype.h>
#include <stddef.h>

extern void CPU2_Delay_Ms(uint32_t ms);

typedef struct {
    char        ch;
    const char *code; /* '.' = dit, '-' = dah */
} morse_entry_t;

static const morse_entry_t MORSE_TABLE[] = {
    {'A', ".-"},    {'B', "-..."},  {'C', "-.-."},  {'D', "-.."},
    {'E', "."},     {'F', "..-."},  {'G', "--."},   {'H', "...."},
    {'I', ".."},    {'J', ".---"},  {'K', "-.-"},   {'L', ".-.."},
    {'M', "--"},    {'N', "-."},    {'O', "---"},   {'P', ".--."},
    {'Q', "--.-"},  {'R', ".-."},   {'S', "..."},   {'T', "-"},
    {'U', "..-"},   {'V', "...-"},  {'W', ".--"},   {'X', "-..-"},
    {'Y', "-.--"},  {'Z', "--.."},
    {'0', "-----"}, {'1', ".----"}, {'2', "..---"}, {'3', "...--"},
    {'4', "....-"}, {'5', "....."}, {'6', "-...."}, {'7', "--..."},
    {'8', "---.."}, {'9', "----."},
};
#define MORSE_TABLE_LEN (sizeof(MORSE_TABLE) / sizeof(MORSE_TABLE[0]))

static const char *Morse_Lookup(char c) {
    for (size_t i = 0; i < MORSE_TABLE_LEN; i++) {
        if (MORSE_TABLE[i].ch == c) return MORSE_TABLE[i].code;
    }
    return NULL;
}

void cw_beacon1_pack(const cw_beacon1_t *b, uint8_t out[CW_BEACON_PACKED_LEN]) {
    out[0] = (uint8_t)(((b->system_stat & 0x03) << 6) |
                        ((b->mission_status & 0x03) << 4) |
                        (b->time_after_reset & 0x0F));
    out[1] = b->sol_total_i;
    out[2] = (uint8_t)(b->sol_total_v >> 8);
    out[3] = (uint8_t)b->sol_total_v;
    out[4] = (uint8_t)(b->bat_v >> 8);
    out[5] = (uint8_t)b->bat_v;
    out[6] = (uint8_t)(b->bat_i >> 8);
    out[7] = (uint8_t)b->bat_i;
}

void cw_beacon2_pack(const cw_beacon2_t *b, uint8_t out[CW_BEACON_PACKED_LEN]) {
    out[0] = b->gyro_x;
    out[1] = b->gyro_y;
    out[2] = b->gyro_z;
    out[3] = b->mag_x;
    out[4] = b->mag_y;
    out[5] = b->mag_z;
    out[6] = (uint8_t)(((b->sol_plus_x  & 1) << 4) |
                        ((b->sol_minus_y & 1) << 3) |
                        ((b->sol_minus_z & 1) << 2) |
                        ((b->sol_plus_y  & 1) << 1) |
                        (b->sol_plus_z  & 1));
    out[7] = b->bat_temp;
}

void cw_beacon_hex_encode(const uint8_t *bytes, int len, char *out) {
    static const char HEX[] = "0123456789ABCDEF";
    for (int i = 0; i < len; i++) {
        out[i * 2]     = HEX[(bytes[i] >> 4) & 0x0F];
        out[i * 2 + 1] = HEX[bytes[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

void cw_beacon_send(const char *message, int wpm) {
    if (wpm <= 0) wpm = CW_DEFAULT_WPM;
    uint32_t unit_ms = 1200u / (uint32_t)wpm;

    for (const char *p = message; *p != '\0'; p++) {
        char c = (char)toupper((unsigned char)*p);

        if (c == ' ') {
            CPU2_Delay_Ms(unit_ms * 7); /* inter-word gap */
            continue;
        }

        const char *code = Morse_Lookup(c);
        if (code == NULL) continue; /* unsupported character, silently skipped */

        for (const char *s = code; *s != '\0'; s++) {
            RadioApp_KeyCw(true);
            CPU2_Delay_Ms((*s == '-') ? (unit_ms * 3) : unit_ms);
            RadioApp_KeyCw(false);
            CPU2_Delay_Ms(unit_ms); /* intra-character (mark) gap */
        }
        /* Already spent 1 unit as the mark gap above; 2 more brings the
           gap between characters up to the standard 3 units. */
        CPU2_Delay_Ms(unit_ms * 2);
    }
}
