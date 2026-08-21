/*
 * G3RUH.c
 *
 *  Created on: Mar 5, 2026
 *      Author: reekn
 */
#include "project_config.h"
#include "UART_F.h"
#include "app_subghz_phy.h"

//#include "G3RUH.h"
#include <stdbool.h>
#include <string.h>
#include <stdint.h>


// declaration of variables
uint16_t   TXbit_to_FModulation ;
uint8_t    PrevsBit             ;
uint8_t    TX_Mode              ;
uint32_t   Shift_Register       ;
uint16_t   Nof_bit1_Counter     ;
uint16_t   Flags_to_send        ;
uint16_t   Byte_Counter         ;
uint16_t   Bit_Counter          ;
uint16_t   Frame_Length         ;
uint8_t    Bitstuff_Status      ;
uint8_t    transmission         ;
uint8_t    reception            ;
uint8_t    RX_Mode              ;
uint32_t   Sync_Word            ;

uint16_t   TX_DELAY       =  150 ;   // Number of flags to be sent before the frame.
uint16_t   TX_TAIL        =  5   ;

uint32_t RX_BIT_COUNTER = 0   ;

// Rx Modes
#define RX_FREQ_SYNC_MODE    0x11  // Initial Frequency sync mode.
#define RX_FLAGS_MODE        0x12  // Ignoring sync flags.
#define RX_DATA_MODE         0x13  // Data decoder mode.
#define RX_DONE              0x14  //

// Tx Modes
#define TX_DELAY_FLAG        0x21   // Synchronization mode (sending flags).
#define TX_DATA_MODE         0x22   // Data transmission mode.
#define TX_TAIL_MODE         0x23   // Tail mode (send flags after the frame)
#define TX_DONE              0x24   //

#define MAX_TX_PCKT_LENGTH    300
#define MAX_RX_PCKT_LENGTH    255

extern uint8_t TRX_MODE;

uint8_t RXDATBuffer[MAX_RX_PCKT_LENGTH];

static const uint16_t crc16_x25_table[256] =
{
  0x0000,0x1189,0x2312,0x329B,0x4624,0x57AD,0x6536,0x74BF,
  0x8C48,0x9DC1,0xAF5A,0xBED3,0xCA6C,0xDBE5,0xE97E,0xF8F7,
  0x1081,0x0108,0x3393,0x221A,0x56A5,0x472C,0x75B7,0x643E,
  0x9CC9,0x8D40,0xBFDB,0xAE52,0xDAED,0xCB64,0xF9FF,0xE876,
  0x2102,0x308B,0x0210,0x1399,0x6726,0x76AF,0x4434,0x55BD,
  0xAD4A,0xBCC3,0x8E58,0x9FD1,0xEB6E,0xFAE7,0xC87C,0xD9F5,
  0x3183,0x200A,0x1291,0x0318,0x77A7,0x662E,0x54B5,0x453C,
  0xBDCB,0xAC42,0x9ED9,0x8F50,0xFBEF,0xEA66,0xD8FD,0xC974,
  0x4204,0x538D,0x6116,0x709F,0x0420,0x15A9,0x2732,0x36BB,
  0xCE4C,0xDFC5,0xED5E,0xFCD7,0x8868,0x99E1,0xAB7A,0xBAF3,
  0x5285,0x430C,0x7197,0x601E,0x14A1,0x0528,0x37B3,0x263A,
  0xDECD,0xCF44,0xFDDF,0xEC56,0x98E9,0x8960,0xBBFB,0xAA72,
  0x6306,0x728F,0x4014,0x519D,0x2522,0x34AB,0x0630,0x17B9,
  0xEF4E,0xFEC7,0xCC5C,0xDDD5,0xA96A,0xB8E3,0x8A78,0x9BF1,
  0x7387,0x620E,0x5095,0x411C,0x35A3,0x242A,0x16B1,0x0738,
  0xFFCF,0xEE46,0xDCDD,0xCD54,0xB9EB,0xA862,0x9AF9,0x8B70,
  0x8408,0x9581,0xA71A,0xB693,0xC22C,0xD3A5,0xE13E,0xF0B7,
  0x0840,0x19C9,0x2B52,0x3ADB,0x4E64,0x5FED,0x6D76,0x7CFF,
  0x9489,0x8500,0xB79B,0xA612,0xD2AD,0xC324,0xF1BF,0xE036,
  0x18C1,0x0948,0x3BD3,0x2A5A,0x5EE5,0x4F6C,0x7DF7,0x6C7E,
  0xA50A,0xB483,0x8618,0x9791,0xE32E,0xF2A7,0xC03C,0xD1B5,
  0x2942,0x38CB,0x0A50,0x1BD9,0x6F66,0x7EEF,0x4C74,0x5DFD,
  0xB58B,0xA402,0x9699,0x8710,0xF3AF,0xE226,0xD0BD,0xC134,
  0x39C3,0x284A,0x1AD1,0x0B58,0x7FE7,0x6E6E,0x5CF5,0x4D7C,
  0xC60C,0xD785,0xE51E,0xF497,0x8028,0x91A1,0xA33A,0xB2B3,
  0x4A44,0x5BCD,0x6956,0x78DF,0x0C60,0x1DE9,0x2F72,0x3EFB,
  0xD68D,0xC704,0xF59F,0xE416,0x90A9,0x8120,0xB3BB,0xA232,
  0x5AC5,0x4B4C,0x79D7,0x685E,0x1CE1,0x0D68,0x3FF3,0x2E7A,
  0xE70E,0xF687,0xC41C,0xD595,0xA12A,0xB0A3,0x8238,0x93B1,
  0x6B46,0x7ACF,0x4854,0x59DD,0x2D62,0x3CEB,0x0E70,0x1FF9,
  0xF78F,0xE606,0xD49D,0xC514,0xB1AB,0xA022,0x92B9,0x8330,
  0x7BC7,0x6A4E,0x58D5,0x495C,0x3DE3,0x2C6A,0x1EF1,0x0F78
};


void AX25_txInitCfg()
{
  TX_Mode              = TX_DELAY_FLAG ;
  Flags_to_send        = TX_DELAY      ;
  TXbit_to_FModulation = 0             ;
  PrevsBit             = 0             ;
  Nof_bit1_Counter     = 0             ;
  Byte_Counter         = 0             ;
  Bit_Counter          = 0             ;
  Shift_Register       = 0x9999        ;
}

uint8_t NRZI_encoding(uint8_t bit)
{
  int encoded_bit;
  if(bit == 1)
  {
    encoded_bit = PrevsBit;
    Nof_bit1_Counter = Nof_bit1_Counter + 1;
  }
  else
  {
    encoded_bit = !PrevsBit;
    Nof_bit1_Counter = 0;
  }
  PrevsBit = encoded_bit;
  return     encoded_bit;
}


uint8_t CHECK_BIT_STUFFING()
{
  if (Nof_bit1_Counter == 5)  // If we have 5 consecutive 1s -> stuff a 0.
  {
    // Perform bitwise XOR on bit 17 (Shift_Register >> 16) and bit 12 (Shift_Register >> 11)
    uint8_t firstXorScrambler = ((Shift_Register >> 11) & 0x01) ^ ((Shift_Register >> 16) & 0x01);

    // Add a zero bit and perform NRZI encoding
    TXbit_to_FModulation = NRZI_encoding(0) ^ firstXorScrambler;

    // Shift the Shift_Register left by 1
    Shift_Register <<= 1;

    // Set or clear the least significant bit of Shift_Register based on TXbit_to_FModulation
    Shift_Register |= (TXbit_to_FModulation ? 0x01 : 0x00);

    // Reset the counter after bit stuffing
    Nof_bit1_Counter = 0;

    return 1;  // Bit stuffing was performed
  }
  return 0;  // No bit stuffing was necessary
}

void process_txBit(uint8_t data_bit)
{
  // Perform XOR on bit 17 (Shift_Register >> 16) and bit 12 (Shift_Register >> 11)
  uint8_t firstXorScrambler = ((Shift_Register >> 11) & 0x01) ^ ((Shift_Register >> 16) & 0x01);

  // Add data bit and perform NRZI encoding
  TXbit_to_FModulation = NRZI_encoding(data_bit) ^ firstXorScrambler;

  // Shift the Shift_Register left by 1
  Shift_Register <<= 1;

  // Set or clear the least significant bit of Shift_Register
  Shift_Register |= (TXbit_to_FModulation ? 0x01 : 0x00);
}


uint8_t Bit_Stuffing_Check_Before_Tail = 0;

uint8_t Prepare_NextBit_To_Send(uint8_t *buffer)
{
  uint8_t currentBit;  // Variable to store the current bit to be processed

  // Check for TX_DELAY_FLAG mode
  if (TX_Mode == TX_DELAY_FLAG)
  {
    // Get the bit from the flag pattern 0x7E using bitwise operations
    currentBit = (0x7E >> Bit_Counter) & 0x01;  // Shift right to get the required bit
    process_txBit(currentBit);  // Encode the current bit
    Bit_Counter++;              // Move to the next bit

    // Check if we have sent all bits for the flags (8 bits)
    if (Bit_Counter > 7)
    {
      Bit_Counter = 0;  // Reset Bit_Counter for the next byte
      Flags_to_send--;  // Decrement the number of flags to send

      // If all flags have been sent, switch to TX_DATA_MODE
      if (Flags_to_send == 0)
      {
        TX_Mode = TX_DATA_MODE;  // Switch mode to TX_DATA_MODE
        return 1;  // Indicate that a bit has been sent
      }
    }
    return 1;  // Indicate that a bit has been sent
  }

  // Check for TX_DATA_MODE
  else if (TX_Mode == TX_DATA_MODE)
  {
    // Check for bit stuffing; if stuffing is needed, return early
    if (CHECK_BIT_STUFFING()) return 1;

    // Get the next data bit from the buffer using bitwise operations
    currentBit = (buffer[Byte_Counter] >> Bit_Counter) & 0x01;  // Fetch the bit from the buffer
    process_txBit(currentBit);  // Encode the current data bit
    Bit_Counter++;              // Move to the next bit

    // Check if we have sent all bits for the current byte (8 bits)
    if (Bit_Counter > 7)
    {
      Bit_Counter = 0;  // Reset Bit_Counter for the next byte

      // Check if all data bytes have been sent
      if (Byte_Counter == Frame_Length-1 )
      {
        TX_Mode = TX_TAIL_MODE;  // Switch mode to TX_TAIL_MODE
        Flags_to_send = TX_TAIL;  // Set flags to send for tail mode
        Bit_Stuffing_Check_Before_Tail = 1;
        return 1;  // Indicate that a bit has been sent
      }
      Byte_Counter++;  // Move to the next byte in the buffer
    }
    return 1;  // Indicate that a bit has been sent
  }

  // Check for TX_TAIL_MODE
  else if (TX_Mode == TX_TAIL_MODE)
  {
    if(Bit_Stuffing_Check_Before_Tail == 1)
    {
      Bit_Stuffing_Check_Before_Tail = 0;
      if (CHECK_BIT_STUFFING()) return 1;
    }

    // Get the bit from the flag pattern 0x7E for tail mode
    currentBit = (0x7E >> Bit_Counter) & 0x01;  // Shift right to get the required bit
    process_txBit(currentBit);  // Encode the current tail bit
    Bit_Counter++;  // Move to the next bit

    // Check if we have sent all bits for the tail (8 bits)
    if (Bit_Counter > 7)
    {
      Bit_Counter = 0;  // Reset Bit_Counter for the next byte
      Flags_to_send--;  // Decrement the number of tail flags to send

      // Check if all tail flags have been sent
      if (Flags_to_send == 0)
      {
        TX_Mode = TX_DONE;  // Switch mode to TX_OFF
        return 0;  // Indicate that transmission is complete
      }
    }
    return 1;  // Indicate that a bit has been sent
  }
  return 0;  // In case none of the modes match, return 0
}

uint16_t CRC_CALC(uint8_t *buffer, uint16_t size_frame, uint16_t start)
{
    uint16_t crc = 0xFFFF;

    for(uint16_t i = start; i < size_frame; i++)
    {
        crc = (crc >> 8) ^ crc16_x25_table[(crc ^ buffer[i]) & 0xFF];
    }

    return crc ^ 0xFFFF;
}



uint16_t Build_G3RUH_TX_Frame( uint8_t *input_data, uint16_t input_length, uint8_t *output_data)
{
    uint16_t CRC_Value;

    /* Add CRC to input buffer */
    CRC_Value = CRC_CALC(input_data, input_length, 0);

    input_data[input_length]     = CRC_Value & 0xFF;
    input_data[input_length + 1] = (CRC_Value >> 8) & 0xFF;

    Frame_Length = input_length + 2;

    uint8_t txByte = 0;
    uint8_t txBitPos = 0;
    uint16_t txIndex = 0;

    do
    {
        transmission = Prepare_NextBit_To_Send(input_data);

        /* MSB-first packing */
        txByte |= (TXbit_to_FModulation << (7 - txBitPos));
        txBitPos++;

        if(txBitPos == 8)
        {
            output_data[txIndex++] = txByte;

            txByte = 0;
            txBitPos = 0;

            if(txIndex >= 255)
                break;
        }

    } while(transmission);

    /* Flush remaining bits */

    if(txBitPos != 0)
    {
        output_data[txIndex++] = txByte;
    }

    return txIndex;
}

//////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////
//////////////////////////////////////////////////////////////////////////////////////

//void INITIALIZE_RX_PARAMETERS()
//{
//  RX_Mode          = RX_FREQ_SYNC_MODE ;
//  Nof_bit1_Counter = 0                 ;
//  PrevsBit         = 1                 ;
//  Byte_Counter     = 1                 ;
//  Bit_Counter      = 0                 ;
//  Bitstuff_Status  = 0                 ;
//}

uint16_t CRCV = 0xFFFF;
void INITIALIZE_RX_PARAMETERS()
{
  RX_Mode          = RX_FREQ_SYNC_MODE;
  Nof_bit1_Counter = 0;
  PrevsBit         = 1;
  Byte_Counter     = 0;
  Bit_Counter      = 0;

  Sync_Word        = 0;
  RX_BIT_COUNTER   = 0;
  Shift_Register   = 0x1FFFF;

  CRCV = 0xFFFF;
}



uint32_t MLC            = 0   ;
int32_t  AFC_BITS       = 96  ;
volatile uint8_t dec    = 0   ;




void Update_CRC(uint8_t data)
{
    CRCV = (CRCV >> 8) ^ crc16_x25_table[(CRCV ^ data) & 0xFF];
}

uint8_t Process_RX_Bit(uint8_t *RX_Bffr, uint8_t bit)
{
  // Descramble the received bit using XOR on specific shift register bits.
  // Shift_Register bits 11 and 16 are used for descrambling.
  uint8_t RcvdBit = bit ^ ((Shift_Register >> 11) & 0x01) ^ ((Shift_Register >> 16) & 0x01);
  // Shift the Shift_Register left by 1 and add the current bit at the LSB.
  Shift_Register = (Shift_Register << 1) | (bit & 0x01);
  // Perform NRZI decoding. If the received bit is the same as the previous one,
  // decoded bit is 1; otherwise, it is 0.
  uint8_t rxbit = (RcvdBit == PrevsBit) ? 1 : 0;
  // Update the previous received bit for the next iteration.
  PrevsBit = RcvdBit;

  switch (RX_Mode)
  {
      case RX_FREQ_SYNC_MODE:

	  Sync_Word = (Sync_Word >> 1) | ( (uint32_t)rxbit << 31 ); // Shift register + insert new bit

	  // Check for synchronization flag
	  if (Sync_Word == 0x7E7E7E7E)
	  {
	      RX_Mode = RX_FLAGS_MODE;
	      Bit_Counter = 0;
	      Nof_bit1_Counter = 0;
	      CRCV = 0xFFFF;
	      Byte_Counter = 0;
	      return 1;
	  }
	  return 1;
      //______________________________________________________________________

      case RX_FLAGS_MODE:

	  if (Bit_Counter == 0) RX_Bffr[Byte_Counter] = 0;
	  // Shift buffer byte to the right and put the bit inside 7th possition.
	  RX_Bffr[Byte_Counter] = (RX_Bffr[Byte_Counter] >> 1) | (rxbit << 7);

	  // Increment bit counter and check if we have a full byte.
	  Bit_Counter++;
	  if (Bit_Counter > 7)
	  {
	    Bit_Counter = 0;

	    // If the byte is not a flag (0x7E), move to data mode.
	    if (RX_Bffr[Byte_Counter] != 0x7E)
	    {
		RX_Mode = RX_DATA_MODE;
		Update_CRC( RX_Bffr[Byte_Counter] );
		Byte_Counter++;
	    }
	  }
	  return 1;
      //_____________________________________________________________________

      case RX_DATA_MODE:

	  // Abort detection (11111111)
//	  if (Nof_bit1_Counter >= 7)
//	  {
//	      INITIALIZE_RX_PARAMETERS();
//	      return 0;
//	  }

	  if (Nof_bit1_Counter < 5)
	  {
	    if (Bit_Counter == 0) RX_Bffr[Byte_Counter] = 0;
	    // Shift buffer byte to the right and put the bit inside 7th possition.
	    RX_Bffr[Byte_Counter] = (RX_Bffr[Byte_Counter] >> 1) | (rxbit << 7);

	    // Increment bit counter and check if we have a full byte.
	    Bit_Counter++;
	    if (Bit_Counter > 7)
	    {
              Update_CRC( RX_Bffr[Byte_Counter]);   // <-- incremental CRC
	      Bit_Counter = 0;
	      Byte_Counter++;
	      if (Byte_Counter >= MAX_RX_PCKT_LENGTH)
	      {
		  RX_Mode = RX_DONE;
		  return 0;
	      }
	    }

	    if (rxbit == 1) Nof_bit1_Counter++   ; // Increment the count of consecutive 1s.
	    else            Nof_bit1_Counter = 0 ; // Reset consecutive 1s counter.
	    return 1;
	  }
	  else // Handling bit stuffing
	  {
	    if (rxbit == 0)
	    {
		Nof_bit1_Counter = 0; // Reset consecutive 1s counter.
		return 1;             // Ignore the stuffed bit.
	    }
	    else // If it's a flag, transition to RX_OFF mode.
	    {
		RX_Bffr[Byte_Counter] = 0x7E; // Set byte to flag (0x7E).
		RX_Mode = RX_DONE;            // End of frame.
		return 0;
	    }
	  }
	  return 0;
      //____________________________________________________________________________

      default:
	  return 0;
  }
}



void G3RUH_Process_Received_Bytes(uint8_t *rx_buffer, uint16_t size)
{
    for (uint16_t i = 0; i < size; i++)
    {
        for (int b = 7; b >= 0; b--)
        {
            uint8_t bit = (rx_buffer[i] >> b) & 0x01;

            if (Process_RX_Bit(RXDATBuffer, bit) == 0)
            {
        	if (CRCV == 0xF0B8)
        	{
		    /* Frame completed */
        	    Serial2_write(0xC0);
        	    Serial2_write(0x00);

        	    for(uint16_t i = 0; i < Byte_Counter-2; i++)
        	    {
        	        switch(RXDATBuffer[i])
        	        {
        	            case 0xC0:
        	                Serial2_write(0xDB);
        	                Serial2_write(0xDC);
        	                break;

        	            case 0xDB:
        	                Serial2_write(0xDB);
        	                Serial2_write(0xDD);
        	                break;

        	            default:
        	                Serial2_write(RXDATBuffer[i]);
        	                break;
        	        }
        	    }

        	    Serial2_write(0xC0);

		    //for(int i = 0; i<160; i++) rx_buffer[i] = 0;
        	}
                return;
            }
        }
    }
}

uint8_t KISS_Frame[200];
int16_t RAWTX_Data_Bffr_length = 0;
uint8_t RAWTX_Data_Bffr[120];
extern uint8_t TRX_MODE;

void CAPTURE_PC_COMMAND()
{
  if (Serial2_available())
  {
    delay_ms(100); // Wait for all data to arrive

    int count = 0;
    while (Serial2_available() && count < sizeof(KISS_Frame))
    {
	KISS_Frame[count++] = Serial2_read();
    }
    RAWTX_Data_Bffr_length = 0;

    // Check if it's a KISS frame with data frame type 0x00
    if (count > 2 && KISS_Frame[0] == 0xC0 && KISS_Frame[1] == 0x00)
    {
      for (int i = 2; i < count; i++)
      {
        if (KISS_Frame[i] == 0xDB) // Escape sequence
        {
          if (i + 1 < count)
          {
                 if (KISS_Frame[i + 1] == 0xDC) RAWTX_Data_Bffr[RAWTX_Data_Bffr_length] = 0xC0;
            else if (KISS_Frame[i + 1] == 0xDD) RAWTX_Data_Bffr[RAWTX_Data_Bffr_length] = 0xDB;
            RAWTX_Data_Bffr_length++;
            i++; // Skip next byte
          }
        }
        else if (KISS_Frame[i] == 0xC0) // End of frame
        {
            TRX_MODE = TRX_TX;
            //for(int i = 0; i<RAWTX_Data_Bffr_length; i++) Serial2_print("%X ", RAWTX_Data_Bffr[i]);
	    break;
        }
        else
        {
            RAWTX_Data_Bffr[RAWTX_Data_Bffr_length++] = KISS_Frame[i];
        }
      }
    }
  }
}
