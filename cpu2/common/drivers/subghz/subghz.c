/**
  ******************************************************************************
  * @file    subghz.c
  * @brief   Sub-GHz radio SPI driver, written on LL / direct register access.
  *
  * Replaces stm32wlxx_hal_subghz.c. The HAL_SUBGHZ_* names and signatures that the
  * radio middleware (radio_driver.c) and the IRQ handlers use are kept, but every
  * command / register / buffer access now goes through one transaction routine, and
  * the HAL handle state machine, locking, callback tables and sticky error codes are
  * gone. Behaviour kept: RFBUSY wait before and after each transaction, NSS wake-up
  * while the radio sleeps, and a command arriving mid-transaction (radio IRQ vs main
  * loop) returns HAL_BUSY instead of interleaving SPI bytes.
  ******************************************************************************
  */

#include "subghz.h"

SUBGHZ_HandleTypeDef hsubghz;

#define SUBGHZ_TIMEOUT_MS     100U
#define SUBGHZ_LOOP_TIME      ((SystemCoreClock * 28U) >> 19U)   /* loops per ms, SPI polls */
#define SUBGHZ_RFBUSY_LOOP    ((SystemCoreClock * 24U) >> 20U)   /* loops per ms, RFBUSY    */
#define DS_KEEP               2U

static uint8_t s_ready;       /* HAL_SUBGHZ_Init() done                       */
static uint8_t s_busy;        /* transaction in flight                        */
static uint8_t s_deepsleep = 1U;

static HAL_StatusTypeDef wait_rfbusy(void)
{
    uint32_t n = SUBGHZ_TIMEOUT_MS * SUBGHZ_RFBUSY_LOOP;
    while (LL_PWR_IsActiveFlag_RFBUSYS() != 0UL)
    {
        if (n-- == 0U) return HAL_ERROR;
    }
    return HAL_OK;
}

/* One byte over SUBGHZSPI (full duplex): returns the byte clocked in. */
static uint8_t spi_xfer(uint8_t out, HAL_StatusTypeDef *st)
{
    volatile uint8_t *dr = (volatile uint8_t *)&SUBGHZSPI->DR;
    uint32_t n = SUBGHZ_TIMEOUT_MS * SUBGHZ_LOOP_TIME;
    while ((SUBGHZSPI->SR & SPI_SR_TXE) == 0U)
    {
        if (n-- == 0U) { *st = HAL_ERROR; break; }
    }
    *dr = out;
    n = SUBGHZ_TIMEOUT_MS * SUBGHZ_LOOP_TIME;
    while ((SUBGHZSPI->SR & SPI_SR_RXNE) == 0U)
    {
        if (n-- == 0U) { *st = HAL_ERROR; break; }
    }
    return *dr;
}

/*
 * The ONE SPI transaction routine every HAL_SUBGHZ_* call below goes through (the HAL had a separate
 * near-copy for each of command / register / buffer, read and write).
 *   hdr, hlen   opcode (+ address / offset / dummy) bytes sent first
 *   data, n     payload; written from data[] or, if is_read, clocked into data[]
 *   wait_after  wait for RFBUSY to drop again after NSS goes high (all but SetSleep: the radio is asleep)
 *   ds          new "radio may be in deep sleep" state, or DS_KEEP to leave it as it is
 * Returns HAL_BUSY (nothing sent) if another transaction is already running, e.g. the radio IRQ arrived
 * in the middle of a main-loop command: the caller retries instead of two commands interleaving on SPI.
 */
static HAL_StatusTypeDef txn(const uint8_t *hdr, uint8_t hlen, uint8_t *data, uint16_t n,
                             uint8_t is_read, uint8_t wait_after, uint8_t ds)
{
    HAL_StatusTypeDef st = HAL_OK;

    if (s_ready == 0U || s_busy != 0U) return HAL_BUSY;
    s_busy = 1U;

    /* device ready: wake the radio through NSS if it was left in sleep (pulse NSS, wait for RFBUSY to clear) */
    if (s_deepsleep != 0U) LL_PWR_SelectSUBGHZSPI_NSS();
    if (wait_rfbusy() != HAL_OK) st = HAL_ERROR;
    if (s_deepsleep != 0U) LL_PWR_UnselectSUBGHZSPI_NSS();
    if (ds != DS_KEEP) s_deepsleep = ds;   /* SetSleep / SetRxDutyCycle put the radio to sleep: wake it next time */

    LL_PWR_SelectSUBGHZSPI_NSS();
    for (uint8_t i = 0U; i < hlen; i++) (void)spi_xfer(hdr[i], &st);
    for (uint16_t i = 0U; i < n; i++)
    {
        if (is_read) data[i] = spi_xfer(0xFFU, &st);
        else         (void)spi_xfer(data[i], &st);
    }
    LL_PWR_UnselectSUBGHZSPI_NSS();

    if (wait_after && wait_rfbusy() != HAL_OK) st = HAL_ERROR;
    s_busy = 0U;
    return st;
}

/* Bring the radio out of reset and set up SUBGHZSPI (master, software NSS, 8-bit, prescaler /4). The reset
 * wait and the SPI register values are the HAL ones; the handle/lock/state-machine bookkeeping is dropped. */
HAL_StatusTypeDef HAL_SUBGHZ_Init(SUBGHZ_HandleTypeDef *h)
{
    HAL_StatusTypeDef status = HAL_OK;
    (void)h;

    if (s_ready == 0U)
    {
        __HAL_RCC_SUBGHZSPI_CLK_ENABLE();
        NVIC_SetPriority(SUBGHZ_Radio_IRQn, 0U);
        NVIC_EnableIRQ(SUBGHZ_Radio_IRQn);
        LL_C2_EXTI_EnableIT_32_63(LL_EXTI_LINE_44);

        LL_RCC_RF_DisableReset();
        uint32_t n = SUBGHZ_TIMEOUT_MS * SUBGHZ_LOOP_TIME;
        while (LL_RCC_IsRFUnderReset() != 0UL)
        {
            if (n-- == 0U) { status = HAL_ERROR; break; }
        }
        LL_PWR_UnselectSUBGHZSPI_NSS();
        LL_C2_PWR_SetRadioBusyTrigger(LL_PWR_RADIO_BUSY_TRIGGER_WU_IT);
    }
    LL_PWR_ClearFlag_RFBUSY();

    if (status == HAL_OK)
    {
        CLEAR_BIT(SUBGHZSPI->CR1, SPI_CR1_SPE);
        WRITE_REG(SUBGHZSPI->CR1, (SPI_CR1_MSTR | SPI_CR1_SSI | SUBGHZSPI_BAUDRATEPRESCALER_4 | SPI_CR1_SSM));
        WRITE_REG(SUBGHZSPI->CR2, (SPI_CR2_FRXTH | SPI_CR2_DS_0 | SPI_CR2_DS_1 | SPI_CR2_DS_2));
        SET_BIT(SUBGHZSPI->CR1, SPI_CR1_SPE);
        s_deepsleep = 1U;
    }
    s_ready = 1U;
    return status;
}

void MX_SUBGHZ_Init(void)
{
    if (HAL_SUBGHZ_Init(&hsubghz) != HAL_OK)
    {
        Error_Handler();
    }
}

/* Radio "set" command: opcode then params. Waits for RFBUSY afterwards, except SetSleep. */
HAL_StatusTypeDef HAL_SUBGHZ_ExecSetCmd(SUBGHZ_HandleTypeDef *h, SUBGHZ_RadioSetCmd_t cmd,
                                        uint8_t *buf, uint16_t size)
{
    uint8_t c = (uint8_t)cmd;
    (void)h;
    return txn(&c, 1U, buf, size, 0U, (cmd != RADIO_SET_SLEEP),
               (uint8_t)((cmd == RADIO_SET_SLEEP) || (cmd == RADIO_SET_RXDUTYCYCLE)));
}

/* Radio "get" command: opcode, one dummy byte (status), then size bytes read back. */
HAL_StatusTypeDef HAL_SUBGHZ_ExecGetCmd(SUBGHZ_HandleTypeDef *h, SUBGHZ_RadioGetCmd_t cmd,
                                        uint8_t *buf, uint16_t size)
{
    uint8_t hdr[2] = { (uint8_t)cmd, 0x00U };
    (void)h;
    return txn(hdr, 2U, buf, size, 1U, 1U, DS_KEEP);
}

/* Register write: opcode, 16-bit address, data. */
HAL_StatusTypeDef HAL_SUBGHZ_WriteRegisters(SUBGHZ_HandleTypeDef *h, uint16_t addr,
                                            uint8_t *buf, uint16_t size)
{
    uint8_t hdr[3] = { SUBGHZ_RADIO_WRITE_REGISTER, (uint8_t)(addr >> 8), (uint8_t)addr };
    (void)h;
    return txn(hdr, 3U, buf, size, 0U, 1U, DS_KEEP);
}

/* Register read: opcode, 16-bit address, one dummy byte, data back. */
HAL_StatusTypeDef HAL_SUBGHZ_ReadRegisters(SUBGHZ_HandleTypeDef *h, uint16_t addr,
                                           uint8_t *buf, uint16_t size)
{
    uint8_t hdr[4] = { SUBGHZ_RADIO_READ_REGISTER, (uint8_t)(addr >> 8), (uint8_t)addr, 0U };
    (void)h;
    return txn(hdr, 4U, buf, size, 1U, 1U, DS_KEEP);
}

/* Packet-buffer write (TX payload): opcode, offset, data. */
HAL_StatusTypeDef HAL_SUBGHZ_WriteBuffer(SUBGHZ_HandleTypeDef *h, uint8_t offset,
                                         uint8_t *buf, uint16_t size)
{
    uint8_t hdr[2] = { SUBGHZ_RADIO_WRITE_BUFFER, offset };
    (void)h;
    return txn(hdr, 2U, buf, size, 0U, 1U, DS_KEEP);
}

/* Packet-buffer read (RX payload): opcode, offset, one dummy byte, data back. */
HAL_StatusTypeDef HAL_SUBGHZ_ReadBuffer(SUBGHZ_HandleTypeDef *h, uint8_t offset,
                                        uint8_t *buf, uint16_t size)
{
    uint8_t hdr[3] = { SUBGHZ_RADIO_READ_BUFFER, offset, 0U };
    (void)h;
    return txn(hdr, 3U, buf, size, 1U, 1U, DS_KEEP);
}

/* Radio IRQ (EXTI 44): read + clear the status, then run the middleware callbacks. */
void HAL_SUBGHZ_IRQHandler(SUBGHZ_HandleTypeDef *h)
{
    uint8_t isr[2] = { 0U, 0U };

    (void)HAL_SUBGHZ_ExecGetCmd(h, RADIO_GET_IRQSTATUS, isr, 2U);
    uint16_t it = (uint16_t)(((uint16_t)isr[0] << 8U) | isr[1]);
    (void)HAL_SUBGHZ_ExecSetCmd(h, RADIO_CLR_IRQSTATUS, isr, 2U);

    if (it & SUBGHZ_IT_TX_CPLT)            HAL_SUBGHZ_TxCpltCallback(h);
    if (it & SUBGHZ_IT_RX_CPLT)            HAL_SUBGHZ_RxCpltCallback(h);
    if (it & SUBGHZ_IT_PREAMBLE_DETECTED)  HAL_SUBGHZ_PreambleDetectedCallback(h);
    if (it & SUBGHZ_IT_SYNCWORD_VALID)     HAL_SUBGHZ_SyncWordValidCallback(h);
    if (it & SUBGHZ_IT_HEADER_VALID)       HAL_SUBGHZ_HeaderValidCallback(h);
    if (it & SUBGHZ_IT_HEADER_ERROR)       HAL_SUBGHZ_HeaderErrorCallback(h);
    if (it & SUBGHZ_IT_CRC_ERROR)          HAL_SUBGHZ_CRCErrorCallback(h);
    if (it & SUBGHZ_IT_CAD_DONE)
    {
        HAL_SUBGHZ_CADStatusCallback(h, (it & SUBGHZ_IT_CAD_ACTIVITY_DETECTED)
                                            ? HAL_SUBGHZ_CAD_DETECTED : HAL_SUBGHZ_CAD_CLEAR);
    }
    if (it & SUBGHZ_IT_RX_TX_TIMEOUT)      HAL_SUBGHZ_RxTxTimeoutCallback(h);
    if (it & SUBGHZ_IT_LR_FHSS_HOP)        HAL_SUBGHZ_LrFhssHopCallback(h);
}
