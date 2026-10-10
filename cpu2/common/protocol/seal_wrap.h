#ifndef SEAL_WRAP_H
#define SEAL_WRAP_H

/*
 * Packet integrity check (AUTH).
 *
 * With AUTH_ENABLE=1 (make AUTH=TRUE) every short packet (<= SEAL_MAX_PAYLOAD bytes: commands, ACK / NACK, HELLO, ...)
 * gets a 12-byte trailer behind the plain Packet-Format information field:
 *
 *     [ payload ][ counter (4, big endian) ][ tag (8) ]
 *
 * The tag is a keyed 64-bit digest over payload + counter (seal_tag.S); the counter is a replay counter, RAM only: after a
 * reboot the sender restarts at 1 and the receiver rejects until its floor is cleared (Seal_ResetRxFloor).
 * Longer packets (camera fragments, 128-byte telemetry) are not touched. Both ends must use the same AUTH setting.
 * With AUTH_ENABLE=0 nothing is added and the stubs below let the rest of the code compile unchanged.
 */

#include <stdint.h>
#include <stdbool.h>

#ifndef AUTH_ENABLE
#define AUTH_ENABLE 1
#endif

#define SEAL_MAX_PAYLOAD   107U     /* longest payload that is wrapped */
#define SEAL_TAG_LEN       8U
#define SEAL_AUTH_TRAILER  12U      /* counter(4) + tag(8) */

#ifdef __cplusplus
extern "C" {
#endif

#if AUTH_ENABLE
/* true when an information field of n bytes can carry the trailer (cheap test before Protocol_DecodePayload) */
#define SEAL_LEN_MAY_BE_WRAPPED(n)  ((n) > SEAL_AUTH_TRAILER && (n) <= SEAL_MAX_PAYLOAD + SEAL_AUTH_TRAILER)

/* assembly (seal_tag.S); key and data must be 4-byte aligned */
void seal_tag(const uint8_t key[16], const uint8_t *data, uint32_t len, uint8_t out[8]);

/* Appends the trailer: out gets payload + 12 bytes. Returns len + 12, or 0. */
uint16_t Seal_Wrap(const uint8_t *payload, uint16_t len, uint8_t *out);

/* Inverse of Seal_Wrap. False on a bad tag or a replayed / old counter. */
bool Seal_Unwrap(const uint8_t *wire, uint16_t wireLen,
                 uint8_t *payloadOut, uint16_t payloadOutMax, uint16_t *payloadLenOut);

/* Boot-time check of the tag function (official test vector) and a wrap / tamper / replay round trip. */
bool Seal_SelfTest(void);

/* Clear the replay floor so a restarted sender (counter back at 1) is accepted again. */
void Seal_ResetRxFloor(void);

#else  /* AUTH=F: nothing is added, nothing is checked */
#define SEAL_LEN_MAY_BE_WRAPPED(n)  (0)
static inline uint16_t Seal_Wrap(const uint8_t *payload, uint16_t len, uint8_t *out) { (void)payload; (void)len; (void)out; return 0; }
static inline bool Seal_Unwrap(const uint8_t *wire, uint16_t wireLen, uint8_t *payloadOut, uint16_t payloadOutMax, uint16_t *payloadLenOut)
{ (void)wire; (void)wireLen; (void)payloadOut; (void)payloadOutMax; (void)payloadLenOut; return false; }
static inline bool Seal_SelfTest(void) { return true; }
static inline void Seal_ResetRxFloor(void) { }
#endif /* AUTH_ENABLE */

#ifdef __cplusplus
}
#endif

#endif /* SEAL_WRAP_H */
