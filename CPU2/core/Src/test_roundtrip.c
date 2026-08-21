#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "protocol.h"
#include "ax25.h"
#include "g3ruh.h"

static void hexdump(const char *label, const uint8_t *buf, uint16_t len) {
    printf("%s (%u bytes):\n", label, len);
    for (uint16_t i = 0; i < len; i++) {
        printf("%02X ", buf[i]);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    if (len % 16 != 0) printf("\n");
}

/* Simulate standard SDR receiver (Direwolf / gr-satellites / Soundmodem) */
static bool simulate_sdr_rx(const uint8_t *wire_bytes, uint16_t wire_len, const char *expected_payload, const char *expected_dest) {
    uint32_t sr = G3RUH_INITIAL_STATE;
    uint8_t last_bit = 1;
    uint8_t flag_shift = 0;
    bool in_frame = false;
    uint8_t frame_bits[AX25_MAX_FRAME_SIZE * 10];
    uint32_t frame_bit_count = 0;
    bool packet_passed = false;

    for (uint16_t byte_idx = 0; byte_idx < wire_len; byte_idx++) {
        uint8_t b = wire_bytes[byte_idx];
        /* SX126x transmits MSB first: bit 7, 6, 5, 4, 3, 2, 1, 0 */
        for (int bit_idx = 7; bit_idx >= 0; bit_idx--) {
            uint8_t wire_bit = (b >> bit_idx) & 1U;

            /* 1. G3RUH Descramble */
            uint8_t feedback = ((sr >> G3RUH_POLY_TAP1) ^ (sr >> G3RUH_POLY_TAP2)) & 1U;
            uint8_t descrambled = wire_bit ^ feedback;
            sr = ((sr << 1) | wire_bit) & G3RUH_REGISTER_MASK;

            /* 2. NRZ-I Decode */
            uint8_t data_bit = (descrambled == last_bit) ? 1U : 0U;
            last_bit = descrambled;

            /* 3. HDLC Flag Detector (0x7E = 01111110) */
            flag_shift = (uint8_t)((flag_shift >> 1) | (data_bit << 7));

            if (flag_shift == AX25_FLAG) {
                if (in_frame) {
                    if (frame_bit_count >= 7) {
                        uint32_t content_bits = frame_bit_count - 7;
                        uint8_t unstuffed[AX25_MAX_FRAME_SIZE] = {0};
                        uint32_t out_bit_count = 0;
                        uint8_t ones = 0;
                        bool violation = false;

                        for (uint32_t k = 0; k < content_bits; k++) {
                            uint8_t bit = frame_bits[k];
                            if (ones == 5) {
                                if (bit != 0) {
                                    violation = true;
                                    break;
                                }
                                ones = 0;
                                continue; /* Discard stuffed 0 */
                            }
                            if (bit == 1) {
                                ones++;
                                if ((out_bit_count / 8) < sizeof(unstuffed)) {
                                    unstuffed[out_bit_count / 8] |= (uint8_t)(1U << (out_bit_count % 8));
                                }
                            } else {
                                ones = 0;
                            }
                            out_bit_count++;
                        }

                        if (!violation && out_bit_count >= (18U * 8U) && (out_bit_count % 8U == 0)) {
                            uint16_t byte_len = (uint16_t)(out_bit_count / 8U);
                            uint16_t crc_check = AX25_CalculateCRC(unstuffed, byte_len);

                            if (crc_check == AX25_CRC_RESIDUE) {
                                char dest[7], src[7];
                                AX25_DecodeAddress(&unstuffed[0], dest);
                                AX25_DecodeAddress(&unstuffed[7], src);

                                uint16_t plen = (byte_len >= 18) ? (byte_len - 18) : 0;
                                char recovered[256] = {0};
                                if (plen > 0) {
                                    memcpy(recovered, &unstuffed[16], plen);
                                }

                                printf(" [SDR RX] Frame OK: %u bytes, Dest=%s, Src=%s, CRC=0x%04X (PASS)\n",
                                       byte_len, dest, src, crc_check);
                                printf(" [SDR RX] Payload: \"%s\"\n", recovered);

                                if (strcmp(dest, expected_dest) == 0) {
                                    if (expected_payload == NULL || memcmp(recovered, expected_payload, plen) == 0) {
                                        packet_passed = true;
                                    }
                                }
                            }
                        }
                    }
                    frame_bit_count = 0;
                } else {
                    in_frame = true;
                    frame_bit_count = 0;
                }
                continue;
            }

            if (in_frame) {
                if (frame_bit_count < sizeof(frame_bits)) {
                    frame_bits[frame_bit_count++] = data_bit;
                }
            }
        }
    }
    return packet_passed;
}

static int run_case(const char *name, const RadioConfig_t *txcfg, const RadioConfig_t *rxcfg,
                    const uint8_t *payload, uint16_t payload_len) {
    printf("\n==================== TEST: %s ====================\n", name);

    uint8_t wire_frame[RADIO_FIXED_PACKET_LEN];
    uint16_t flen = Protocol_CreatePacket(wire_frame, payload, payload_len, txcfg);
    if (flen == 0) {
        printf("FAIL: Protocol_CreatePacket returned 0\n");
        return 1;
    }

    printf("On-air buffer length = %u bytes\n", flen);
    hexdump("WIRE SCRAMBLED BYTES", wire_frame, flen);

    /* 1. Verify SDR Demodulation */
    bool sdr_ok = simulate_sdr_rx(wire_frame, flen, (const char*)payload, rxcfg->sourceCallsign);
    printf("SDR Demodulation / Decoding: %s\n", sdr_ok ? "PASS" : "FAIL");
    if (!sdr_ok) return 1;

    /* 2. Verify STM32WL55 Node Extraction */
    uint8_t extracted[AX25_MAX_FRAME_SIZE];
    uint16_t extracted_len = 0;
    bool node_ok = Protocol_ExtractFrame(wire_frame, flen, extracted, sizeof(extracted), &extracted_len);
    printf("STM32WL55 Protocol_ExtractFrame: %s (extracted %u bytes)\n", node_ok ? "PASS" : "FAIL", extracted_len);
    if (!node_ok) return 1;

    bool crc_ok = AX25_VerifyCRC_Method1(extracted, extracted_len);
    printf("AX25_VerifyCRC_Method1: %s\n", crc_ok ? "PASS" : "FAIL");
    if (!crc_ok) return 1;

    char dest[7], src[7];
    AX25_DecodeAddress(&extracted[1], dest);
    AX25_DecodeAddress(&extracted[8], src);
    printf("Extracted DEST=%s (SSID %u)  SRC=%s (SSID %u)\n", dest, AX25_DecodeSSID(&extracted[1]), src, AX25_DecodeSSID(&extracted[8]));

    if (strcmp(dest, rxcfg->sourceCallsign) != 0) {
        printf("FAIL: Dest mismatch (%s != %s)\n", dest, rxcfg->sourceCallsign);
        return 1;
    }

    uint16_t plen = extracted_len - 18 - 2;
    if (payload_len > 0) {
        if (memcmp(&extracted[17], payload, payload_len) != 0 || plen != payload_len) {
            printf("FAIL: Payload mismatch!\n");
            return 1;
        }
    }
    printf("RESULT: PASS\n");
    return 0;
}

int main(void) {
    RadioConfig_t SatelliteProfile = {
        .txFrequency = 435000000UL, .rxFrequency = 437375000UL,
        .sourceCallsign = "NEPSAT", .sourceSSID = 1,
        .destCallsign = "GROUND", .destSSID = 0,
        .isSatelliteMode = true
    };
    RadioConfig_t GroundProfile = {
        .txFrequency = 437375000UL, .rxFrequency = 435000000UL,
        .sourceCallsign = "GROUND", .sourceSSID = 0,
        .destCallsign = "NEPSAT", .destSSID = 1,
        .isSatelliteMode = false
    };

    int failures = 0;

    /* 1. Ground -> Satellite Command */
    uint8_t cmd_payload[1] = { (uint8_t)CMD_REQUEST_BURST };
    failures += run_case("GROUND->SAT command (opcode 0x01)", &GroundProfile, &SatelliteProfile,
                         cmd_payload, sizeof(cmd_payload));

    /* 2. Satellite -> Ground Beacon */
    const char *beacon = "NEPSAT BEACON";
    failures += run_case("SAT->GROUND beacon", &SatelliteProfile, &GroundProfile,
                         (const uint8_t*)beacon, (uint16_t)strlen(beacon));

    /* 3. Satellite -> Ground Burst Telemetry */
    const char *burst = "Namaste Everyone! From Antarikchya Nepal. S2S-2 CubeSat Beacon Communication Test";
    failures += run_case("SAT->GROUND burst packet (76 bytes payload)", &SatelliteProfile, &GroundProfile,
                         (const uint8_t*)burst, (uint16_t)strlen(burst));

    printf("\n\n===== FINAL TEST SUMMARY: %d failure(s) =====\n", failures);
    return failures;
}