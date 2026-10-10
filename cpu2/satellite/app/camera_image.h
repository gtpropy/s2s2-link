/* CubeSat Camera Image Configuration for M0+ (Data is streamed from M4 via IPC Ring Buffer) */
#ifndef __CAMERA_IMAGE_H
#define __CAMERA_IMAGE_H

#include <stdint.h>

#define DUMMY_CAM_RAW_IMAGE_SIZE    8774U
#define DUMMY_CAM_CHUNK_SIZE        128U
#define DUMMY_CAM_NUM_CHUNKS        69U
#define DUMMY_CAM_PADDED_IMAGE_SIZE 8832U

#define CAM_RAW_IMAGE_SIZE          DUMMY_CAM_RAW_IMAGE_SIZE
#define CAM_CHUNK_SIZE              DUMMY_CAM_CHUNK_SIZE
#define CAM_NUM_CHUNKS              DUMMY_CAM_NUM_CHUNKS
#ifndef CAM_TOTAL_CHUNKS
#define CAM_TOTAL_CHUNKS            DUMMY_CAM_NUM_CHUNKS
#endif
#define CAM_PADDED_IMAGE_SIZE       DUMMY_CAM_PADDED_IMAGE_SIZE

/* Minimal fallback header pattern for M0+ standalone testing so M0+ flash stays within 56KB */
static const uint8_t g_camera_dummy_header[16] = {
    0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, 0x4A, 0x46, 0x49, 0x46, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00
};

#endif /* __CAMERA_IMAGE_H */
