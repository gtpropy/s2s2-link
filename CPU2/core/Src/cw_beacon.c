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
