/*
 * mem_stats.h -- real runtime stack high-water-mark via stack painting.
 * Everything else the web tool's "memory profile" reports (flash/RAM
 * .text/.data/.bss) is static, link-time-only information read from the
 * built .elf -- it can't see how deep the stack has actually gone at
 * runtime, since this firmware has no RTOS/task accounting to sample.
 * This is the one number that needs the firmware's own cooperation.
 *
 * @author Ryan Dhakal, SOS Hermann Gmeiner School, Gandaki
 * @date   2026-09-28
 */
#ifndef MEM_STATS_H
#define MEM_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Paints the unused stack region (from the linker's `end` symbol -- the
   first byte after .bss -- up to just below the current stack pointer)
   with a sentinel pattern. Call this ONCE, as the very first statement
   in main(), before anything else runs: the earlier this runs, the more
   of the stack is still untouched and paintable, so the later
   high-water-mark reading is accurate back to boot. Calling it any
   later only undercounts (treats already-used-but-since-freed stack as
   "available"), never overcounts -- safe failure direction for a bench
   diagnostic, not a safety system. */
void MemStats_PaintStack(void);

/* Scans upward from `end` for the first word that no longer matches the
   paint pattern -- that boundary is the deepest point the stack pointer
   has reached since MemStats_PaintStack() ran. Returns bytes used
   (distance from that boundary to the top of stack, _estack).

   Caveat inherent to the stack-painting technique: if real stack data
   ever legitimately contains the exact sentinel word, this undercounts
   by however much of that data sits below the true high-water mark --
   accepted false-negative risk of the technique itself, not a bug here;
   pick an unusual-enough pattern (see mem_stats.c) to make it unlikely. */
uint32_t MemStats_GetStackUsedBytes(void);

/* Total stack region size in bytes: _estack minus the linker's `end`
   symbol (this firmware has no heap allocations, so the entire region
   above .bss is available to the stack, not split with a real heap). */
uint32_t MemStats_GetStackTotalBytes(void);

#ifdef __cplusplus
}
#endif

#endif /* MEM_STATS_H */
