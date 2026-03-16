#ifndef ESPCLAW_ADMIN_API_H
#define ESPCLAW_ADMIN_API_H

#include <stdbool.h>
#include <stddef.h>

#include "espclaw/auth_store.h"
#include "espclaw/board_config.h"
#include "espclaw/board_profile.h"
#include "espclaw/ota_state.h"

size_t espclaw_render_admin_status_json(
    const espclaw_board_profile_t *profile,
    espclaw_storage_backend_t storage_backend,
    const char *provider_id,
    const char *channel_id,
    bool workspace_ready,
    const espclaw_ota_state_t *ota_state,
    char *buffer,
    size_t buffer_size
);

size_t espclaw_render_workspace_files_json(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_apps_json(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_tools_json(
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_app_detail_json(
    const char *workspace_root,
    const char *app_id,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_auth_profile_json(
    const espclaw_auth_profile_t *profile,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_board_json(
    const espclaw_board_descriptor_t *board,
    char *buffer,
    size_t buffer_size
);
size_t espclaw_render_session_transcript_json(
    const char *workspace_root,
    const char *session_id,
    char *buffer,
    size_t buffer_size
);

#endif
