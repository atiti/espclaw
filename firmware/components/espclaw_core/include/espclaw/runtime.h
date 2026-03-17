#ifndef ESPCLAW_RUNTIME_H
#define ESPCLAW_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "esp_err.h"
#else
#ifndef ESPCLAW_HOST_ESP_ERR_T_DEFINED
#define ESPCLAW_HOST_ESP_ERR_T_DEFINED 1
typedef int esp_err_t;
#endif
#endif

#include "espclaw/board_profile.h"
#include "espclaw/provisioning.h"

typedef struct {
    espclaw_board_profile_t profile;
    espclaw_storage_backend_t storage_backend;
    bool storage_ready;
    bool wifi_ready;
    bool wifi_boot_deferred;
    bool provisioning_active;
    bool telegram_ready;
    char workspace_root[128];
    char wifi_ssid[33];
    char onboarding_ssid[33];
    size_t storage_total_bytes;
    size_t storage_used_bytes;
} espclaw_runtime_status_t;

typedef struct {
    char ssid[33];
    int rssi;
    int channel;
    bool secure;
} espclaw_wifi_network_t;

esp_err_t espclaw_runtime_start(espclaw_board_profile_id_t profile_id, espclaw_runtime_status_t *status);
const espclaw_runtime_status_t *espclaw_runtime_status(void);
esp_err_t espclaw_runtime_wifi_scan(
    espclaw_wifi_network_t *networks,
    size_t max_networks,
    size_t *count_out
);
esp_err_t espclaw_runtime_wifi_join(
    const char *ssid,
    const char *password,
    char *message,
    size_t message_size
);
esp_err_t espclaw_runtime_get_provisioning_descriptor(espclaw_provisioning_descriptor_t *descriptor);
bool espclaw_runtime_time_is_sane(void);
bool espclaw_runtime_wait_for_time_sync(uint32_t timeout_ms);
esp_err_t espclaw_runtime_factory_reset(char *message, size_t message_size);
void espclaw_runtime_reboot(void);
#ifdef ESP_PLATFORM
bool espclaw_runtime_should_defer_wifi_boot(const espclaw_board_profile_t *profile, bool storage_ready);
bool espclaw_runtime_should_force_softap_only_boot(
    const espclaw_board_profile_t *profile,
    bool storage_ready,
    bool has_saved_wifi_credentials
);
#else
static inline bool espclaw_runtime_should_defer_wifi_boot(const espclaw_board_profile_t *profile, bool storage_ready)
{
    return profile != NULL &&
           profile->profile_id == ESPCLAW_BOARD_PROFILE_ESP32CAM &&
           !storage_ready;
}
static inline bool espclaw_runtime_should_force_softap_only_boot(
    const espclaw_board_profile_t *profile,
    bool storage_ready,
    bool has_saved_wifi_credentials
)
{
    return profile != NULL &&
           profile->profile_id == ESPCLAW_BOARD_PROFILE_ESP32CAM &&
           !storage_ready &&
           !has_saved_wifi_credentials;
}
#endif

#endif
