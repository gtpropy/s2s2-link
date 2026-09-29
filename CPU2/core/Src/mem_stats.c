/*
 * mem_stats.c -- see mem_stats.h.
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-09-28
 */
#include "mem_stats.h"
#include "stm32wlxx_hal.h"  /* CMSIS __get_MSP() */

/* Linker-provided symbols (wl55jc_cm0p.ld): `end` is PROVIDE()'d right
   after .bss (start of the heap+stack region), `_estack` is the top of
   RAM (initial MSP). Neither is a real variable -- only its ADDRESS is
   meaningful, hence the "take the address of an opaque extern" idiom
   instead of dereferencing them. */
extern uint32_t end;
extern uint32_t _estack;

/* Chosen to be unlikely to occur naturally in real stack data (not a
   round/common constant like 0 or 0xFFFFFFFF, not a valid RAM address on
   this chip, not a plausible float/int application value). Inherent
   false-negative risk of the stack-painting technique either way -- see
   mem_stats.h. */
#define STACK_PAINT_PATTERN 0xC5C5A3A3UL

/* Never paint over the caller's own current frame -- leaves a safety
   margin below the live SP at the moment this runs (only matters
   because MemStats_PaintStack() itself, plus whatever startup code ran
   before it in main(), already has some frame depth by the time it
   executes; a generous margin costs nothing, this only runs once). */
#define PAINT_MARGIN_BYTES 128U

void MemStats_PaintStack(void)
{
    uint32_t *start = &end;
    uint32_t sp = __get_MSP();
    uint32_t *limit = (uint32_t *)(sp - PAINT_MARGIN_BYTES);

    for (uint32_t *p = start; p < limit; p++) {
        *p = STACK_PAINT_PATTERN;
    }
}

uint32_t MemStats_GetStackUsedBytes(void)
{
    uint32_t *p = &end;
    uint32_t *top = &_estack;

    while (p < top && *p == STACK_PAINT_PATTERN) {
        p++;
    }
    return (uint32_t)((uint8_t *)top - (uint8_t *)p);
}

uint32_t MemStats_GetStackTotalBytes(void)
{
    return (uint32_t)((uint8_t *)&_estack - (uint8_t *)&end);
}
