#include "espclaw/admin_api.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "espclaw/app_runtime.h"
#include "espclaw/auth_store.h"
#include "espclaw/session_store.h"
#include "espclaw/tool_catalog.h"
#include "espclaw/workspace.h"

static const char *ota_status_label(espclaw_ota_status_t status)
{
    switch (status) {
    case ESPCLAW_OTA_STATUS_IDLE:
        return "idle";
    case ESPCLAW_OTA_STATUS_DOWNLOADED:
        return "downloaded";
    case ESPCLAW_OTA_STATUS_PENDING_REBOOT:
        return "pending_reboot";
    case ESPCLAW_OTA_STATUS_VERIFYING:
        return "verifying";
    case ESPCLAW_OTA_STATUS_CONFIRMED:
        return "confirmed";
    case ESPCLAW_OTA_STATUS_ROLLBACK_REQUIRED:
        return "rollback_required";
    default:
        return "unknown";
    }
}

static size_t append_json_chunk(char *buffer, size_t buffer_size, size_t used, const char *fmt, ...)
{
    int written;
    va_list args;

    if (buffer == NULL || buffer_size == 0 || used >= buffer_size) {
        return used;
    }

    va_start(args, fmt);
    written = vsnprintf(buffer + used, buffer_size - used, fmt, args);
    va_end(args);

    if (written < 0) {
        return used;
    }

    if ((size_t)written >= buffer_size - used) {
        return buffer_size - 1;
    }

    return used + (size_t)written;
}

static size_t append_json_escaped_string(
    char *buffer,
    size_t buffer_size,
    size_t used,
    const char *value
)
{
    const char *cursor = value != NULL ? value : "";

    used = append_json_chunk(buffer, buffer_size, used, "\"");
    while (*cursor != '\0' && used + 2 < buffer_size) {
        char c = *cursor++;

        switch (c) {
        case '\\':
        case '"':
            used = append_json_chunk(buffer, buffer_size, used, "\\%c", c);
            break;
        case '\n':
            used = append_json_chunk(buffer, buffer_size, used, "\\n");
            break;
        case '\r':
            used = append_json_chunk(buffer, buffer_size, used, "\\r");
            break;
        case '\t':
            used = append_json_chunk(buffer, buffer_size, used, "\\t");
            break;
        default:
            used = append_json_chunk(buffer, buffer_size, used, "%c", c);
            break;
        }
    }
    used = append_json_chunk(buffer, buffer_size, used, "\"");
    return used;
}

static size_t append_string_array_json(
    char *buffer,
    size_t buffer_size,
    size_t used,
    char items[][ESPCLAW_APP_PERMISSION_NAME_MAX + 1],
    size_t count
)
{
    size_t index;

    used = append_json_chunk(buffer, buffer_size, used, "[");
    for (index = 0; index < count; ++index) {
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_escaped_string(buffer, buffer_size, used, items[index]);
    }
    used = append_json_chunk(buffer, buffer_size, used, "]");
    return used;
}

static size_t append_trigger_array_json(
    char *buffer,
    size_t buffer_size,
    size_t used,
    char items[][ESPCLAW_APP_TRIGGER_NAME_MAX + 1],
    size_t count
)
{
    size_t index;

    used = append_json_chunk(buffer, buffer_size, used, "[");
    for (index = 0; index < count; ++index) {
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_escaped_string(buffer, buffer_size, used, items[index]);
    }
    used = append_json_chunk(buffer, buffer_size, used, "]");
    return used;
}

static const char *tool_safety_label(espclaw_tool_safety_t safety)
{
    switch (safety) {
    case ESPCLAW_TOOL_SAFETY_CONFIRM_REQUIRED:
        return "confirm_required";
    case ESPCLAW_TOOL_SAFETY_READ_ONLY:
    default:
        return "read_only";
    }
}

size_t espclaw_render_admin_status_json(
    const espclaw_board_profile_t *profile,
    const char *provider_id,
    const char *channel_id,
    bool workspace_ready,
    const espclaw_ota_state_t *ota_state,
    char *buffer,
    size_t buffer_size
)
{
    const char *provider = provider_id != NULL ? provider_id : "";
    const char *channel = channel_id != NULL ? channel_id : "";
    const char *profile_id = "";
    const char *provisioning = "";
    const char *ota_status = "unknown";
    bool rollback_allowed = false;
    unsigned int target_slot = 0;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    if (profile != NULL) {
        profile_id = profile->id;
        provisioning = profile->provisioning;
    }

    if (ota_state != NULL) {
        ota_status = ota_status_label(ota_state->status);
        rollback_allowed = ota_state->rollback_allowed;
        target_slot = ota_state->target_slot;
    }

    return (size_t)snprintf(
        buffer,
        buffer_size,
        "{"
        "\"board_profile\":\"%s\","
        "\"provisioning\":\"%s\","
        "\"workspace_ready\":%s,"
        "\"provider\":\"%s\","
        "\"channel\":\"%s\","
        "\"ota\":{"
        "\"status\":\"%s\","
        "\"rollback_allowed\":%s,"
        "\"target_slot\":%u"
        "}"
        "}",
        profile_id,
        provisioning,
        workspace_ready ? "true" : "false",
        provider,
        channel,
        ota_status,
        rollback_allowed ? "true" : "false",
        target_slot
    );
}

size_t espclaw_render_workspace_files_json(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used = append_json_chunk(buffer, buffer_size, used, "{\"files\":[");

    for (index = 0; index < espclaw_workspace_file_count(); ++index) {
        char absolute_path[512];
        struct stat file_stat;
        const espclaw_workspace_file_t *file = espclaw_workspace_file_at(index);
        bool exists = false;

        if (file == NULL) {
            continue;
        }

        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }

        if (workspace_root != NULL &&
            espclaw_workspace_resolve_path(workspace_root, file->relative_path, absolute_path, sizeof(absolute_path)) == 0 &&
            stat(absolute_path, &file_stat) == 0) {
            exists = true;
        }

        used = append_json_chunk(
            buffer,
            buffer_size,
            used,
            "{\"path\":\"%s\",\"exists\":%s}",
            file->relative_path,
            exists ? "true" : "false"
        );
    }

    used = append_json_chunk(buffer, buffer_size, used, "]}");
    return used;
}

size_t espclaw_render_apps_json(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size
)
{
    char ids[8][ESPCLAW_APP_ID_MAX + 1];
    size_t count = 0;
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used = append_json_chunk(buffer, buffer_size, used, "{\"apps\":[");
    if (workspace_root != NULL && espclaw_app_collect_ids(workspace_root, ids, 8, &count) == 0) {
        for (index = 0; index < count; ++index) {
            espclaw_app_manifest_t manifest;

            if (espclaw_app_load_manifest(workspace_root, ids[index], &manifest) != 0) {
                continue;
            }

            if (index > 0) {
                used = append_json_chunk(buffer, buffer_size, used, ",");
            }

            used = append_json_chunk(buffer, buffer_size, used, "{");
            used = append_json_chunk(buffer, buffer_size, used, "\"id\":");
            used = append_json_escaped_string(buffer, buffer_size, used, manifest.app_id);
            used = append_json_chunk(buffer, buffer_size, used, ",\"title\":");
            used = append_json_escaped_string(buffer, buffer_size, used, manifest.title);
            used = append_json_chunk(buffer, buffer_size, used, ",\"version\":");
            used = append_json_escaped_string(buffer, buffer_size, used, manifest.version);
            used = append_json_chunk(buffer, buffer_size, used, ",\"entrypoint\":");
            used = append_json_escaped_string(buffer, buffer_size, used, manifest.entrypoint);
            used = append_json_chunk(buffer, buffer_size, used, ",\"permissions\":");
            used = append_string_array_json(
                buffer,
                buffer_size,
                used,
                manifest.permissions,
                manifest.permission_count
            );
            used = append_json_chunk(buffer, buffer_size, used, ",\"triggers\":");
            used = append_trigger_array_json(
                buffer,
                buffer_size,
                used,
                manifest.triggers,
                manifest.trigger_count
            );
            used = append_json_chunk(buffer, buffer_size, used, "}");
        }
    }
    used = append_json_chunk(buffer, buffer_size, used, "]}");
    return used;
}

size_t espclaw_render_tools_json(
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used = append_json_chunk(buffer, buffer_size, used, "{\"tools\":[");
    for (index = 0; index < espclaw_tool_count(); ++index) {
        const espclaw_tool_descriptor_t *tool = espclaw_tool_at(index);

        if (tool == NULL) {
            continue;
        }
        if (index > 0) {
            used = append_json_chunk(buffer, buffer_size, used, ",");
        }
        used = append_json_chunk(buffer, buffer_size, used, "{\"name\":");
        used = append_json_escaped_string(buffer, buffer_size, used, tool->name);
        used = append_json_chunk(buffer, buffer_size, used, ",\"summary\":");
        used = append_json_escaped_string(buffer, buffer_size, used, tool->summary);
        used = append_json_chunk(buffer, buffer_size, used, ",\"safety\":");
        used = append_json_escaped_string(buffer, buffer_size, used, tool_safety_label(tool->safety));
        used = append_json_chunk(buffer, buffer_size, used, ",\"parameters\":%s}", tool->parameters_json);
    }
    used = append_json_chunk(buffer, buffer_size, used, "]}");
    return used;
}

size_t espclaw_render_app_detail_json(
    const char *workspace_root,
    const char *app_id,
    char *buffer,
    size_t buffer_size
)
{
    espclaw_app_manifest_t manifest;
    char source[4096];
    size_t used = 0;
    int read_result;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    if (workspace_root == NULL || app_id == NULL ||
        espclaw_app_load_manifest(workspace_root, app_id, &manifest) != 0) {
        return (size_t)snprintf(buffer, buffer_size, "{\"error\":\"app_not_found\"}");
    }

    source[0] = '\0';
    read_result = espclaw_app_read_source(workspace_root, app_id, source, sizeof(source));

    used = append_json_chunk(buffer, buffer_size, used, "{\"app\":{");
    used = append_json_chunk(buffer, buffer_size, used, "\"id\":");
    used = append_json_escaped_string(buffer, buffer_size, used, manifest.app_id);
    used = append_json_chunk(buffer, buffer_size, used, ",\"title\":");
    used = append_json_escaped_string(buffer, buffer_size, used, manifest.title);
    used = append_json_chunk(buffer, buffer_size, used, ",\"version\":");
    used = append_json_escaped_string(buffer, buffer_size, used, manifest.version);
    used = append_json_chunk(buffer, buffer_size, used, ",\"entrypoint\":");
    used = append_json_escaped_string(buffer, buffer_size, used, manifest.entrypoint);
    used = append_json_chunk(buffer, buffer_size, used, ",\"permissions\":");
    used = append_string_array_json(
        buffer,
        buffer_size,
        used,
        manifest.permissions,
        manifest.permission_count
    );
    used = append_json_chunk(buffer, buffer_size, used, ",\"triggers\":");
    used = append_trigger_array_json(
        buffer,
        buffer_size,
        used,
        manifest.triggers,
        manifest.trigger_count
    );
    used = append_json_chunk(buffer, buffer_size, used, ",\"source_available\":%s", read_result == 0 ? "true" : "false");
    used = append_json_chunk(buffer, buffer_size, used, ",\"source\":");
    used = append_json_escaped_string(buffer, buffer_size, used, read_result == 0 ? source : "");
    used = append_json_chunk(buffer, buffer_size, used, "}}");
    return used;
}

size_t espclaw_render_auth_profile_json(
    const espclaw_auth_profile_t *profile,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }
    if (profile == NULL) {
        return (size_t)snprintf(buffer, buffer_size, "{\"configured\":false}");
    }

    used = append_json_chunk(buffer, buffer_size, used, "{");
    used = append_json_chunk(buffer, buffer_size, used, "\"configured\":%s", profile->configured ? "true" : "false");
    used = append_json_chunk(buffer, buffer_size, used, ",\"provider_id\":");
    used = append_json_escaped_string(buffer, buffer_size, used, profile->provider_id);
    used = append_json_chunk(buffer, buffer_size, used, ",\"model\":");
    used = append_json_escaped_string(buffer, buffer_size, used, profile->model);
    used = append_json_chunk(buffer, buffer_size, used, ",\"base_url\":");
    used = append_json_escaped_string(buffer, buffer_size, used, profile->base_url);
    used = append_json_chunk(buffer, buffer_size, used, ",\"account_id\":");
    used = append_json_escaped_string(buffer, buffer_size, used, profile->account_id);
    used = append_json_chunk(buffer, buffer_size, used, ",\"source\":");
    used = append_json_escaped_string(buffer, buffer_size, used, profile->source);
    used = append_json_chunk(buffer, buffer_size, used, ",\"has_refresh_token\":%s", profile->refresh_token[0] != '\0' ? "true" : "false");
    used = append_json_chunk(buffer, buffer_size, used, ",\"expires_at\":%ld", profile->expires_at);
    used = append_json_chunk(buffer, buffer_size, used, "}");
    return used;
}

size_t espclaw_render_session_transcript_json(
    const char *workspace_root,
    const char *session_id,
    char *buffer,
    size_t buffer_size
)
{
    char transcript[8192];
    size_t used = 0;
    const char *source = "";

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    if (workspace_root != NULL && session_id != NULL &&
        espclaw_session_read_transcript(workspace_root, session_id, transcript, sizeof(transcript)) == 0) {
        source = transcript;
    }

    used = append_json_chunk(buffer, buffer_size, used, "{\"session_id\":");
    used = append_json_escaped_string(buffer, buffer_size, used, session_id != NULL ? session_id : "");
    used = append_json_chunk(buffer, buffer_size, used, ",\"transcript\":");
    used = append_json_escaped_string(buffer, buffer_size, used, source);
    used = append_json_chunk(buffer, buffer_size, used, "}");
    return used;
}
