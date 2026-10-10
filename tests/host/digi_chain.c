/* digi_chain: the satellite digipeater path on the host with the REAL framing code, no radio.
 *   for every TNC2 argument:  sender builds the 200-byte radio packet (Protocol_CreateRawPacket)
 *                             -> satellite side: Protocol_ExtractFrame -> FCS check -> Digi_Process
 *                             -> Protocol_CreateRawPacket of the repeat
 * stdout: one hex line per transmitted radio packet ("IN <hex>" for the senders, "OUT <hex>" for the repeats),
 * stderr: the decision per frame. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"
#include "digipeater.h"

int main(int argc, char **argv)
{
    Digi_t d; DigiConfig_t c;
    Digi_DefaultConfig(&c, "9N2SJ", 1);
    strcpy(c.alias[0], "ARISS"); c.nAlias = 1;
    Digi_Init(&d, &c);
    uint32_t now = 0;
    for (int i = 1; i < argc; i++, now += 12000) {
        uint8_t body[DIGI_MAX_BODY], pkt[RADIO_FIXED_PACKET_LEN], fr[AX25_MAX_FRAME_SIZE], out[DIGI_MAX_BODY], pkt2[RADIO_FIXED_PACKET_LEN];
        uint16_t bl = Digi_FromTnc2(argv[i], body, sizeof body), fl = 0, ol = 0;
        if (!bl) { fprintf(stderr, "bad frame text: %s\n", argv[i]); return 2; }
        if (Protocol_CreateRawPacket(pkt, body, bl) != RADIO_FIXED_PACKET_LEN) { fprintf(stderr, "sender packet failed: %s\n", argv[i]); return 2; }
        printf("IN ");  for (int k = 0; k < RADIO_FIXED_PACKET_LEN; k++) printf("%02x", pkt[k]); printf("\n");
        if (!Protocol_ExtractFrame(pkt, RADIO_FIXED_PACKET_LEN, fr, sizeof fr, &fl) || !AX25_VerifyCRC_Method1(fr, fl)) {
            fprintf(stderr, "[%d] satellite could not decode the sender packet\n", i); return 3;
        }
        DigiResult_t r = Digi_Process(&d, fr + 1, (uint16_t)(fl - 4), now, out, sizeof out, &ol);
        if (r != DIGI_REPEAT) { fprintf(stderr, "[%d] %-12s %s\n", i, Digi_ReasonName(r), argv[i]); continue; }
        DigiFrame_t g; char txt[300]; Digi_ParseFrame(out, ol, &g); Digi_FormatFrame(&g, txt, sizeof txt);
        fprintf(stderr, "[%d] REPEAT       %s\n", i, txt);
        if (Protocol_CreateRawPacket(pkt2, out, ol) != RADIO_FIXED_PACKET_LEN) { fprintf(stderr, "repeat packet does not fit\n"); return 4; }
        printf("OUT "); for (int k = 0; k < RADIO_FIXED_PACKET_LEN; k++) printf("%02x", pkt2[k]); printf("\n");
    }
    return 0;
}
