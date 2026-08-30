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
#include "uart_debug.h"
#include "board_pins.h"

/* USER CODE BEGIN Includes */

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

/* Exported functions --------------------------------------------------------*/
int32_t RBI_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

#ifdef BOARD_HAS_EXTRA_FRONTEND_PINS
    GPIO_InitStruct.Pin = BOARD_EXTRA_PIN_A | BOARD_EXTRA_PIN_B;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(BOARD_EXTRA_GPIO_PORT, &GPIO_InitStruct);
#endif

    /* RF switch (FE_CTRL1/2/3) -- same pins on every board variant. */
    GPIO_InitStruct.Pin = BOARD_FE_CTRL_ALL_PINS;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    HAL_GPIO_Init(BOARD_FE_CTRL_GPIO_PORT, &GPIO_InitStruct);

    /* Default: All OFF */
#ifdef BOARD_HAS_EXTRA_FRONTEND_PINS
    HAL_GPIO_WritePin(BOARD_EXTRA_GPIO_PORT, BOARD_EXTRA_PIN_A | BOARD_EXTRA_PIN_B, GPIO_PIN_RESET);
#endif
    HAL_GPIO_WritePin(BOARD_FE_CTRL_GPIO_PORT, BOARD_FE_CTRL_ALL_PINS, GPIO_PIN_RESET);

    return 0;
}

int32_t RBI_DeInit(void)
{
#ifdef BOARD_HAS_EXTRA_FRONTEND_PINS
    HAL_GPIO_WritePin(BOARD_EXTRA_GPIO_PORT, BOARD_EXTRA_PIN_A | BOARD_EXTRA_PIN_B, GPIO_PIN_RESET);
#endif
    HAL_GPIO_WritePin(BOARD_FE_CTRL_GPIO_PORT, BOARD_FE_CTRL_ALL_PINS, GPIO_PIN_RESET);

#ifdef BOARD_HAS_EXTRA_FRONTEND_PINS
    HAL_GPIO_DeInit(BOARD_EXTRA_GPIO_PORT, BOARD_EXTRA_PIN_A | BOARD_EXTRA_PIN_B);
#endif
    HAL_GPIO_DeInit(BOARD_FE_CTRL_GPIO_PORT, BOARD_FE_CTRL_ALL_PINS);

    return 0;
}

int32_t RBI_ConfigRFSwitch(RBI_Switch_TypeDef Config)
{
    switch (Config)
    {
    case RBI_SWITCH_RFO_HP:
        uart2_puts("RFSWITCH TX HP\r\n");

#ifdef BOARD_HAS_EXTRA_FRONTEND_PINS
        /* External PA/rail A ON, rail B OFF */
        HAL_GPIO_WritePin(BOARD_EXTRA_GPIO_PORT, BOARD_EXTRA_PIN_A, GPIO_PIN_SET);
        HAL_GPIO_WritePin(BOARD_EXTRA_GPIO_PORT, BOARD_EXTRA_PIN_B, GPIO_PIN_RESET);
#endif

        /* RF Switch -> TX HP to SMA: CTRL3=1, CTRL1=0, CTRL2=1 */
        HAL_GPIO_WritePin(BOARD_FE_CTRL_GPIO_PORT, BOARD_FE_CTRL3_PIN, GPIO_PIN_SET);
        HAL_GPIO_WritePin(BOARD_FE_CTRL_GPIO_PORT, BOARD_FE_CTRL1_PIN, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(BOARD_FE_CTRL_GPIO_PORT, BOARD_FE_CTRL2_PIN, GPIO_PIN_SET);
        break;

    case RBI_SWITCH_RFO_LP:
        uart2_puts("RFSWITCH TX LP\r\n");

#ifdef BOARD_HAS_EXTRA_FRONTEND_PINS
        /* External PA/rail A ON, rail B OFF */
        HAL_GPIO_WritePin(BOARD_EXTRA_GPIO_PORT, BOARD_EXTRA_PIN_A, GPIO_PIN_SET);
        HAL_GPIO_WritePin(BOARD_EXTRA_GPIO_PORT, BOARD_EXTRA_PIN_B, GPIO_PIN_RESET);
#endif

        /* RF Switch -> TX LP to SMA: CTRL3=1, CTRL1=1, CTRL2=1 */
        HAL_GPIO_WritePin(BOARD_FE_CTRL_GPIO_PORT, BOARD_FE_CTRL3_PIN, GPIO_PIN_SET);
        HAL_GPIO_WritePin(BOARD_FE_CTRL_GPIO_PORT, BOARD_FE_CTRL1_PIN, GPIO_PIN_SET);
        HAL_GPIO_WritePin(BOARD_FE_CTRL_GPIO_PORT, BOARD_FE_CTRL2_PIN, GPIO_PIN_SET);
        break;

    case RBI_SWITCH_RX:
        uart2_puts("RFSWITCH RX\r\n");

#ifdef BOARD_HAS_EXTRA_FRONTEND_PINS
        /* External PA/rail A OFF, rail B ON */
        HAL_GPIO_WritePin(BOARD_EXTRA_GPIO_PORT, BOARD_EXTRA_PIN_A, GPIO_PIN_RESET);
        HAL_GPIO_WritePin(BOARD_EXTRA_GPIO_PORT, BOARD_EXTRA_PIN_B, GPIO_PIN_SET);
#endif

        /* RF Switch -> RX from SMA: CTRL3=1, CTRL1=1, CTRL2=0 */
        HAL_GPIO_WritePin(BOARD_FE_CTRL_GPIO_PORT, BOARD_FE_CTRL3_PIN, GPIO_PIN_SET);
        HAL_GPIO_WritePin(BOARD_FE_CTRL_GPIO_PORT, BOARD_FE_CTRL1_PIN, GPIO_PIN_SET);
        HAL_GPIO_WritePin(BOARD_FE_CTRL_GPIO_PORT, BOARD_FE_CTRL2_PIN, GPIO_PIN_RESET);
        break;

    case RBI_SWITCH_OFF:
    default:
        uart2_puts("RFSWITCH OFF\r\n");

#ifdef BOARD_HAS_EXTRA_FRONTEND_PINS
        HAL_GPIO_WritePin(BOARD_EXTRA_GPIO_PORT, BOARD_EXTRA_PIN_A | BOARD_EXTRA_PIN_B, GPIO_PIN_RESET);
#endif
        HAL_GPIO_WritePin(BOARD_FE_CTRL_GPIO_PORT, BOARD_FE_CTRL_ALL_PINS, GPIO_PIN_RESET);
        break;
    }

    return 0;
}

int32_t RBI_GetTxConfig(void)
{
  /* USER CODE BEGIN RBI_GetTxConfig_1 */

  /* USER CODE END RBI_GetTxConfig_1 */
#if defined(USE_BSP_DRIVER)
  /* Important note: BSP code is board dependent
   * STM32WL_Nucleo code can be found
   *       either in STM32CubeWL package under Drivers/BSP/STM32WLxx_Nucleo/
   *       or at https://github.com/STMicroelectronics/STM32CubeWL/tree/main/Drivers/BSP/STM32WLxx_Nucleo/
   * 1/ For User boards, the BSP/STM32WLxx_Nucleo/ directory can be copied and replaced in the project. The copy must then be updated depending:
   *       on board RF switch configuration (pin control, number of port etc)
   *       on TCXO configuration
   *       on DC/DC configuration
   *       on maximum output power that the board can deliver*/
  return BSP_RADIO_GetTxConfig();
#else
  /* 2/ Board has a fixed external PA wired to the SX1262 RFO_HP pin */
  int32_t retcode = RBI_CONF_RFO_HP;
  /* USER CODE BEGIN RBI_GetTxConfig_2 */
  /* USER CODE END RBI_GetTxConfig_2 */
  return retcode;
#endif  /* USE_BSP_DRIVER */
}

int32_t RBI_IsTCXO(void)
{
  /* USER CODE BEGIN RBI_IsTCXO_1 */

  /* USER CODE END RBI_IsTCXO_1 */
#if defined(USE_BSP_DRIVER)
  /* Important note: BSP code is board dependent
   * STM32WL_Nucleo code can be found
   *       either in STM32CubeWL package under Drivers/BSP/STM32WLxx_Nucleo/
   *       or at https://github.com/STMicroelectronics/STM32CubeWL/tree/main/Drivers/BSP/STM32WLxx_Nucleo/
   * 1/ For User boards, the BSP/STM32WLxx_Nucleo/ directory can be copied and replaced in the project. The copy must then be updated depending:
   *       on board RF switch configuration (pin control, number of port etc)
   *       on TCXO configuration
   *       on DC/DC configuration
   *       on maximum output power that the board can deliver*/
  return BSP_RADIO_IsTCXO();
#else
  /* 2/ Or implement RBI_IsTCXO here */
  int32_t retcode = IS_TCXO_SUPPORTED;
  /* USER CODE BEGIN RBI_IsTCXO_2 */
#warning user to provide its board code or to call his board driver functions
  /* USER CODE END RBI_IsTCXO_2 */
  return retcode;
#endif  /* USE_BSP_DRIVER  */
}

int32_t RBI_IsDCDC(void)
{
  /* USER CODE BEGIN RBI_IsDCDC_1 */

  /* USER CODE END RBI_IsDCDC_1 */
#if defined(USE_BSP_DRIVER)
  /* Important note: BSP code is board dependent
   * STM32WL_Nucleo code can be found
   *       either in STM32CubeWL package under Drivers/BSP/STM32WLxx_Nucleo/
   *       or at https://github.com/STMicroelectronics/STM32CubeWL/tree/main/Drivers/BSP/STM32WLxx_Nucleo/
   * 1/ For User boards, the BSP/STM32WLxx_Nucleo/ directory can be copied and replaced in the project. The copy must then be updated depending:
   *       on board RF switch configuration (pin control, number of port etc)
   *       on TCXO configuration
   *       on DC/DC configuration
   *       on maximum output power that the board can deliver*/
  return BSP_RADIO_IsDCDC();
#else
  /* 2/ Or implement RBI_IsDCDC here */
  int32_t retcode = IS_DCDC_SUPPORTED;
  /* USER CODE BEGIN RBI_IsDCDC_2 */
#warning user to provide its board code or to call his board driver functions
  /* USER CODE END RBI_IsDCDC_2 */
  return retcode;
#endif  /* USE_BSP_DRIVER  */
}

int32_t RBI_GetRFOMaxPowerConfig(RBI_RFOMaxPowerConfig_TypeDef Config)
{
  /* USER CODE BEGIN RBI_GetRFOMaxPowerConfig_1 */

  /* USER CODE END RBI_GetRFOMaxPowerConfig_1 */
#if defined(USE_BSP_DRIVER)
  /* Important note: BSP code is board dependent
   * STM32WL_Nucleo code can be found
   *       either in STM32CubeWL package under Drivers/BSP/STM32WLxx_Nucleo/
   *       or at https://github.com/STMicroelectronics/STM32CubeWL/tree/main/Drivers/BSP/STM32WLxx_Nucleo/
   * 1/ For User boards, the BSP/STM32WLxx_Nucleo/ directory can be copied and replaced in the project. The copy must then be updated depending:
   *       on board RF switch configuration (pin control, number of port etc)
   *       on TCXO configuration
   *       on DC/DC configuration
   *       on maximum output power that the board can deliver*/
  return BSP_RADIO_GetRFOMaxPowerConfig((BSP_RADIO_RFOMaxPowerConfig_TypeDef) Config);
#else
  /* 2/ Or implement RBI_RBI_GetRFOMaxPowerConfig here */
  int32_t ret = 0;
  /* USER CODE BEGIN RBI_GetRFOMaxPowerConfig_2 */
#warning user to provide its board code or to call his board driver functions
  if (Config == RBI_RFO_LP_MAXPOWER)
  {
    ret = 15; /*dBm*/
  }
  else
  {
    ret = 22; /*dBm*/
  }
  /* USER CODE END RBI_GetRFOMaxPowerConfig_2 */
  return ret;
#endif  /* USE_BSP_DRIVER  */
}
/* USER CODE BEGIN EF */

/* USER CODE END EF */

/* Private Functions Definition -----------------------------------------------*/
/* USER CODE BEGIN PrFD */

/* USER CODE END PrFD */
