/* txpkt <payload_hex> [count]: builds N radio packets through the real Protocol_CreatePacket
 * and prints each as hex ("0" if the frame did not fit the fixed packet). Payload is
 * regenerated per packet from the seed when count > 1 (payload_hex = "rand:<len>"). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    int count = argc > 2 ? atoi(argv[2]) : 1;
    RadioConfig_t cfg; memset(&cfg, 0, sizeof cfg);
    cfg.destCallsign = "GROUND"; cfg.destSSID = 0;
    cfg.sourceCallsign = "9NS2S2"; cfg.sourceSSID = 1;
    srand(12345);
    for (int n = 0; n < count; n++) {
        uint8_t payload[260]; int len;
        if (strncmp(argv[1], "rand:", 5) == 0) {
            len = atoi(argv[1] + 5);
            for (int i = 0; i < len; i++) payload[i] = (uint8_t)rand();
        } else {
            len = (int)(strlen(argv[1]) / 2);
            for (int i = 0; i < len; i++) { unsigned v; sscanf(argv[1] + 2 * i, "%2x", &v); payload[i] = (uint8_t)v; }
        }
        uint8_t out[RADIO_FIXED_PACKET_LEN];
        uint16_t r = Protocol_CreatePacket(out, payload, (uint16_t)len, &cfg);
        if (r == 0) { puts("0"); continue; }
        for (int i = 0; i < r; i++) printf("%02x", out[i]);
        putchar('\n');
    }
    return 0;
}
