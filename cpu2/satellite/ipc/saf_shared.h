#ifndef SAF_SHARED_H
#define SAF_SHARED_H

/*
 * Store-and-forward mailbox between CPU2 (M0+, digipeater) and CPU1 (M4, NuttX app s2s2obc) in SRAM2.
 *
 * CPU2 keeps the messages in RAM and sends them (cpu2/common/digipeater/saf.c). CPU1 keeps them in a journal file on its
 * LittleFS (/obc/saf.jrn, internal flash) so they survive a reset, and plays them back to CPU2 after a restart.
 * CPU1 can also hand CPU2 a frame as if it had been received over the air (test injection from the PC, VCP command 0x0D).
 *
 * Every field has exactly one writer. Handshakes are sequence counters: the producer increments *_seq after filling the
 * record, the consumer copies it and sets *_ack = *_seq. Both sides re-initialise their own fields at start-up, so either
 * core may boot first or restart alone.
 *
 *   CPU2 -> CPU1   put      op STORE (insert / update a record) or DELETE (id)        put_seq  / put_ack
 *   CPU1 -> CPU2   rst      the journal's live records, one by one, then op END       rst_seq  / rst_ack
 *   CPU2 -> CPU1   restore_req: "send me the records again"                          restore_req / restore_ack
 *   CPU1 -> CPU2   inject   one received-frame body for the digipeater                inject_seq / inject_ack, result
 *
 * Region 0x20008000..0x200082FF is free in SRAM2 (the TX ring starts at 0x20008300).
 */

#include <stdint.h>

#define IPC_SAF_BASE       0x20008000UL
#define SAF_MBOX_MAGIC     0x53414631UL      /* "SAF1" */
#define SAF_CPU1_READY     0x4A524E4CUL      /* "JRNL": CPU1's journal is open */

#define SAF_OP_STORE       1U
#define SAF_OP_DELETE      2U
#define SAF_OP_END         3U                /* restore stream finished; len = number of records sent */
#define SAF_BODY_MAX       160U              /* = DIGI_MAX_BODY */

struct saf_wire_s
{
  uint16_t id;
  uint8_t  op;
  uint8_t  len;                              /* bytes of body */
  uint8_t  hops_left;
  uint8_t  fwd_count;
  uint16_t reserved;
  uint32_t ttl_left_s;                       /* remaining life; the time while the satellite was off is not counted (no RTC) */
  uint8_t  body[SAF_BODY_MAX];
};

struct saf_mbox_s
{
  volatile uint32_t cpu2_magic;              /* CPU2 */
  volatile uint32_t cpu1_ready;              /* CPU1 */
  volatile uint32_t put_seq;                 /* CPU2 */
  volatile uint32_t put_ack;                 /* CPU1 */
  volatile uint32_t restore_req;             /* CPU2 */
  volatile uint32_t restore_ack;             /* CPU1 */
  volatile uint32_t rst_seq;                 /* CPU1 */
  volatile uint32_t rst_ack;                 /* CPU2 */
  volatile uint32_t inject_seq;              /* CPU1 */
  volatile uint32_t inject_ack;              /* CPU2 */
  volatile uint32_t inject_result;           /* CPU2: DigiResult_t of the injected frame, 0x100 + that when it was transmitted */
  volatile uint32_t live;                    /* CPU1: records in the journal, for the status line */
  struct saf_wire_s put;                     /* CPU2 -> CPU1 */
  struct saf_wire_s rst;                     /* CPU1 -> CPU2 */
  struct saf_wire_s inject;                  /* CPU1 -> CPU2: only len and body are used */
};

#define SAF_MBOX  ((struct saf_mbox_s *)IPC_SAF_BASE)
#define SAF_BARRIER() __asm__ volatile ("dmb" ::: "memory")

#endif /* SAF_SHARED_H */
