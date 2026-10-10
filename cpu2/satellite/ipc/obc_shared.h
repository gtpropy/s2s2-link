#ifndef OBC_SHARED_H
#define OBC_SHARED_H

/*
 * CPU1 (M4, NuttX "s2s2obc" app) <-> CPU2 (M0+, this firmware) handshake for the image path:
 *
 *   PC --VCP--> CPU1 stores the image in internal flash (0x08022000..0x08031FFF)
 *   CPU1 reads a chunk from flash --> shared flash ring (ipc/Sring_buffer.h, 0x20008840)
 *   CPU2 takes it from the ring and transmits it over RF, then reports "done" here.
 *
 * Block lives in the last 256 bytes of SRAM2 below CPU2's own RAM (0x20009000).
 * Fields are written by exactly one side.
 */

#include <stdint.h>

#define IPC_OBC_BASE      0x20008F00UL
#define OBC_SHARED_MAGIC  0x4F424331UL      /* "OBC1" */

#define OBC_TXST_OK       0x00U
#define OBC_TXST_TXFAIL   0x03U
#define OBC_TXST_PENDING  0xFFU

struct obc_shared_s
{
  volatile uint32_t magic;          /* CPU2 sets it at boot */
  volatile uint32_t cpu2_beat;      /* CPU2: incremented every main-loop pass (liveness) */
  volatile uint32_t session;        /* CPU1: != 0 while an image transfer is running (CPU2 keeps CW off) */
  volatile uint32_t tx_req_seq;     /* CPU1: chunk index + 1 of the chunk just queued in the ring */
  volatile uint32_t tx_done_seq;    /* CPU2: chunk index + 1 it has finished transmitting */
  volatile uint32_t tx_status;      /* CPU2: OBC_TXST_* of that chunk */
  volatile uint32_t reset_req;      /* CPU1: incremented to ask CPU2 to forget its replay floor */
  volatile uint32_t reset_ack;      /* CPU2: = reset_req once done */
  /* ADDED: raw air-rate test (cmd RATE on the VCP). CPU1 fills rate_n / rate_gap_ms and bumps rate_seq; CPU2 sends
   * rate_n packets back to back with rate_gap_ms between them, writes the results and sets rate_done_seq = rate_seq. */
  volatile uint32_t rate_seq;       /* CPU1: request counter */
  volatile uint32_t rate_done_seq;  /* CPU2: = rate_seq when the test is finished */
  volatile uint32_t rate_n;         /* CPU1: number of packets */
  volatile uint32_t rate_gap_ms;    /* CPU1: pause after each packet (0 = back to back) */
  volatile uint32_t rate_sent;      /* CPU2: packets whose TX-done interrupt arrived */
  volatile uint32_t rate_elapsed_ms;/* CPU2: first TX start -> last TX done, in ms (1 ms tick) */
  /* ADDED: continuous CW "HELLO" test for a spectrum analyzer (VCP command CWTEST). */
  volatile uint32_t cw_test_s;      /* CPU1: how long to keep sending, in seconds (0 = stop) */
  volatile uint32_t cw_test_req;    /* CPU1: bumped on every CWTEST command (start, stop or restart) */
  volatile uint32_t cw_test_run;    /* CPU2: 1 while the test is transmitting */
  volatile uint32_t cw_test_words;  /* CPU2: number of HELLO cycles (CW) or packets (GMSK) sent so far */
  volatile uint32_t cw_test_mode;   /* CPU1: 0 = Morse CW "HELLO", 1 = "HELLO" in standard GMSK AX.25 packets, back to back */
};

#define OBC_SHARED  ((struct obc_shared_s *)IPC_OBC_BASE)

#endif /* OBC_SHARED_H */
