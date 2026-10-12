/*
 * ADDED: status LEDs for the image-transfer proof. CPU2 owns the yellow (green+red) "transmitting" state;
 * CPU1 (NuttX app s2s2obc) drives blue = writing its flash and green = reading it. Both cores write the
 * same GPIOB pins with BSRR (atomic set/reset), so they do not need to lock against each other.
 * The bench samples GPIOB->ODR over SWD (tools/console LedTrace) to prove the colours really happened.
 */
#include "obc_led.h"
#include "stm32wlxx.h"
#include "stm32wlxx_hal.h"

/* ---- LEDs: PB15 blue, PB9 green, PB11 red --------------------------------------------- */
#define LED_B   (1UL << 15)
#define LED_G   (1UL << 9)
#define LED_R   (1UL << 11)

#ifdef SAT_CUBUS
/*
 * CUBUS has no LEDs on these pins: PB15 = ADC_MOSI, PB9 = CS_MPU (gyro chip select), PB11 = CS_MAG (magnetometer chip select). They
 * belong to CPU1's SPI buses, so CPU2 must not configure or toggle them. The functions stay (the call sites are unchanged) and do nothing.
 */
void Obc_Led(uint8_t mask)  { (void)mask; }
void Obc_Led_Tick(void)     { }
#else
static uint32_t s_led_last;
static uint8_t  s_led_init;

/* Lazy init: enable the GPIOB clock for CPU2 and set PB15/PB9/PB11 to push-pull outputs, all off. */
static void led_init(void)
{
    if (s_led_init) return;
    RCC->AHB2ENR   |= RCC_AHB2ENR_GPIOBEN;
    RCC->C2AHB2ENR |= RCC_C2AHB2ENR_GPIOBEN;
    uint32_t m = GPIOB->MODER;
    m &= ~((3UL << 30) | (3UL << 18) | (3UL << 22));
    m |=  ((1UL << 30) | (1UL << 18) | (1UL << 22));
    GPIOB->MODER = m;
    GPIOB->BSRR = (LED_B | LED_G | LED_R) << 16;
    s_led_init = 1;
}

/* Show exactly the colours in mask (OBC_LED_*), turn the others off. Each call restarts the idle timer. */
void Obc_Led(uint8_t mask)
{
    led_init();
    uint32_t on = ((mask & OBC_LED_BLUE) ? LED_B : 0) | ((mask & OBC_LED_GREEN) ? LED_G : 0) |
                  ((mask & OBC_LED_RED) ? LED_R : 0);
    GPIOB->BSRR = on | (((LED_B | LED_G | LED_R) & ~on) << 16);
    s_led_last = HAL_GetTick();
}

/* Called every main-loop pass: switch the LED off OBC_LED_IDLE_MS after the last Obc_Led(), so a finished
 * or aborted transfer does not leave a colour lit (and a long RF burst keeps it lit via a second Obc_Led()). */
void Obc_Led_Tick(void)
{
    if (s_led_init && (GPIOB->ODR & (LED_B | LED_G | LED_R)) &&
        (HAL_GetTick() - s_led_last) > OBC_LED_IDLE_MS)
    {
        GPIOB->BSRR = (LED_B | LED_G | LED_R) << 16;
    }
}
#endif /* SAT_CUBUS */
