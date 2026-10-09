#ifndef SEAL_WRAP_H
#define SEAL_WRAP_H

/*
 * Light FEC wrap + integrity tag for short packets.
 *
 * Wire layout (152 bytes):
 *   [counter(4)][len(1)][payload up to 107][zero pad to 112][tag(8)] + 32 check bytes
 * The check bytes come from the assembly core (seal_core.S): up to 16 bad
 * bytes per packet are repaired. The tag + counter half is compiled in only
 * when AUTH_ENABLE=1 (make AUTH=TRUE).
 */

#include <stdint.h>
#include <stdbool.h>

#ifndef AUTH_ENABLE
#define AUTH_ENABLE 1
#endif

#define SEAL_HDR_LEN       5U
#define SEAL_BLOCK_LEN     112U
#define SEAL_TAG_LEN       8U
#define SEAL_MAX_PAYLOAD   (SEAL_BLOCK_LEN - SEAL_HDR_LEN)      /* 107 */
#define SEAL_DATA_LEN      (SEAL_BLOCK_LEN + SEAL_TAG_LEN)      /* 120 */
#define SEAL_CHECK_LEN     32U
#define SEAL_WIRE_LEN      (SEAL_DATA_LEN + SEAL_CHECK_LEN)     /* 152 */

#ifdef __cplusplus
extern "C" {
#endif

/* assembly core (seal_core.S) */
void     seal_encode(const uint8_t data[SEAL_DATA_LEN], uint8_t chk[SEAL_CHECK_LEN + 1]);
uint32_t seal_syndromes(const uint8_t cw[SEAL_WIRE_LEN], uint8_t syn[32]);
int32_t  seal_decode(uint8_t cw[SEAL_WIRE_LEN]);

/* Wraps payload (<= SEAL_MAX_PAYLOAD) into out[SEAL_WIRE_LEN]. Returns 152, or 0. */
uint16_t Seal_Wrap(const uint8_t *payload, uint16_t len, uint8_t *out);

/* Inverse of Seal_Wrap. False on uncorrectable damage, bad tag, or replay. */
bool Seal_Unwrap(const uint8_t *wire, uint16_t wireLen,
                 uint8_t *payloadOut, uint16_t payloadOutMax, uint16_t *payloadLenOut);

/* Boot-time check of the assembly core: encode, damage 6 bytes, decode. */
bool Seal_SelfTest(void);

/* Clear the replay floor so a restarted sender (counter back at 1) is accepted again. */
void Seal_ResetRxFloor(void);

#ifdef __cplusplus
}
#endif

#endif /* SEAL_WRAP_H */
