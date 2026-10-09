#ifndef __IPCC_TEST_H
#define __IPCC_TEST_H

#include <stdint.h>
#include <stdbool.h>

/*
 * ============================================================
 * STM32WL55 CPU1 <-> CPU2 IPCC
 * ============================================================
 *
 * CPU1 = Cortex-M4
 * CPU2 = Cortex-M0+
 *
 * IPCC base address:
 *
 *     0x58000000
 *
 * We use two logical channels:
 *
 *     CH0 = hardware IPCC channel 1
 *     CH1 = hardware IPCC channel 2
 *
 * CH0:
 *
 *     M4 -> M0+
 *
 * CH1:
 *
 *     M0+ -> M4
 *
 * ============================================================
 */


/*
 * ============================================================
 * IPCC base
 * ============================================================
 */

#define IPCC_BASE             0x58000000UL


/*
 * ============================================================
 * CPU1 / M4 registers
 * ============================================================
 */

#define IPCC_C1SCR_OFFSET     0x0008UL
#define IPCC_C1TOC2SR_OFFSET  0x000CUL


/*
 * ============================================================
 * CPU2 / M0+ registers
 * ============================================================
 */

#define IPCC_C2SCR_OFFSET     0x0018UL
#define IPCC_C2TOC1SR_OFFSET  0x001CUL


/*
 * ============================================================
 * CPU1 / M4 register access
 * ============================================================
 */

#define IPCC_C1SCR \
  (*(volatile uint32_t *)(IPCC_BASE + IPCC_C1SCR_OFFSET))


#define IPCC_C1TOC2SR \
  (*(volatile uint32_t *)(IPCC_BASE + IPCC_C1TOC2SR_OFFSET))


/*
 * ============================================================
 * CPU2 / M0+ register access
 * ============================================================
 */

#define IPCC_C2SCR \
  (*(volatile uint32_t *)(IPCC_BASE + IPCC_C2SCR_OFFSET))


#define IPCC_C2TOC1SR \
  (*(volatile uint32_t *)(IPCC_BASE + IPCC_C2TOC1SR_OFFSET))


/*
 * ============================================================
 * Logical channels
 * ============================================================
 *
 * Hardware IPCC channels are numbered:
 *
 *     1 ... 6
 *
 * Our software uses:
 *
 *     IPCC_CH0 = hardware channel 1
 *     IPCC_CH1 = hardware channel 2
 *
 * ============================================================
 */

#define IPCC_CH0     1U
#define IPCC_CH1     2U


/*
 * ============================================================
 * Application events
 * ============================================================
 *
 * These values are NOT hardware IPCC channel numbers.
 *
 * They describe what the shared-memory message means.
 * ============================================================
 */

#define IPC_EVENT_DATA_READY  0x01U
#define IPC_EVENT_READY_ACK   0x02U
#define IPC_EVENT_DATA_ACK    0x03U


/*
 * ============================================================
 * IPCC bit helpers
 * ============================================================
 *
 * RX bits:
 *
 *     bit 0 = channel 1
 *     bit 1 = channel 2
 *     ...
 *
 * TX bits:
 *
 *     bit 16 = channel 1
 *     bit 17 = channel 2
 *     ...
 *
 * ============================================================
 */

#define IPCC_RX_BIT(ch) \
  (1UL << ((ch) - 1U))


#define IPCC_TX_BIT(ch) \
  (1UL << (16U + ((ch) - 1U)))


/*
 * ============================================================
 * Memory barrier
 * ============================================================
 *
 * Important when communicating through shared SRAM.
 * ============================================================
 */

static inline void ipcc_memory_barrier(void)
{
  __asm__ volatile ("dmb sy" ::: "memory");
}


/*
 * ============================================================
 * M4 / CPU1
 * ============================================================
 */


/*
 * M4 -> M0+
 *
 * Request/send notification.
 */

static inline void ipcc_m4_send(uint32_t ch)
{
  IPCC_C1SCR = IPCC_TX_BIT(ch);

  ipcc_memory_barrier();
}


/*
 * M4 checks:
 *
 * M0+ -> M4
 */

static inline bool ipcc_m4_received(uint32_t ch)
{
  return (IPCC_C2TOC1SR & IPCC_RX_BIT(ch)) != 0U;
}


/*
 * M4 clears received notification.
 */

static inline void ipcc_m4_clear(uint32_t ch)
{
  IPCC_C1SCR = IPCC_RX_BIT(ch);

  ipcc_memory_barrier();
}


/*
 * ============================================================
 * M0+ / CPU2
 * ============================================================
 */


/*
 * M0+ -> M4
 *
 * Send notification.
 */

static inline void ipcc_m0_send(uint32_t ch)
{
  IPCC_C2SCR = IPCC_TX_BIT(ch);

  ipcc_memory_barrier();
}


/*
 * M0+ checks:
 *
 * M4 -> M0+
 */

static inline bool ipcc_m0_received(uint32_t ch)
{
  return (IPCC_C1TOC2SR & IPCC_RX_BIT(ch)) != 0U;
}


/*
 * M0+ clears received notification.
 */

static inline void ipcc_m0_clear(uint32_t ch)
{
  IPCC_C2SCR = IPCC_RX_BIT(ch);

  ipcc_memory_barrier();
}


#endif