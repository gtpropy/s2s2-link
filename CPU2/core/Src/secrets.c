/*
 * secrets.c -- defines s2s2_auth_key from secrets.h. See secrets.example.h.
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-30
 */
#include "secrets.h"
#include "s2s2_auth.h"

const uint8_t s2s2_auth_key[S2S2_AUTH_KEY_LEN] = { S2S2_AUTH_KEY_BYTES };
