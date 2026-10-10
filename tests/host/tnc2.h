/* TNC2 monitor text <-> frame body helpers for the host tests: thin wrapper over the engine's Digi_FromTnc2. */
#ifndef TNC2_H
#define TNC2_H
#include "digipeater.h"
static inline uint16_t tnc2_to_body(const char *txt, uint8_t *body, uint16_t max, uint8_t control, uint8_t pid)
{
    uint16_t n = Digi_FromTnc2(txt, body, max);
    if (n) { DigiFrame_t f; if (Digi_ParseFrame(body, n, &f)) { body[14 + 7U * f.nvia] = control; body[15 + 7U * f.nvia] = pid; } }
    return n;
}
#endif
