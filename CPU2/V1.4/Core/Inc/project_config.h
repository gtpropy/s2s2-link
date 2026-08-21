/*
 * project_config.h
 *
 *  Created on: Mar 9, 2026
 *      Author: reekn
 */

#ifndef INC_PROJECT_CONFIG_H_
#define INC_PROJECT_CONFIG_H_

#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>


//////////////////////////////////////////////////////////////////////////////////////////
// G3RUH.c funtions _____________________________________________________________________

#define TRX_TX 0xA1
#define TRX_RX 0xA2

void RECEIVE_GS_CMD();
void TRANSMIT_PACKETS();
void GFSK_DIRECT_MODE_RX(float frequency, float br, float fd, float rxbw);
void GFSK_DIRECT_MODE_TX(float frequency, float br, float fd, float rxbw, int8_t p);
void NEW_CW_TX_MODE(float frequency, float br, float fd,  int8_t p);
void TRANSMIT_CW_LETTER(uint8_t CWL);

void G3RUH_Process_Received_Bytes(uint8_t *rx_buffer,
                                      uint16_t size);

uint8_t Process_RX_Bit(uint8_t *RX_Bffr, uint8_t bit);

bool ReadKISSFrame(uint8_t *buffer, uint16_t *length);

void AX25_txInitCfg();
void INITIALIZE_RX_PARAMETERS();

//uint16_t CRC_CALC(uint8_t buffer[], uint16_t size_frame, uint16_t start);

//uint16_t CRC_CALC(uint8_t *buffer, uint16_t size_frame, uint16_t start);
uint16_t CRC_CALC(uint8_t *buffer, uint16_t size_frame, uint16_t start);
uint16_t Build_G3RUH_TX_Frame( uint8_t *input_data, uint16_t input_length, uint8_t *output_data);

void delay_us(uint32_t us);
void delay_ms(uint32_t ms);

void CAPTURE_PC_COMMAND();

#endif /* INC_PROJECT_CONFIG_H_ */
