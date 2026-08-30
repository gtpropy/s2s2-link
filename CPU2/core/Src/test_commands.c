#include "s2s2_commands.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static void test_command(void) {
    cubus_command_t cmd = { .opcode = CMD_DOWNLOAD_FILE, .subsystem = SUBSYS_OBC,
                             .data_id = DATA_ID_SAT_LOG, .address = 0xDEADBEEFu, .num_packets = 300 };
    uint8_t buf[COMMAND_PAYLOAD_LEN];
    command_pack(&cmd, buf);

    cubus_command_t out;
    CHECK(command_unpack(buf, sizeof(buf), &out) == 0);
    CHECK(out.opcode == CMD_DOWNLOAD_FILE);
    CHECK(out.subsystem == SUBSYS_OBC);
    CHECK(out.data_id == DATA_ID_SAT_LOG);
    CHECK(out.address == 0xDEADBEEFu);
    CHECK(out.num_packets == 300);
    CHECK(command_unpack(buf, COMMAND_PAYLOAD_LEN - 1, &out) == -1);
    printf("test_command: %s\n", failures ? "FAIL" : "ok so far");
}

static void test_ack(void) {
    cubus_ack_t ack = { .status = ACK_STATUS_DATA_UNAVAILABLE, .data_id = DATA_ID_BEACON2, .acked_seq = 4321 };
    uint8_t buf[ACK_PAYLOAD_LEN];
    ack_pack(&ack, buf);

    cubus_ack_t out;
    CHECK(ack_unpack(buf, sizeof(buf), &out) == 0);
    CHECK(out.status == ACK_STATUS_DATA_UNAVAILABLE);
    CHECK(out.data_id == DATA_ID_BEACON2);
    CHECK(out.acked_seq == 4321);
    CHECK(strcmp(ack_status_str(out.status), "requested data unavailable") == 0);
}

static void test_beacon1(void) {
    beacon1_t b = { .type = 1, .tim_day = 200, .tim_hour = 14, .bat_v = 8200, .bat_i = 150,
                    .bat_temp = 25, .raw_v = 33, .raw_i = 10, .sol_tot_v = 4100, .sol_tot_i = 200,
                    .unreg_i = 12, .main_3v3_i = 40, .msn_3v3_i = 20, .v5_i = 30, .adc_3v32_i = 5,
                    .temp1 = 22, .temp2 = 23, .temp3 = 24, .temp4 = 21, .temp5 = 20,
                    .bpb_t = 19, .ant_p_t = 18, .obc_com_t = 26, .system_stat = 0x01,
                    .mcu_stat = 0x02, .adc_stat = 0x00 };
    uint8_t buf[BEACON1_PAYLOAD_LEN];
    beacon1_pack(&b, buf);

    beacon1_t out;
    CHECK(beacon1_unpack(buf, sizeof(buf), &out) == 0);
    CHECK(out.type == b.type && out.tim_day == b.tim_day && out.tim_hour == b.tim_hour &&
          out.bat_v == b.bat_v && out.bat_i == b.bat_i && out.bat_temp == b.bat_temp &&
          out.raw_v == b.raw_v && out.raw_i == b.raw_i && out.sol_tot_v == b.sol_tot_v &&
          out.sol_tot_i == b.sol_tot_i && out.unreg_i == b.unreg_i &&
          out.main_3v3_i == b.main_3v3_i && out.msn_3v3_i == b.msn_3v3_i &&
          out.v5_i == b.v5_i && out.adc_3v32_i == b.adc_3v32_i &&
          out.temp1 == b.temp1 && out.temp2 == b.temp2 && out.temp3 == b.temp3 &&
          out.temp4 == b.temp4 && out.temp5 == b.temp5 && out.bpb_t == b.bpb_t &&
          out.ant_p_t == b.ant_p_t && out.obc_com_t == b.obc_com_t &&
          out.system_stat == b.system_stat && out.mcu_stat == b.mcu_stat &&
          out.adc_stat == b.adc_stat);
    CHECK(beacon1_unpack(buf, BEACON1_PAYLOAD_LEN - 1, &out) == -1);
}

static void test_beacon2(void) {
    beacon2_t b = { .tim_day = 200, .sol_p1_v = 30, .sol_p2_v = 31, .sol_p3_v = 32, .sol_p4_v = 33,
                    .sol_p5_v = 34, .sol_p1_i = 5, .sol_p2_i = 6, .sol_p3_i = 7, .sol_p4_i = 8,
                    .sol_p5_i = 9, .gyro_x = 100, .gyro_y = 200, .gyro_z = 300, .mag_x = 400,
                    .mag_y = 500, .mag_z = 600, .power_stat = 0x01 };
    uint8_t buf[BEACON2_PAYLOAD_LEN];
    beacon2_pack(&b, buf);

    beacon2_t out;
    CHECK(beacon2_unpack(buf, sizeof(buf), &out) == 0);
    CHECK(out.tim_day == b.tim_day && out.sol_p1_v == b.sol_p1_v && out.sol_p2_v == b.sol_p2_v &&
          out.sol_p3_v == b.sol_p3_v && out.sol_p4_v == b.sol_p4_v && out.sol_p5_v == b.sol_p5_v &&
          out.sol_p1_i == b.sol_p1_i && out.sol_p2_i == b.sol_p2_i && out.sol_p3_i == b.sol_p3_i &&
          out.sol_p4_i == b.sol_p4_i && out.sol_p5_i == b.sol_p5_i && out.gyro_x == b.gyro_x &&
          out.gyro_y == b.gyro_y && out.gyro_z == b.gyro_z && out.mag_x == b.mag_x &&
          out.mag_y == b.mag_y && out.mag_z == b.mag_z && out.power_stat == b.power_stat);
}

static void test_camera_fragment(void) {
    uint8_t data[64];
    for (int i = 0; i < 64; i++) data[i] = (uint8_t)i;

    camera_fragment_t frag = { .image_id = 7, .fragment_index = 3, .total_fragments = 10,
                                .image_type = IMAGE_TYPE_NIR, .ai_score = 90, .data_len = 64, .data = data };
    uint8_t buf[CAMERA_FRAGMENT_HDR_LEN + 64];
    int packed_len = camera_fragment_pack(&frag, buf);
    CHECK(packed_len == CAMERA_FRAGMENT_HDR_LEN + 64);

    camera_fragment_unpacked_t out;
    CHECK(camera_fragment_unpack(buf, (uint16_t)packed_len, &out) == 0);
    CHECK(out.image_id == 7);
    CHECK(out.fragment_index == 3);
    CHECK(out.total_fragments == 10);
    CHECK(out.image_type == IMAGE_TYPE_NIR);
    CHECK(out.ai_score == 90);
    CHECK(out.data_len == 64);
    CHECK(memcmp(out.data, data, 64) == 0);

    /* zero-length fragment (last fragment can be empty/short) */
    camera_fragment_t frag2 = { .image_id = 1, .fragment_index = 9, .total_fragments = 10,
                                 .image_type = IMAGE_TYPE_RGB, .ai_score = 0, .data_len = 0, .data = NULL };
    uint8_t buf2[CAMERA_FRAGMENT_HDR_LEN];
    int len2 = camera_fragment_pack(&frag2, buf2);
    CHECK(len2 == CAMERA_FRAGMENT_HDR_LEN);
    camera_fragment_unpacked_t out2;
    CHECK(camera_fragment_unpack(buf2, (uint16_t)len2, &out2) == 0);
    CHECK(out2.data_len == 0);

    /* oversized reject */
    uint8_t big[CAMERA_FRAGMENT_MAX_DATA + 1] = {0};
    camera_fragment_t frag3 = { .image_id = 1, .fragment_index = 0, .total_fragments = 1,
                                 .image_type = IMAGE_TYPE_RGB, .ai_score = 0,
                                 .data_len = CAMERA_FRAGMENT_MAX_DATA + 1, .data = big };
    uint8_t bigbuf[CAMERA_FRAGMENT_HDR_LEN + CAMERA_FRAGMENT_MAX_DATA + 1];
    CHECK(camera_fragment_pack(&frag3, bigbuf) == -1);
}

int main(void) {
    test_command();
    test_ack();
    test_beacon1();
    test_beacon2();
    test_camera_fragment();

    if (failures == 0) {
        printf("ALL COMMAND/TELEMETRY PAYLOAD TESTS PASSED\n");
        return 0;
    }
    printf("%d CHECK(S) FAILED\n", failures);
    return 1;
}
