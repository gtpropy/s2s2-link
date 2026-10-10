#ifndef SAF_H
#define SAF_H

/*
 * Store-and-forward memory of the digipeater -- pure logic, no flash, no radio.
 *
 * A frame the digipeater accepted is KEPT here (already rewritten for our hop, so its path carries the remaining hop budget
 * of a WIDEn-N path) and transmitted again later, on a later pass or to a station that was out of range the first time.
 * Hopping: every message carries a hop budget N (the unused WIDEn-N of its path). The hop is consumed when the message is
 * accepted (path rewrite in Digi_Process), so the next store-and-forward node can continue the chain; our own copy is
 * re-sent at most `max_forwards` times, `forward_interval` apart, and dies after `ttl`. A node never takes a message back
 * that already carries its own call sign (loop check of the engine), which bounds the total number of hops in the network.
 *
 * Scheduling: messages that still have hops left go first (they can travel further), then the ones forwarded least often,
 * then the oldest.
 *
 * Persistence is the caller's job: the table reports what changed (dirty slots, deleted ids) so a journal can follow it.
 */

#include <stdint.h>
#include <stdbool.h>
#include "digipeater.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SAF_SLOTS 12U

typedef struct {
    bool     used;
    bool     dirty;                  /* changed since it was last written to the journal */
    uint16_t id;                     /* unique while stored; the journal key */
    uint8_t  len;
    uint8_t  hops_left;              /* hop budget left in the path (information; used for the forward priority) */
    uint8_t  fwd_count;              /* times this node has sent it again */
    uint32_t t_store_ms;
    uint32_t expire_ms;              /* absolute, in the caller's clock */
    uint32_t next_fwd_ms;            /* absolute: not before this */
    uint8_t  body[DIGI_MAX_BODY];
} SafSlot_t;

typedef struct {
    uint8_t  max_forwards;           /* sends of one stored message, >= 1 */
    uint32_t forward_interval_ms;    /* minimum time between two sends of the same message */
    uint32_t ttl_ms;                 /* a message is dropped this long after it was stored */
} SafConfig_t;

typedef struct {
    uint32_t stored, forwarded, expired, evicted, duplicates, restored;
} SafStats_t;

typedef struct {
    SafConfig_t cfg;
    SafSlot_t   slot[SAF_SLOTS];
    uint16_t    nextId;
    uint16_t    gone[SAF_SLOTS * 2U];   /* ids removed since the journal last heard of them */
    uint8_t     nGone;
    SafStats_t  stats;
} Saf_t;

void Saf_Init(Saf_t *s, const SafConfig_t *cfg);

/* keep a frame body. first_delay_ms = time until its first send. Returns the slot, -1 if the same frame is already stored
 * (its TTL is not extended), -2 if it does not fit. When the table is full the entry that has been sent most often (then the
 * oldest) is evicted. */
int Saf_Store(Saf_t *s, const uint8_t *body, uint16_t len, uint8_t hops_left, uint32_t now_ms, uint32_t first_delay_ms);

/* the slot to send next, -1 when nothing is due */
int Saf_NextDue(Saf_t *s, uint32_t now_ms);

/* the caller has sent slot i: counts it, schedules the next send, removes the message after the last one */
void Saf_Forwarded(Saf_t *s, int i, uint32_t now_ms);

/* drop everything whose time is up */
void Saf_Expire(Saf_t *s, uint32_t now_ms);

/* put a message back after a restart (from the journal): remaining TTL in ms, its id and counters are kept */
bool Saf_Restore(Saf_t *s, uint16_t id, const uint8_t *body, uint16_t len, uint8_t hops_left, uint8_t fwd_count,
                 uint32_t ttl_left_ms, uint32_t now_ms);

/* journal follow-up */
int  Saf_NextDirty(const Saf_t *s);                 /* slot index or -1 */
void Saf_ClearDirty(Saf_t *s, int i);
bool Saf_PopGone(Saf_t *s, uint16_t *id);
uint32_t Saf_TtlLeftMs(const Saf_t *s, int i, uint32_t now_ms);
uint8_t  Saf_Count(const Saf_t *s);

#ifdef __cplusplus
}
#endif

#endif /* SAF_H */
