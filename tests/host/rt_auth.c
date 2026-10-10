/* AUTH without FEC (-DAUTH_ENABLE=1): payload + counter(4) + tag(8) trailer.
 * create -> extract -> verify -> unwrap, then tamper, replay, and the sheet-compliant prefix. */
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
    cfg.destCallsign = "GROUND"; cfg.sourceCallsign = "9N2SJ"; cfg.sourceSSID = 1;
    for (int len = 1; len <= 107; len += 7) {
        uint8_t pl[107], out[RADIO_FIXED_PACKET_LEN], fr[AX25_MAX_FRAME_SIZE], back[200]; uint16_t fl = 0, bl = 0;
        for (int i = 0; i < len; i++) pl[i] = (uint8_t)(rand());
        CHK(Protocol_CreatePacket(out, pl, len, &cfg) > 0);
        CHK(Protocol_ExtractFrame(out, RADIO_FIXED_PACKET_LEN, fr, sizeof fr, &fl));
        CHK(Protocol_VerifyPacket(fr, fl));
        CHK(fl - 20 == (uint16_t)(len + 12));                             /* info field = payload + 12 */
        CHK(memcmp(fr + 17, pl, len) == 0);                               /* sheet layout stays in front */
        CHK(Protocol_DecodePayload(fr + 17, fl - 20, back, sizeof back, &bl));
        CHK(bl == len && memcmp(back, pl, len) == 0);
        CHK(!Protocol_DecodePayload(fr + 17, fl - 20, back, sizeof back, &bl));   /* replay */
        /* every single flipped bit of the info field must be rejected */
        CHK(Protocol_CreatePacket(out, pl, len, &cfg) > 0);
        CHK(Protocol_ExtractFrame(out, RADIO_FIXED_PACKET_LEN, fr, sizeof fr, &fl));
        for (int b = 0; b < (len + 12) * 8; b++) {
            uint8_t w[119]; memcpy(w, fr + 17, len + 12); w[b >> 3] ^= (uint8_t)(1U << (b & 7));
            CHK(!Protocol_DecodePayload(w, len + 12, back, sizeof back, &bl));
        }
        CHK(Protocol_DecodePayload(fr + 17, fl - 20, back, sizeof back, &bl));    /* the untouched one still passes */
    }
    /* plain (unauthenticated) short packet must not unwrap */
    { uint8_t plain[20] = { 0x53, 0x04, 0xCC, 0x5E, 0xBD }, back[64]; uint16_t bl = 0;
      CHK(!Protocol_DecodePayload(plain, sizeof plain, back, sizeof back, &bl)); }
    CHK(Seal_SelfTest());
    printf(fails ? "FAILED %d\n" : "rt_auth OK\n", fails);
    return fails != 0;
}
