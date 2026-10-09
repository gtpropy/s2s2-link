#ifndef __SHARE_IPC_H
#define __SHARE_IPC_H

#include <stdint.h>

/*
 * ============================================================
 * STM32WL55 SRAM2 shared memory
 * ============================================================
 */

#define IPC_SHARED_ADDR   0x20008200UL

/*
 * Maximum payload in one packet
 */

#define IPC_DATA_SIZE     80

/*
 * IPC magic
 */

#define IPC_MAGIC         0x49504331UL   /* "IPC1" */

/*
 * Ring buffer
 *
 * For this test we use 4 slots.
 */

#define IPC_RING_SIZE     4

/*
 * ============================================================
 * IPC states
 * ============================================================
 */

#define IPC_STATE_IDLE          0

#define IPC_STATE_M4_HELLO      1
#define IPC_STATE_M0_ACK        2

#define IPC_STATE_M4_BURST      3
#define IPC_STATE_M0_BURST_ACK  4

#define IPC_STATE_DONE          5

/*
 * ============================================================
 * One ring-buffer packet
 * ============================================================
 */

struct ipc_ring_packet
{
  volatile uint32_t sequence;

  volatile uint32_t length;

  volatile uint8_t data[IPC_DATA_SIZE];
};

/*
 * ============================================================
 * Shared structure
 * ============================================================
 */

struct ipc_shared_data
{
  volatile uint32_t magic;

  volatile uint32_t state;

  volatile uint32_t sequence;

  volatile uint32_t length;

  volatile uint32_t burst_count;

  /*
   * Ring buffer pointers
   */

  volatile uint32_t ring_write;

  volatile uint32_t ring_read;

  /*
   * Ring buffer
   */

  struct ipc_ring_packet ring[IPC_RING_SIZE];
};

/*
 * ============================================================
 * Shared RAM pointer
 * ============================================================
 */

#define IPC_SHARED \
  ((volatile struct ipc_shared_data *)IPC_SHARED_ADDR)

#endif