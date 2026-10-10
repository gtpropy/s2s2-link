/*
 * ST's advanced-trace layer (stm32_adv_trace.c + stm32_tiny_vsnprintf.c + stm32_trace_driver.c) formatted every middleware log
 * line into a buffer and then handed it to a driver whose Send() did nothing: about 2 KB of flash and CPU time for output that
 * was discarded. The radio driver still calls MW_LOG(), so this stub keeps the call and drops the formatting.
 * (The satellite's own log goes through uart1_printf(), the ground station's through uart2_printf().)
 */
#include <stdint.h>
#include "stm32_adv_trace.h"

UTIL_ADV_TRACE_Status_t UTIL_ADV_TRACE_COND_FSend(uint32_t VerboseLevel, uint32_t Region, uint32_t TimeStampState,
                                                  const char *strFormat, ...)
{
    (void)VerboseLevel; (void)Region; (void)TimeStampState; (void)strFormat;
    return UTIL_ADV_TRACE_OK;
}
