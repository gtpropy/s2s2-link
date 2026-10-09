/* rxfr: reads hex lines (raw radio buffers) from stdin, runs Protocol_ExtractFrame on each.
 * Prints "0" or "1 <len> <hex>" per line. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "protocol.h"

int main(void)
{
    static char line[8192];
    while (fgets(line, sizeof line, stdin)) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = 0;
        uint8_t buf[2048]; size_t len = n / 2;
        for (size_t i = 0; i < len; i++) { unsigned v; sscanf(line + 2*i, "%2x", &v); buf[i] = (uint8_t)v; }
        uint8_t out[AX25_MAX_FRAME_SIZE]; uint16_t olen = 0;
        if (Protocol_ExtractFrame(buf, (uint16_t)len, out, sizeof out, &olen)) {
            printf("1 %u ", olen);
            for (int i = 0; i < olen; i++) printf("%02x", out[i]);
            putchar('\n');
        } else puts("0");
    }
    return 0;
}
