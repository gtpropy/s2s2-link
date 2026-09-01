/*
 * tx_history.h -- small ring buffer of recently-sent S2S2 messages, keyed
 * by seq_num, so a satellite can actually honor CMD_REQUEST_RETRANSMIT and
 * an incoming TYPE_GAP_REPORT instead of always replying
 * ACK_STATUS_DATA_UNAVAILABLE. Pure C, no HAL dependency, so it's usable
 * from ipcc_m0plus_main.c and host-testable the same way as
 * core/Src/test_roundtrip.c.
 *
 * TX_HISTORY_MAX_PAYLOAD is deliberately much smaller than MAX_PAYLOAD_LEN
 * (659): the M0+ core's whole SRAM budget is 32K (../../scripts/wl55jc_cm0p.ld),
 * and storing TX_HISTORY_LEN entries at the full protocol ceiling would eat
 * a large fraction of that. Real telemetry payloads sent by this firmware
 * today are well under 128 bytes; a message larger than
 * TX_HISTORY_MAX_PAYLOAD is simply not buffered for retransmit (the
 * existing ACK_STATUS_DATA_UNAVAILABLE reply already covers "not
 * available", so this degrades gracefully rather than corrupting anything).
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-08-30
*/
#ifndef TX_HISTORY_H
#define TX_HISTORY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TX_HISTORY_LEN          16
#define TX_HISTORY_MAX_PAYLOAD  128

typedef struct {
    bool     used;
    uint16_t seq_num;
    uint8_t  type;
    uint8_t  dst_id;
    uint8_t  flags;
    uint16_t payload_len;
    uint8_t  payload[TX_HISTORY_MAX_PAYLOAD];
} tx_history_entry_t;

typedef struct {
    tx_history_entry_t entries[TX_HISTORY_LEN];
    uint32_t next;
} tx_history_t;

void tx_history_init(tx_history_t *h);

/* Stores one sent message keyed by seq_num. Silently a no-op if
   payload_len > TX_HISTORY_MAX_PAYLOAD (see header comment above) --
   that message just won't be retransmittable later. */
void tx_history_store(tx_history_t *h, uint16_t seq_num, uint8_t type, uint8_t dst_id,
                       uint8_t flags, const uint8_t *payload, uint16_t payload_len);

/* Returns the entry for seq_num, or NULL if it was never stored, was too
   large to store, or has since been overwritten by newer traffic. */
const tx_history_entry_t *tx_history_find(const tx_history_t *h, uint16_t seq_num);

#ifdef __cplusplus
}
#endif

#endif /* TX_HISTORY_H */
