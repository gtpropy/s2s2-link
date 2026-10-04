/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    radio_board_if.c
  * @author  MCD Application Team
  * @brief   This file provides an interface layer between MW and Radio Board
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2021 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "radio_board_if.h"
#include "stm32wlxx_hal.h"

/* USER CODE BEGIN Includes */

/* GpioSet/GpioReset/GpioIsSet now live in radio_board_if.h (header-inline)
   so satellite_app.c's AMP_5V_EN/AMP_3V3_EN toggles share the same path. */

/* USER CODE END Includes */

/* External variables ---------------------------------------------------------*/
/* USER CODE BEGIN EV */

/* USER CODE END EV */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

static uint8_t s_pa_enabled = 1U;
static RBI_Switch_TypeDef s_tx_switch_config = RBI_SWITCH_RFO_LP; /* Default: 3.3V PA */

void RBI_EnablePA(uint8_t enable)
{
    s_pa_enabled = enable ? 1U : 0U;

    if (!s_pa_enabled)
    {
        /* Global PA & 5V DC/DC OFF */
        GpioReset(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN);
        GpioReset(AMP_5V_EN_PORT, AMP_5V_EN_PIN);
        GpioReset(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN);
    }
}

uint8_t RBI_IsPAEnabled(void)
{
    return s_pa_enabled;
}

void RBI_Enable5VDCDC(uint8_t enable)
{
    if (enable) GpioSet(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN);
    else GpioReset(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN);
}

void RBI_SetTxSwitchConfig(RBI_Switch_TypeDef config)
{
    s_tx_switch_config = config;
}

RBI_Switch_TypeDef RBI_GetTxSwitchConfig(void)
{
    return s_tx_switch_config;
}

/* Exported functions --------------------------------------------------------*/
int32_t RBI_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* Enable GPIOA and GPIOC clocks in CPU1 & CPU2 domains */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    RCC->AHB2ENR   |= (RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOCEN);
    RCC->C2AHB2ENR |= (RCC_C2AHB2ENR_GPIOAEN | RCC_C2AHB2ENR_GPIOCEN);

    /* Configure 5V DC/DC Converter Enable (PA0: High = 5V ON, Low = OFF) */
    GPIO_InitStruct.Pin   = DCDC_5V_EN_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(DCDC_5V_EN_PORT, &GPIO_InitStruct);

    /* Configure 3.3V PA Enable (PC2), 5V PA Enable (PC3), RF Switch FE_CTRL1 (PC4), RF Switch FE_CTRL2 (PC5) */
    GPIO_InitStruct.Pin   = AMP_3V3_EN_PIN | AMP_5V_EN_PIN | FE_CTRL1_PIN | FE_CTRL2_PIN;
    GPIO_InitStruct.Mode  = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull  = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

    /* Default: All OFF (PA0=0, PC2=0, PC3=0, PC4=0, PC5=0) */
    GpioReset(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN);
    GpioReset(GPIOC, AMP_3V3_EN_PIN | AMP_5V_EN_PIN | FE_CTRL1_PIN | FE_CTRL2_PIN);

    return 0;
}

int32_t RBI_DeInit(void)
{
    /* Safe shutdown: deassert all controls */
    GpioReset(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN);
    HAL_GPIO_DeInit(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN);

    GpioReset(GPIOC, AMP_3V3_EN_PIN | AMP_5V_EN_PIN | FE_CTRL1_PIN | FE_CTRL2_PIN);
    HAL_GPIO_DeInit(GPIOC, AMP_3V3_EN_PIN | AMP_5V_EN_PIN | FE_CTRL1_PIN | FE_CTRL2_PIN);

    return 0;
}

int32_t RBI_ConfigRFSwitch(RBI_Switch_TypeDef Config)
{
    /* Ensure GPIOA and GPIOC clocks are active in CPU2 domain */
    RCC->C2AHB2ENR |= (RCC_C2AHB2ENR_GPIOAEN | RCC_C2AHB2ENR_GPIOCEN);

    /* If generic TX is requested by SUBGRF_SetSwitch(), route according to active config */
    if (Config == RBI_SWITCH_RFO_LP || Config == RBI_SWITCH_RFO_HP)
    {
        if (s_tx_switch_config == RBI_SWITCH_RFO_LP5V)
        {
            Config = RBI_SWITCH_RFO_LP5V;
        }
    }
    else if (Config == RBI_SWITCH_RFO_LP5V)
    {
        s_tx_switch_config = RBI_SWITCH_RFO_LP5V;
    }

    switch (Config)
    {
    case RBI_SWITCH_RFO_LP5V:
        /*
         * TRANSMIT MODE (5V PA for GFSK):
         * 1. Disable 3.3V External PA (PC2 / SO2 = 0) - Prevents dual-PA power overload & brownout
         * 2. Enable 5V DC/DC Converter (PA0 = 1) - Power rail for 5V PA
         * 3. DC/DC rail ramp-up settle delay (~5 ms)
         * 4. RF Switch -> TX Path (FE_CTRL1 = 0, FE_CTRL2 = 1)
         * 5. Assert 5V External PA (PC3 / SI2 = 1) - 5V power amplifier active
         * 6. PA bias network & switch settle delay (~1 ms)
         */
        if (!GpioIsSet(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN))
        {
            GpioSet(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN);
            /* DC/DC 5V voltage boost initial capacitor ramp settle (~5 ms at 48 MHz) */
            for (volatile uint32_t i = 0; i < 60000; i++) { __NOP(); }
        }

        GpioReset(FE_CTRL1_PORT, FE_CTRL1_PIN);
        GpioSet(FE_CTRL2_PORT, FE_CTRL2_PIN);

        if (s_pa_enabled)
        {
            GpioSet(AMP_5V_EN_PORT, AMP_5V_EN_PIN);
        }
        else
        {
            GpioReset(AMP_5V_EN_PORT, AMP_5V_EN_PIN);
        }

        /* PA bias & RF switch settle delay (~1 ms at 48 MHz) */
        for (volatile uint32_t i = 0; i < 12000; i++) { __NOP(); }
        break;

    case RBI_SWITCH_RFO_HP:
    case RBI_SWITCH_RFO_LP:
        /*
         * TRANSMIT MODE (3.3V PA for CW):
         * 1. Ensure 5V DC/DC is OFF (PA0 = 0) - ONLY ON while GFSK send
         * 2. Disable 5V External PA (PC3 / SI2 = 0)
         * 3. RF Switch -> TX Path (FE_CTRL1 = 0, FE_CTRL2 = 1)
         * 4. Assert 3.3V External Power Amplifier Enable (PC2 / SO2 = 1)
         * 5. PA bias network & switch settle delay (~200 us)
         */
        s_tx_switch_config = RBI_SWITCH_RFO_LP;
        GpioReset(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN);
        GpioReset(AMP_5V_EN_PORT, AMP_5V_EN_PIN);

        GpioReset(FE_CTRL1_PORT, FE_CTRL1_PIN);
        GpioSet(FE_CTRL2_PORT, FE_CTRL2_PIN);

        if (s_pa_enabled)
        {
            GpioSet(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN);
        }
        else
        {
            GpioReset(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN);
        }

        /* PA bias & RF switch settle delay (~200 us at 48 MHz) */
        for (volatile uint32_t i = 0; i < 2500; i++) { __NOP(); }
        break;

    case RBI_SWITCH_RX:
        /*
         * RECEIVE MODE:
         * 1. Disable 5V DC/DC Converter (PA0 = 0)
         * 2. Disable 5V External PA (PC3 = 0)
         * 3. Disable 3.3V External PA (PC2 = 0)
         * 4. RF Switch -> RX Path (FE_CTRL1 = 1, FE_CTRL2 = 0)
         */
        GpioReset(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN);
        GpioReset(AMP_5V_EN_PORT, AMP_5V_EN_PIN);
        GpioReset(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN);
        GpioSet(FE_CTRL1_PORT, FE_CTRL1_PIN);
        GpioReset(FE_CTRL2_PORT, FE_CTRL2_PIN);

        /* Settle delay (~50 us) */
        for (volatile uint32_t i = 0; i < 500; i++) { __NOP(); }
        break;

    case RBI_SWITCH_OFF:
    default:
        /*
         * STANDBY / OFF:
         * Deassert 5V DC/DC, 5V PA, 3.3V PA, and both RF switch control pins
         */
        GpioReset(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN);
        GpioReset(AMP_5V_EN_PORT, AMP_5V_EN_PIN);
        GpioReset(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN);
        GpioReset(FE_CTRL1_PORT, FE_CTRL1_PIN);
        GpioReset(FE_CTRL2_PORT, FE_CTRL2_PIN);
        break;
    }

    return 0;
}

int32_t RBI_GetTxConfig(void)
{
    /* Support both Low Power and High Power configurations */
    return RBI_CONF_RFO_LP_HP;
}

int32_t RBI_IsTCXO(void)
{
    return IS_TCXO_SUPPORTED;
}

int32_t RBI_IsDCDC(void)
{
    return IS_DCDC_SUPPORTED;
}

int32_t RBI_GetRFOMaxPowerConfig(RBI_RFOMaxPowerConfig_TypeDef Config)
{
  if (Config == RBI_RFO_LP_MAXPOWER)
  {
    return 14; /* 14 dBm: optimal drive for linear GFSK and safe current budget (prevents brownout reset) */
  }
  else
  {
    return 22; /* 22 dBm */
  }
}
/* USER CODE BEGIN EF */

/* USER CODE END EF */

/* Private Functions Definition -----------------------------------------------*/
/* USER CODE BEGIN PrFD */

/* USER CODE END PrFD */
