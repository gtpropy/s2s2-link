#include <stdint.h>

#include "uart_debug.h"
#include "share_ipc.h"

/*
 * ============================================================
 * Delay
 * ============================================================
 */

static void delay_loop(volatile uint32_t count)
{
  while (count--)
    {
      __asm__ volatile ("nop");
    }
}

/*
 * ============================================================
 * M0+ memory barrier
 * ============================================================
 */

static void m0_memory_barrier(void)
{
  __asm__ volatile ("dmb sy" ::: "memory");
}

/*
 * ============================================================
 * Print text packet
 * ============================================================
 */

static void print_text_packet(uint32_t slot)
{
  uint32_t i;
  uint32_t length;

  length = IPC_SHARED->ring[slot].length;

  uart2_printf("M0+: slot     = %lu\r\n",
               (unsigned long)slot);

  uart2_printf("M0+: sequence = %lu\r\n",
               (unsigned long)
               IPC_SHARED->ring[slot].sequence);

  uart2_printf("M0+: length   = %lu\r\n",
               (unsigned long)length);

  uart2_printf("M0+: DATA:\r\n");

  /*
   * Print packet as characters.
   */

  for (i = 0; i < length; i++)
    {
      uart2_printf("%c",
                   IPC_SHARED->ring[slot].data[i]);
    }

  uart2_printf("\r\n");
}

/*
 * ============================================================
 * MAIN
 * ============================================================
 */

int main(void)
{
  uint32_t slot;

  uart2_init();

  uart2_printf("\r\n");
  uart2_printf("========================================\r\n");
  uart2_printf(" STM32WL55 M0+ IPC RING BUFFER TEST\r\n");
  uart2_printf("========================================\r\n");

  uart2_printf("M0+: START\r\n");

  uart2_printf("M0+: shared address = 0x%08lX\r\n",
               (unsigned long)IPC_SHARED_ADDR);

  uart2_printf("M0+: data size = %lu\r\n",
               (unsigned long)IPC_DATA_SIZE);

  uart2_printf("M0+: ring size = %lu\r\n",
               (unsigned long)IPC_RING_SIZE);

  /*
   * ==========================================================
   * WAIT FOR M4
   * ==========================================================
   */

  uart2_printf("M0+: WAITING FOR M4...\r\n");

  while (IPC_SHARED->magic != IPC_MAGIC)
    {
      delay_loop(10000);
    }

  m0_memory_barrier();

  uart2_printf("M0+: MAGIC OK\r\n");

  /*
   * ==========================================================
   * MAIN LOOP
   * ==========================================================
   */

  while (1)
    {
      /*
       * ======================================================
       * M4 -> M0 HELLO
       * ======================================================
       */

      if (IPC_SHARED->state == IPC_STATE_M4_HELLO)
        {
          uint32_t i;
          uint32_t length;

          m0_memory_barrier();

          uart2_printf("\r\n");
          uart2_printf("----------------------------------------\r\n");
          uart2_printf(" M4 -> M0+ : HELLO\r\n");
          uart2_printf("----------------------------------------\r\n");

          length = IPC_SHARED->ring[0].length;

          uart2_printf("M0+: length   = %lu\r\n",
                       (unsigned long)length);

          uart2_printf("M0+: sequence = %lu\r\n",
                       (unsigned long)
                       IPC_SHARED->ring[0].sequence);

          uart2_printf("M0+: data     = ");

          for (i = 0; i < length; i++)
            {
              uart2_printf("%c",
                           IPC_SHARED->ring[0].data[i]);
            }

          uart2_printf("\r\n");

          /*
           * ACK
           */

          m0_memory_barrier();

          IPC_SHARED->state = IPC_STATE_M0_ACK;

          m0_memory_barrier();

          uart2_printf("M0+ -> M4 : ACK SENT\r\n");
        }

      /*
       * ======================================================
       * M4 -> M0 TEXT BURST
       * ======================================================
       */

      if (IPC_SHARED->state == IPC_STATE_M4_BURST)
        {
          m0_memory_barrier();

          /*
           * Read current ring slot.
           */

          slot = IPC_SHARED->ring_read;

          uart2_printf("\r\n");
          uart2_printf("========================================\r\n");
          uart2_printf(" M4 -> M0+ : TEXT BURST RECEIVED\r\n");
          uart2_printf("========================================\r\n");

          /*
           * Print packet.
           */

          print_text_packet(slot);

          /*
           * Move read pointer.
           */

          IPC_SHARED->ring_read =
              (slot + 1) % IPC_RING_SIZE;

          m0_memory_barrier();

          uart2_printf("M0+: ring_read = %lu\r\n",
                       (unsigned long)
                       IPC_SHARED->ring_read);

          /*
           * Send ACK.
           */

          IPC_SHARED->state = IPC_STATE_M0_BURST_ACK;

          m0_memory_barrier();

          uart2_printf("M0+ -> M4 : TEXT PACKET ACK SENT\r\n");
        }

      /*
       * ======================================================
       * DONE
       * ======================================================
       */

      if (IPC_SHARED->state == IPC_STATE_DONE)
        {
          uart2_printf("\r\n");
          uart2_printf("========================================\r\n");
          uart2_printf(" M0+ : COMPLETE TEXT RECEIVED\r\n");
          uart2_printf("========================================\r\n");

          uart2_printf("M0+: total bursts = %lu\r\n",
                       (unsigned long)
                       IPC_SHARED->burst_count);

          while (1)
            {
              delay_loop(1000000);
            }
        }

      delay_loop(10000);
    }

  return 0;
}