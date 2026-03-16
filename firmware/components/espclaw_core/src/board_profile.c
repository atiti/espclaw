#include "espclaw/board_profile.h"

espclaw_board_profile_t espclaw_board_profile_for(espclaw_board_profile_id_t profile_id)
{
    switch (profile_id) {
    case ESPCLAW_BOARD_PROFILE_ESP32CAM:
        return (espclaw_board_profile_t){
            .profile_id = ESPCLAW_BOARD_PROFILE_ESP32CAM,
            .id = "esp32cam",
            .display_name = "ESP32-CAM",
            .provisioning = "softap",
            .has_camera = true,
            .has_psram = true,
            .supports_ble_provisioning = false,
            .supports_concurrent_capture = false,
            .default_capture_width = 800,
            .default_capture_height = 600,
            .max_concurrent_sessions = 1,
            .ota_slot_count = 2,
        };
    case ESPCLAW_BOARD_PROFILE_ESP32S3:
    default:
        return (espclaw_board_profile_t){
            .profile_id = ESPCLAW_BOARD_PROFILE_ESP32S3,
            .id = "esp32s3",
            .display_name = "ESP32-S3",
            .provisioning = "ble",
            .has_camera = true,
            .has_psram = true,
            .supports_ble_provisioning = true,
            .supports_concurrent_capture = true,
            .default_capture_width = 1280,
            .default_capture_height = 720,
            .max_concurrent_sessions = 2,
            .ota_slot_count = 2,
        };
    }
}
