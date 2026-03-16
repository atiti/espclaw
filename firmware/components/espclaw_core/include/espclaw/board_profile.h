#ifndef ESPCLAW_BOARD_PROFILE_H
#define ESPCLAW_BOARD_PROFILE_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    ESPCLAW_BOARD_PROFILE_ESP32S3 = 0,
    ESPCLAW_BOARD_PROFILE_ESP32CAM = 1
} espclaw_board_profile_id_t;

typedef struct {
    espclaw_board_profile_id_t profile_id;
    const char *id;
    const char *display_name;
    const char *provisioning;
    bool has_camera;
    bool has_psram;
    bool supports_ble_provisioning;
    bool supports_concurrent_capture;
    size_t default_capture_width;
    size_t default_capture_height;
    size_t max_concurrent_sessions;
    size_t ota_slot_count;
} espclaw_board_profile_t;

espclaw_board_profile_t espclaw_board_profile_for(espclaw_board_profile_id_t profile_id);

#endif
