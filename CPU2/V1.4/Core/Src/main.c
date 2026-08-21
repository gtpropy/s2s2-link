/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
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
#include "main.h"
#include "app_subghz_phy.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stdio.h"
#include "string.h"
#include "radio_driver.h"
#include "UART_F.h"
#include "project_config.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */



/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
PacketParams_t     pkt_params;
ModulationParams_t mod_params;
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
SUBGHZ_HandleTypeDef hsubghz;

TIM_HandleTypeDef htim2;

UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_USART2_UART_Init(void);
static void MX_TIM2_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */


extern uint8_t RAWTX_Data_Bffr[];
extern int16_t RAWTX_Data_Bffr_length;

uint8_t TRX_MODE;
uint8_t G3RUH_TXDataBuffer[255] ;


#define RX_FREQ_CENTER    435300000UL   // your target frequency Hz
#define RX_FREQ_MARGIN    20000UL       // ±30 kHz max correction window

#define RX_FREQ_MIN       (RX_FREQ_CENTER - RX_FREQ_MARGIN)  // 435250000
#define RX_FREQ_MAX       (RX_FREQ_CENTER + RX_FREQ_MARGIN)  // 435350000

void delay_us(uint32_t us)
{
    uint32_t start = __HAL_TIM_GET_COUNTER(&htim2);

    while ((__HAL_TIM_GET_COUNTER(&htim2) - start) < us);
}

void delay_ms(uint32_t ms)
{
    while(ms--) delay_us(1000);
}



/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  //HAL_Init();

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */
  SystemCoreClockUpdate();
  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USART2_UART_Init();
  MX_SubGHz_Phy_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */
  /* UART */
  Serial2_begin(115200);
  HAL_TIM_Base_Start(&htim2);

  /* Radio init */
  SUBGRF_Init(NULL);

  SUBGRF_SetStandby(STDBY_RC);

  SUBGRF_SetBufferBaseAddress(0x00, 0x00);

  // packet parameters
  pkt_params.PacketType = PACKET_TYPE_GFSK;
  pkt_params.Params.Gfsk.PayloadLength     = 160;
  pkt_params.Params.Gfsk.PreambleLength    = 2400;   // important, in bits
  pkt_params.Params.Gfsk.PreambleMinDetect = RADIO_PREAMBLE_DETECTOR_32_BITS; //RADIO_PREAMBLE_DETECTOR_OFF;

  pkt_params.Params.Gfsk.AddrComp   = RADIO_ADDRESSCOMP_FILT_OFF;
  pkt_params.Params.Gfsk.HeaderType = RADIO_PACKET_FIXED_LENGTH;//RADIO_PACKET_VARIABLE_LENGTH;
  pkt_params.Params.Gfsk.CrcLength  = RADIO_CRC_OFF;
  pkt_params.Params.Gfsk.DcFree     = RADIO_DC_FREE_OFF;

  pkt_params.Params.Gfsk.SyncWordLength = 0;

  //uint8_t syncWord[4] = {0x4A, 0xC4, 0xFE, 0xDA};
  //SUBGRF_SetSyncWord(syncWord);

  // modulation parameters
  mod_params.PacketType = PACKET_TYPE_GFSK;
  mod_params.Params.Gfsk.BitRate = 4800;
  mod_params.Params.Gfsk.Fdev = 1200;
  mod_params.Params.Gfsk.Bandwidth = RX_BW_23400 ;
  mod_params.Params.Gfsk.ModulationShaping = MOD_SHAPING_G_BT_05;

  SUBGRF_SetPacketParams(&pkt_params);
  SUBGRF_SetModulationParams(&mod_params);
  SUBGRF_SetRfFrequency(435313000);
  SUBGRF_SetPaConfig(0x04, 0x07, 0x00, 0x01);
  //SUBGRF_SetTxParams(RFO_HP,5,6);
  SUBGRF_SetTxParams(RFO_HP, 5, RADIO_RAMP_200_US);

  SUBGRF_SetDioIrqParams(
      IRQ_RX_DONE | IRQ_RX_TX_TIMEOUT,
      IRQ_RX_DONE | IRQ_RX_TX_TIMEOUT,
      IRQ_RADIO_NONE,
      IRQ_RADIO_NONE
  );

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */

  MX_SubGHz_Phy_Process();
  INITIALIZE_RX_PARAMETERS();
  SUBGRF_SetRxBoosted(0xFFFFFF);
  uint16_t irq_status;

  uint8_t rx_buffer[600];
  uint8_t size;
  uint8_t offset;

  HAL_GPIO_WritePin(GPIOC, GPIO_PIN_4, GPIO_PIN_RESET);

  //volatile uint8_t data_ready = 0;

  while (1)
  {
    /* USER CODE END WHILE */
    MX_SubGHz_Phy_Process();

    /* USER CODE BEGIN 3 */

    CAPTURE_PC_COMMAND();
    if( TRX_MODE == TRX_TX )
    {
	HAL_GPIO_WritePin(GPIOC, GPIO_PIN_5, GPIO_PIN_SET);

	SUBGRF_SetRfFrequency(435313000);
	/* Switch radio to TX packet size to 255 */
	pkt_params.Params.Gfsk.PayloadLength = 255;
	SUBGRF_SetPacketParams(&pkt_params);

        /* Configure TX IRQs */
        SUBGRF_SetDioIrqParams(
            IRQ_TX_DONE | IRQ_RX_TX_TIMEOUT,
            IRQ_TX_DONE | IRQ_RX_TX_TIMEOUT,
            IRQ_RADIO_NONE,
            IRQ_RADIO_NONE
        );

        AX25_txInitCfg();

        uint16_t G3RUH_Frame_Length = Build_G3RUH_TX_Frame( RAWTX_Data_Bffr, RAWTX_Data_Bffr_length, G3RUH_TXDataBuffer);

        SUBGRF_SetPayload(G3RUH_TXDataBuffer, G3RUH_Frame_Length);

        /* VERY IMPORTANT */
        SUBGRF_ClearIrqStatus(IRQ_RADIO_ALL);

        /* Start transmission */
        SUBGRF_SetTx(0);

        /* Wait until TX finished */
        uint16_t irqStatus;
        do
        {
            irqStatus = SUBGRF_GetIrqStatus();
        }
        while( (irqStatus & IRQ_TX_DONE) == 0 );

        /* Configure RX IRQs */
        SUBGRF_SetDioIrqParams(
            IRQ_RX_DONE | IRQ_RX_TX_TIMEOUT,
            IRQ_RX_DONE | IRQ_RX_TX_TIMEOUT,
            IRQ_RADIO_NONE,
            IRQ_RADIO_NONE
        );

        /* Switch radio to RX packet size to 160 */
        pkt_params.Params.Gfsk.PayloadLength = 160;
        SUBGRF_SetPacketParams(&pkt_params);

        /* Clear TX_DONE */
	SUBGRF_ClearIrqStatus(IRQ_TX_DONE);

	/* Switch to RX */
	SUBGRF_SetRxBoosted(0xFFFFFF);

        TRX_MODE = TRX_RX;

        SUBGRF_SetRfFrequency(437375000);
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_5, GPIO_PIN_RESET);
    }


    // Read and process pending radio interrupt flags
    irq_status = SUBGRF_GetIrqStatus();

    // Packet reception completed successfully
    if (irq_status & IRQ_RX_DONE)
    {
        // Clear RX_DONE interrupt flag
        SUBGRF_ClearIrqStatus(IRQ_RX_DONE);
        // Get received packet size and buffer offset
        SUBGRF_GetRxBufferStatus(&size, &offset);
        // Read received packet from radio FIFO buffer
        SUBGRF_ReadBuffer(offset, rx_buffer, 160);
        // Return radio to continuous receive mode
        SUBGRF_SetRxBoosted(0xFFFFFF);
        // Pass received packet to G3RUH decoder
        G3RUH_Process_Received_Bytes(rx_buffer, 160);

        INITIALIZE_RX_PARAMETERS();
    }

    // Reception timeout occurred (no valid packet received)
    if (irq_status & IRQ_RX_TX_TIMEOUT)
    {
        // Clear timeout interrupt flag
        SUBGRF_ClearIrqStatus(IRQ_RX_TX_TIMEOUT);
        // Restart receiver with infinite timeout
        SUBGRF_SetRxBoosted(0xFFFFFF);
    }
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS_PWR;
  RCC_OscInitStruct.HSEDiv = RCC_HSE_DIV1;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV2;
  RCC_OscInitStruct.PLL.PLLN = 6;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the SYSCLKSource, HCLK, PCLK1 and PCLK2 clocks dividers
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK3|RCC_CLOCKTYPE_HCLK
                              |RCC_CLOCKTYPE_SYSCLK|RCC_CLOCKTYPE_PCLK1
                              |RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.AHBCLK3Divider = RCC_SYSCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/**
  * @brief SUBGHZ Initialization Function
  * @param None
  * @retval None
  */
void MX_SUBGHZ_Init(void)
{

  /* USER CODE BEGIN SUBGHZ_Init 0 */

  /* USER CODE END SUBGHZ_Init 0 */

  /* USER CODE BEGIN SUBGHZ_Init 1 */

  /* USER CODE END SUBGHZ_Init 1 */
  hsubghz.Init.BaudratePrescaler = SUBGHZSPI_BAUDRATEPRESCALER_8;
  if (HAL_SUBGHZ_Init(&hsubghz) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN SUBGHZ_Init 2 */

  /* USER CODE END SUBGHZ_Init 2 */

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 47;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 4294967295;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief USART2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_USART2_UART_Init(void)
{

  /* USER CODE BEGIN USART2_Init 0 */

  /* USER CODE END USART2_Init 0 */

  /* USER CODE BEGIN USART2_Init 1 */

  /* USER CODE END USART2_Init 1 */
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 115200;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.Init.ClockPrescaler = UART_PRESCALER_DIV1;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetTxFifoThreshold(&huart2, UART_TXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_SetRxFifoThreshold(&huart2, UART_RXFIFO_THRESHOLD_1_8) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_UARTEx_DisableFifoMode(&huart2) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN USART2_Init 2 */

  /* USER CODE END USART2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, RFSW_Pin|RWSW_VE_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : RFSW_Pin RWSW_VE_Pin */
  GPIO_InitStruct.Pin = RFSW_Pin|RWSW_VE_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
