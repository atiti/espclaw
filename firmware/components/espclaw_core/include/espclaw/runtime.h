#ifndef ESPCLAW_RUNTIME_H
#define ESPCLAW_RUNTIME_H

#include <stdbool.h>

#include "esp_err.h"

#include "espclaw/board_profile.h"

typedef struct {
    espclaw_board_profile_t profile;
    bool storage_ready;
    bool wifi_ready;
    bool provisioning_active;
    bool telegram_ready;
    char workspace_root[128];
} espclaw_runtime_status_t;

esp_err_t espclaw_runtime_start(espclaw_board_profile_id_t profile_id, espclaw_runtime_status_t *status);
const espclaw_runtime_status_t *espclaw_runtime_status(void);

#endif
