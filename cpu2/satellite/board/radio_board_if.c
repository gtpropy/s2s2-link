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
#include "ll_port.h"
#include "stm32wlxx_hal.h"

/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* External variables ---------------------------------------------------------*/
/* USER CODE BEGIN EV */
extern bool Satellite_GetDCDCHold(void);
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
static RBI_Switch_TypeDef s_tx_switch_config = RBI_SWITCH_RFO_LP; /* Default: LP 3.3V PA for CW */

void RBI_EnablePA(uint8_t enable)
{
    s_pa_enabled = enable ? 1U : 0U;

    if (!s_pa_enabled)
    {
        /* Global PA & 5V DC/DC OFF */
        HAL_GPIO_WritePin(AMP_3V3_EN_PORT,
                          AMP_3V3_EN_PIN,
                          GPIO_PIN_RESET);

        HAL_GPIO_WritePin(AMP_5V_EN_PORT,
                          AMP_5V_EN_PIN,
                          GPIO_PIN_RESET);

        HAL_GPIO_WritePin(DCDC_5V_EN_PORT,
                          DCDC_5V_EN_PIN,
                          GPIO_PIN_RESET);
    }
}

uint8_t RBI_IsPAEnabled(void)
{
    return s_pa_enabled;
}

void RBI_Enable5VDCDC(uint8_t enable)
{
#ifdef SAT_NUCLEO_FRONTEND
    /* ADDED: FRONTEND=NUCLEO. A stock Nucleo has no 5 V DC/DC; driving PA0 would fight the user button. */
    (void)enable;   /* no 5 V boost on a Nucleo; PA0 is the user button */
    return;
#endif
    HAL_GPIO_WritePin(DCDC_5V_EN_PORT,
                      DCDC_5V_EN_PIN,
                      enable ? GPIO_PIN_SET : GPIO_PIN_RESET);
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
    /* Enable GPIOA and GPIOC clocks in CPU1 & CPU2 domains */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();
    RCC->AHB2ENR   |= (RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOCEN);
    RCC->C2AHB2ENR |= (RCC_C2AHB2ENR_GPIOAEN | RCC_C2AHB2ENR_GPIOCEN);

    /* Configure 5V DC/DC Converter Enable (PA0: High = 5V ON, Low = OFF) */
    /* CHANGED: GPIO_InitTypeDef + HAL_GPIO_Init -> LL_GPIO_OutputInit (push-pull, no pull, very high speed) */
    LL_GPIO_OutputInit(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN);

    /* Configure 3.3V PA Enable (PC2), 5V PA Enable (PC3), RF Switch FE_CTRL1 (PC4), RF Switch FE_CTRL2 (PC5) */
    LL_GPIO_OutputInit(GPIOC, AMP_3V3_EN_PIN | AMP_5V_EN_PIN | FE_CTRL1_PIN | FE_CTRL2_PIN);

    /* Default: All OFF (PA0=0, PC2=0, PC3=0, PC4=0, PC5=0) */
    HAL_GPIO_WritePin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOC, AMP_3V3_EN_PIN | AMP_5V_EN_PIN | FE_CTRL1_PIN | FE_CTRL2_PIN, GPIO_PIN_RESET);

    return 0;
}

int32_t RBI_DeInit(void)
{
    /* Safe shutdown: deassert all controls */
    HAL_GPIO_WritePin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN, GPIO_PIN_RESET);
    HAL_GPIO_DeInit(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN);

    HAL_GPIO_WritePin(GPIOC, AMP_3V3_EN_PIN | AMP_5V_EN_PIN | FE_CTRL1_PIN | FE_CTRL2_PIN, GPIO_PIN_RESET);
    HAL_GPIO_DeInit(GPIOC, AMP_3V3_EN_PIN | AMP_5V_EN_PIN | FE_CTRL1_PIN | FE_CTRL2_PIN);

    return 0;
}

int32_t RBI_ConfigRFSwitch(RBI_Switch_TypeDef Config)
{
    /* Ensure GPIOA and GPIOC clocks are active in CPU2 domain */
    RCC->C2AHB2ENR |= (RCC_C2AHB2ENR_GPIOAEN | RCC_C2AHB2ENR_GPIOCEN);

#ifdef SAT_NUCLEO_FRONTEND
    /* ADDED (the fix that made the satellite audible): the reference switch code is for the custom board and
     * left a stock Nucleo with the HP switch path selected while the LP PA was on, so nothing reached the
     * antenna. This block uses the same pin table as the ground station. 2500-iteration wait = switch settle. */
    /* Stock Nucleo-WL55JC switch: PC3 = FE_CTRL3 (enable), PC4 = FE_CTRL1, PC5 = FE_CTRL2.
     * RX: C1=1 C2=0   HP TX: C1=0 C2=1   LP TX: C1=1 C2=1   (same table as the ground station). */
    {
        GPIO_PinState c1, c2, c3 = GPIO_PIN_SET;
        switch (Config)
        {
        case RBI_SWITCH_RX:     c1 = GPIO_PIN_SET;   c2 = GPIO_PIN_RESET; break;
        case RBI_SWITCH_OFF:    c1 = GPIO_PIN_RESET; c2 = GPIO_PIN_RESET; c3 = GPIO_PIN_RESET; break;
        case RBI_SWITCH_RFO_LP: c1 = GPIO_PIN_SET;   c2 = GPIO_PIN_SET;   break;
        default:                c1 = GPIO_PIN_RESET; c2 = GPIO_PIN_SET;   break;   /* HP, LP5V */
        }
        HAL_GPIO_WritePin(AMP_5V_EN_PORT, AMP_5V_EN_PIN, c3);
        HAL_GPIO_WritePin(FE_CTRL1_PORT, FE_CTRL1_PIN, c1);
        HAL_GPIO_WritePin(FE_CTRL2_PORT, FE_CTRL2_PIN, c2);
        for (volatile uint32_t i = 0; i < 2500; i++) { __NOP(); }
        return 0;
    }
#endif

    /* If generic TX is requested by SUBGRF_SetSwitch(), route according to active config */
    if (Config == RBI_SWITCH_RFO_LP || Config == RBI_SWITCH_RFO_HP)
    {
        if (s_tx_switch_config == RBI_SWITCH_RFO_LP5V)
        {
            Config = RBI_SWITCH_RFO_LP5V;
        }
        else
        {
            Config = RBI_SWITCH_RFO_LP;
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
         * TRANSMIT MODE (5V PA for GMSK). Never overlap 3.3V and 5V PAs:
         * 1. 3.3V PA OFF first
         * 2. 5V DC/DC ON (PA0) and settle
         * 3. 5V PA ON (PC3)
         * 4. RF switch TX last
         */
        s_tx_switch_config = RBI_SWITCH_RFO_LP5V;

        HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN, GPIO_PIN_RESET);
        for (volatile uint32_t i = 0; i < 12000; i++) { __NOP(); }

        if (HAL_GPIO_ReadPin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN) != GPIO_PIN_SET)
        {
            HAL_GPIO_WritePin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN, GPIO_PIN_SET);
            for (volatile uint32_t i = 0; i < 240000; i++) { __NOP(); } /* ~20 ms */
        }

        HAL_GPIO_WritePin(AMP_5V_EN_PORT, AMP_5V_EN_PIN,
                          s_pa_enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
        for (volatile uint32_t i = 0; i < 60000; i++) { __NOP(); } /* ~5 ms bias */

        HAL_GPIO_WritePin(FE_CTRL1_PORT, FE_CTRL1_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(FE_CTRL2_PORT, FE_CTRL2_PIN, GPIO_PIN_SET);
        for (volatile uint32_t i = 0; i < 12000; i++) { __NOP(); }
        break;

    case RBI_SWITCH_RFO_HP:
    case RBI_SWITCH_RFO_LP:
        /*
         * TRANSMIT MODE (3.3V PA for CW Morse Beacon):
         * 1. 5V DC/DC is strictly OFF (PA0 = 0) - ONLY ON during GMSK
         * 2. 5V External PA is strictly OFF (PC3 = 0)
         * 3. RF Switch -> TX Path (FE_CTRL1 = 0, FE_CTRL2 = 1)
         * 4. Assert 3.3V External Power Amplifier (PC2 = 1)
         * 5. PA bias & switch settle delay (~200 us)
         */
        s_tx_switch_config = RBI_SWITCH_RFO_LP;
        HAL_GPIO_WritePin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(AMP_5V_EN_PORT, AMP_5V_EN_PIN, GPIO_PIN_RESET);

        HAL_GPIO_WritePin(FE_CTRL1_PORT, FE_CTRL1_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(FE_CTRL2_PORT, FE_CTRL2_PIN, GPIO_PIN_SET);

        if (s_pa_enabled)
        {
            HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN, GPIO_PIN_SET);
        }
        else
        {
            HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN, GPIO_PIN_RESET);
        }

        /* PA bias & RF switch settle delay (~200 us at 48 MHz) */
        for (volatile uint32_t i = 0; i < 2500; i++) { __NOP(); }
        break;

    case RBI_SWITCH_RX:
        /*
         * RECEIVE MODE:
         * 1. Disable 5V External Power Amplifier (PC3 = 0)
         * 2. Disable 5V DC/DC Converter (PA0 = 0) unless DCDC hold is active
         * 3. 3.3V External Power Amplifier back ON (PC2 = 1): it is always ON
         *    except while the 5V PA transmits GMSK
         * 4. RF Switch -> RX Path (FE_CTRL1 = 1, FE_CTRL2 = 0)
         *
         * During a GMSK downlink (DCDC hold) keep the 5V PA biased. Dropping
         * PC3 and turning PC2 on mid-session brown-outs the board.
         */
        if (Satellite_GetDCDCHold())
        {
            break;
        }
        HAL_GPIO_WritePin(AMP_5V_EN_PORT, AMP_5V_EN_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN,
                          s_pa_enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
        HAL_GPIO_WritePin(FE_CTRL1_PORT, FE_CTRL1_PIN, GPIO_PIN_SET);
        HAL_GPIO_WritePin(FE_CTRL2_PORT, FE_CTRL2_PIN, GPIO_PIN_RESET);

        /* Settle delay (~50 us) */
        for (volatile uint32_t i = 0; i < 500; i++) { __NOP(); }
        break;

    case RBI_SWITCH_OFF:
    default:
        /*
         * STANDBY / OFF:
         * Deassert 5V DC/DC, 5V PA, 3.3V PA, and both RF switch control pins
         */
        HAL_GPIO_WritePin(DCDC_5V_EN_PORT, DCDC_5V_EN_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(AMP_5V_EN_PORT, AMP_5V_EN_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(AMP_3V3_EN_PORT, AMP_3V3_EN_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(GPIOC, FE_CTRL1_PIN | FE_CTRL2_PIN, GPIO_PIN_RESET);
        break;
    }

    return 0;
}

int32_t RBI_GetTxConfig(void)
{
    /* Satellite uses SX1262 RFO_LP (PB2 / Low Power) wired to external 3.3V and 5V PAs */
    return RBI_CONF_RFO_LP;
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
    return 14; /* 14 dBm: optimal drive for linear GMSK and safe current budget (prevents brownout reset) */
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
