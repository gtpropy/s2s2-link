#include "stm32wlxx.h"
#include "cmsis_gcc.h"

void Error_Handler(void)
{
    __disable_irq();

    while (1)
    {
    }
}