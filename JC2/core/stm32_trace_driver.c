#include "stm32_adv_trace.h"


UTIL_ADV_TRACE_Status_t Trace_Init(void (*cb)(void *))
{
    (void)cb;

    return UTIL_ADV_TRACE_OK;
}


static UTIL_ADV_TRACE_Status_t Trace_DeInit(void)
{
    return UTIL_ADV_TRACE_OK;
}


static UTIL_ADV_TRACE_Status_t Trace_Send(uint8_t *pData, uint16_t size)
{
    (void)pData;
    (void)size;

    return UTIL_ADV_TRACE_OK;
}


const UTIL_ADV_TRACE_Driver_s UTIL_TraceDriver =
{
    .Init = Trace_Init,
    .DeInit = Trace_DeInit,
    .Send = Trace_Send
};