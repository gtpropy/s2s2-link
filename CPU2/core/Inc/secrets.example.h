/*
 * secrets.example.h -- template for secrets.h (gitignored, never commit
 * the real one). Copy this to secrets.h and replace the key below with a
 * freshly generated random 128-bit value, e.g.:
 *
 *   python3 -c "import secrets; print(', '.join('0x%02X' % b for b in secrets.token_bytes(16)))"
 *
 * The same key must be compiled into BOTH the satellite (`make sat`) and
 * ground-station (`make gs`) images -- it's a shared secret, not a
 * per-node one. See s2s2_auth.h for what it protects.
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-30
 */
#ifndef SECRETS_H
#define SECRETS_H

#define S2S2_AUTH_KEY_BYTES \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00

#endif /* SECRETS_H */
