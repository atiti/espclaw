#include "espclaw/board_profile.h"

const char *espclaw_storage_backend_name(espclaw_storage_backend_t backend)
{
    switch (backend) {
    case ESPCLAW_STORAGE_BACKEND_LITTLEFS:
        return "littlefs";
    case ESPCLAW_STORAGE_BACKEND_SD_CARD:
    default:
        return "sdcard";
    }
}

espclaw_board_profile_t espclaw_board_profile_for(espclaw_board_profile_id_t profile_id)
{
    switch (profile_id) {
    case ESPCLAW_BOARD_PROFILE_ESP32CAM:
        return (espclaw_board_profile_t){
            .profile_id = ESPCLAW_BOARD_PROFILE_ESP32CAM,
            .id = "esp32cam",
            .display_name = "ESP32-CAM",
            .provisioning = "softap",
            .default_storage_backend = ESPCLAW_STORAGE_BACKEND_SD_CARD,
            .cpu_cores = 2,
            .has_camera = true,
            .has_psram = true,
            .supports_ble_provisioning = false,
            .supports_concurrent_capture = false,
            .default_capture_width = 800,
            .default_capture_height = 600,
            .max_concurrent_sessions = 1,
            .ota_slot_count = 2,
        };
    case ESPCLAW_BOARD_PROFILE_ESP32C3:
        return (espclaw_board_profile_t){
            .profile_id = ESPCLAW_BOARD_PROFILE_ESP32C3,
            .id = "esp32c3",
            .display_name = "ESP32-C3",
            .provisioning = "ble",
            .default_storage_backend = ESPCLAW_STORAGE_BACKEND_LITTLEFS,
            .cpu_cores = 1,
            .has_camera = false,
            .has_psram = false,
            .supports_ble_provisioning = true,
            .supports_concurrent_capture = false,
            .default_capture_width = 0,
            .default_capture_height = 0,
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
            .default_storage_backend = ESPCLAW_STORAGE_BACKEND_SD_CARD,
            .cpu_cores = 2,
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
