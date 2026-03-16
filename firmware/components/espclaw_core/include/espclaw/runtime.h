#ifndef ESPCLAW_RUNTIME_H
#define ESPCLAW_RUNTIME_H

#include <stdbool.h>

#include "esp_err.h"

#include "espclaw/board_profile.h"
#include "espclaw/provisioning.h"

typedef struct {
    espclaw_board_profile_t profile;
    espclaw_storage_backend_t storage_backend;
    bool storage_ready;
    bool wifi_ready;
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

#endif
