#ifndef AX25_H
#define AX25_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>

/*==========================================================
  AX.25 Constants
==========================================================*/
#define AX25_FLAG                  0x7E
#define AX25_CONTROL_UI            0x03
#define AX25_PID_NO_LAYER3         0xF0
#define AX25_MAX_SSID              15
#define AX25_ADDRESS_LEN           7
#define AX25_ADDRESS_FIELDS        14
#define AX25_CONTROL_PID_LEN       2
#define AX25_FCS_LEN               2
#define AX25_FLAG_LEN              2

#define AX25_CRC_INIT              0xFFFF
#define AX25_CRC_POLY              0x8408
#define AX25_CRC_RESIDUE           0xF0B8
/*to increase the data size please define AX25_MAX_PAYLOAD_LEN before including this header */
#ifndef AX25_MAX_PAYLOAD_LEN
#define AX25_MAX_PAYLOAD_LEN       140
#endif

#define AX25_MAX_FRAME_SIZE \
    (AX25_FLAG_LEN + AX25_ADDRESS_FIELDS + AX25_CONTROL_PID_LEN + \
     AX25_FCS_LEN + AX25_MAX_PAYLOAD_LEN)

#define AX25_MAX_FRAME_LEN         AX25_MAX_FRAME_SIZE

typedef enum {
    AX25_OK = 0,
    AX25_ERROR_NULL_POINTER,
    AX25_ERROR_CALLSIGN,
    AX25_ERROR_SSID,
    AX25_ERROR_BUFFER,
    AX25_ERROR_PAYLOAD
} AX25_Status_t;

/* Existing functions */
AX25_Status_t AX25_EncodeAddress(uint8_t *address, const char *callsign, uint8_t ssid, bool last);
AX25_Status_t AX25_DecodeAddress(const uint8_t *address, char *callsign);
uint8_t AX25_DecodeSSID(const uint8_t *address);
bool AX25_DecodeIsLast(const uint8_t *address);

AX25_Status_t AX25_BuildFrame(
    uint8_t *frameBuffer, uint16_t frameBufferSize, uint16_t *frameLength,
    const char *destination, uint8_t destinationSSID,
    const char *source, uint8_t sourceSSID,
    const uint8_t *payload, uint16_t payloadLength);

bool AX25_VerifyFrame(const uint8_t *frame, uint16_t frameLength);
bool AX25_VerifyCRC_Method1(const uint8_t *frame, uint16_t frameLength);
bool AX25_VerifyCRC_Method2(const uint8_t *frame, uint16_t frameLength,
                            uint16_t *computedCRC, uint16_t *receivedLE, uint16_t *receivedBE);
uint16_t AX25_CalculateCRC(const uint8_t *data, uint16_t length);

static inline uint16_t AX25_MinFrameLength(void) { return 20; }

/* New for Option A */
uint16_t HDLC_BitStuff(const uint8_t *input, uint16_t input_len,
                       uint8_t *output, uint16_t output_max);

uint16_t HDLC_BitUnstuff(const uint8_t *input, uint16_t input_len,
                         uint8_t *output, uint16_t output_max);

#ifdef __cplusplus
}
#endif

#endif /* AX25_H */