#include <stdint.h>
#include "uart/uart_debug.h"

void HardFault_C(uint32_t *sp)
{
    uart2_printf("\r\n!!! HARDFAULT  PC=0x%08lX LR=0x%08lX R0=0x%08lX xPSR=0x%08lX\r\n",
                 (unsigned long)sp[6], (unsigned long)sp[5],
                 (unsigned long)sp[0], (unsigned long)sp[7]);
    for (;;) { }
}

__attribute__((naked)) void HardFault_Handler(void)
{
    __asm volatile(
        ".syntax unified        \n"
        "mov   r1, lr           \n"
        "movs  r2, #4           \n"
        "tst   r1, r2           \n"
        "mrs   r0, msp          \n"
        "beq   1f               \n"
        "mrs   r0, psp          \n"
        "1:                     \n"
        "ldr   r1, =HardFault_C \n"
        "bx    r1               \n"
    );
}