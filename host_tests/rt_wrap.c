/* host round trip through the real wrap: create -> extract -> verify -> decode, + replay + corruption */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "protocol.h"
#include "seal_wrap.h"
static int fails;
#define CHK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)
int main(void)
{
    RadioConfig_t cfg; memset(&cfg, 0, sizeof cfg);
    cfg.destCallsign = "GROUND"; cfg.sourceCallsign = "9NS2S2"; cfg.sourceSSID = 1;
    for (int len = 1; len <= 107; len += 13) {
        uint8_t pl[107], out[RADIO_FIXED_PACKET_LEN], fr[AX25_MAX_FRAME_SIZE], back[200]; uint16_t fl = 0, bl = 0;
        for (int i = 0; i < len; i++) pl[i] = (uint8_t)(rand());
        CHK(Protocol_CreatePacket(out, pl, len, &cfg) > 0);
        CHK(Protocol_ExtractFrame(out, RADIO_FIXED_PACKET_LEN, fr, sizeof fr, &fl));
        CHK(Protocol_VerifyPacket(fr, fl));
        CHK(Protocol_DecodePayload(fr + 17, fl - 20, back, sizeof back, &bl));
        CHK(bl == len && memcmp(back, pl, len) == 0);
        CHK(!Protocol_DecodePayload(fr + 17, fl - 20, back, sizeof back, &bl));   /* replay */
        /* payload tampered with valid parity -> tag must reject */
        CHK(Protocol_CreatePacket(out, pl, len, &cfg) > 0);
        CHK(Protocol_ExtractFrame(out, RADIO_FIXED_PACKET_LEN, fr, sizeof fr, &fl));
        { uint8_t w[152]; uint8_t chk[33]; memcpy(w, fr + 17, 152); w[6] ^= 0x40; seal_encode(w, chk); memcpy(w + 120, chk, 32);
          CHK(!Protocol_DecodePayload(w, 152, back, sizeof back, &bl)); }
        /* damaged parity -> reject (host decode is detect-only) */
        CHK(Protocol_CreatePacket(out, pl, len, &cfg) > 0);
        CHK(Protocol_ExtractFrame(out, RADIO_FIXED_PACKET_LEN, fr, sizeof fr, &fl));
        fr[17 + 130] ^= 0x01;
        CHK(!Protocol_DecodePayload(fr + 17, fl - 20, back, sizeof back, &bl));
    }
    printf(fails ? "FAILED %d\n" : "rt_wrap OK\n", fails);
    return fails != 0;
}
