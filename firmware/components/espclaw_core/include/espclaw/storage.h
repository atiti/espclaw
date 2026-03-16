#ifndef ESPCLAW_STORAGE_H
#define ESPCLAW_STORAGE_H

#include <stddef.h>

#include "espclaw/board_profile.h"

typedef struct {
    espclaw_storage_backend_t backend;
    char workspace_root[128];
    size_t total_bytes;
    size_t used_bytes;
} espclaw_storage_mount_t;

const char *espclaw_storage_describe_workspace_root(const char *workspace_root);

#ifdef ESP_PLATFORM
#include "esp_err.h"

espclaw_storage_backend_t espclaw_storage_backend_for_profile(const espclaw_board_profile_t *profile);
esp_err_t espclaw_storage_mount_workspace(const espclaw_board_profile_t *profile, espclaw_storage_mount_t *mount);
#endif

#endif
