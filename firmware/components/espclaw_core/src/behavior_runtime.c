#include "espclaw/behavior_runtime.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "espclaw/app_runtime.h"
#include "espclaw/workspace.h"

static bool copy_json_string_value(const char *start, char *buffer, size_t buffer_size)
{
    size_t index = 0;
    const char *cursor = start;

    if (start == NULL || buffer == NULL || buffer_size == 0 || *start != '"') {
        return false;
    }

    cursor++;
    while (*cursor != '\0' && *cursor != '"' && index + 1 < buffer_size) {
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor++;
            switch (*cursor) {
            case 'n':
                buffer[index++] = '\n';
                break;
            case 'r':
                buffer[index++] = '\r';
                break;
            case 't':
                buffer[index++] = '\t';
                break;
            default:
                buffer[index++] = *cursor;
                break;
            }
        } else {
            buffer[index++] = *cursor;
        }
        cursor++;
    }

    if (*cursor != '"') {
        return false;
    }

    buffer[index] = '\0';
    return true;
}

static const char *find_key(const char *json, const char *key)
{
    char pattern[64];

    if (json == NULL || key == NULL) {
        return NULL;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(json, pattern);
}

static bool extract_string_after_key(const char *json, const char *key, char *buffer, size_t buffer_size)
{
    const char *key_start = find_key(json, key);
    const char *value_start = NULL;

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (key_start == NULL || buffer == NULL || buffer_size == 0) {
        return false;
    }

    value_start = strchr(key_start, ':');
    if (value_start == NULL) {
        return false;
    }
    value_start++;

    while (*value_start == ' ' || *value_start == '\n' || *value_start == '\r' || *value_start == '\t') {
        value_start++;
    }

    return copy_json_string_value(value_start, buffer, buffer_size);
}

static bool extract_uint_after_key(const char *json, const char *key, uint32_t *value_out)
{
    const char *key_start = find_key(json, key);
    const char *value_start = NULL;
    char *end_ptr = NULL;
    unsigned long value;

    if (key_start == NULL || value_out == NULL) {
        return false;
    }

    value_start = strchr(key_start, ':');
    if (value_start == NULL) {
        return false;
    }
    value_start++;
    while (*value_start == ' ' || *value_start == '\n' || *value_start == '\r' || *value_start == '\t') {
        value_start++;
    }

    value = strtoul(value_start, &end_ptr, 10);
    if (end_ptr == value_start) {
        return false;
    }

    *value_out = (uint32_t)value;
    return true;
}

static bool extract_bool_after_key(const char *json, const char *key, bool *value_out)
{
    const char *key_start = find_key(json, key);
    const char *value_start = NULL;

    if (key_start == NULL || value_out == NULL) {
        return false;
    }

    value_start = strchr(key_start, ':');
    if (value_start == NULL) {
        return false;
    }
    value_start++;
    while (*value_start == ' ' || *value_start == '\n' || *value_start == '\r' || *value_start == '\t') {
        value_start++;
    }

    if (strncmp(value_start, "true", 4) == 0) {
        *value_out = true;
        return true;
    }
    if (strncmp(value_start, "false", 5) == 0) {
        *value_out = false;
        return true;
    }
    if (*value_start == '1') {
        *value_out = true;
        return true;
    }
    if (*value_start == '0') {
        *value_out = false;
        return true;
    }

    return false;
}

static int json_escape_copy(const char *input, char *output, size_t output_size)
{
    size_t written = 0;

    if (output == NULL || output_size == 0) {
        return -1;
    }

    while (input != NULL && *input != '\0' && written + 1 < output_size) {
        const char *replacement = NULL;

        switch (*input) {
        case '\\':
            replacement = "\\\\";
            break;
        case '"':
            replacement = "\\\"";
            break;
        case '\n':
            replacement = "\\n";
            break;
        case '\r':
            replacement = "\\r";
            break;
        case '\t':
            replacement = "\\t";
            break;
        default:
            break;
        }

        if (replacement != NULL) {
            size_t replacement_len = strlen(replacement);

            if (written + replacement_len >= output_size) {
                break;
            }
            memcpy(output + written, replacement, replacement_len);
            written += replacement_len;
        } else {
            output[written++] = *input;
        }
        input++;
    }

    output[written] = '\0';
    return 0;
}

static int build_behavior_relative_path(const char *behavior_id, char *buffer, size_t buffer_size)
{
    int written;

    if (!espclaw_behavior_id_is_valid(behavior_id) || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    written = snprintf(buffer, buffer_size, "behaviors/%s.json", behavior_id);
    if (written < 0 || (size_t)written >= buffer_size) {
        return -1;
    }
    return 0;
}

static int ensure_behaviors_directory(const char *workspace_root)
{
    char path[512];

    if (espclaw_workspace_resolve_path(workspace_root, "behaviors", path, sizeof(path)) != 0) {
        return -1;
    }
    if (mkdir(path, 0x1ED) == 0 || errno == EEXIST) {
        return 0;
    }
    return -1;
}

static int render_behavior_spec_json(const espclaw_behavior_spec_t *spec, char *buffer, size_t buffer_size)
{
    char title[ESPCLAW_BEHAVIOR_TITLE_MAX * 2 + 1];
    char app_id[33 * 2];
    char schedule[ESPCLAW_TASK_SCHEDULE_MAX * 2 + 1];
    char trigger[ESPCLAW_TASK_TRIGGER_MAX * 2 + 1];
    char payload[ESPCLAW_TASK_PAYLOAD_MAX * 2 + 1];

    if (spec == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    json_escape_copy(spec->title, title, sizeof(title));
    json_escape_copy(spec->app_id, app_id, sizeof(app_id));
    json_escape_copy(spec->schedule, schedule, sizeof(schedule));
    json_escape_copy(spec->trigger, trigger, sizeof(trigger));
    json_escape_copy(spec->payload, payload, sizeof(payload));

    snprintf(
        buffer,
        buffer_size,
        "{\n"
        "  \"id\": \"%s\",\n"
        "  \"title\": \"%s\",\n"
        "  \"app_id\": \"%s\",\n"
        "  \"schedule\": \"%s\",\n"
        "  \"trigger\": \"%s\",\n"
        "  \"payload\": \"%s\",\n"
        "  \"period_ms\": %u,\n"
        "  \"max_iterations\": %u,\n"
        "  \"autostart\": %s\n"
        "}\n",
        spec->behavior_id,
        title,
        app_id,
        schedule,
        trigger,
        payload,
        (unsigned)spec->period_ms,
        (unsigned)spec->max_iterations,
        spec->autostart ? "true" : "false"
    );
    return 0;
}

static bool merge_task_status(
    const char *behavior_id,
    const espclaw_task_status_t *tasks,
    size_t task_count,
    espclaw_behavior_status_t *status)
{
    size_t index;

    if (behavior_id == NULL || tasks == NULL || status == NULL) {
        return false;
    }

    for (index = 0; index < task_count; ++index) {
        if (strcmp(tasks[index].task_id, behavior_id) != 0) {
            continue;
        }
        status->active = tasks[index].active;
        status->completed = tasks[index].completed;
        status->stop_requested = tasks[index].stop_requested;
        status->iterations_completed = tasks[index].iterations_completed;
        status->events_received = tasks[index].events_received;
        status->last_status = tasks[index].last_status;
        snprintf(status->last_result, sizeof(status->last_result), "%s", tasks[index].last_result);
        return true;
    }

    return false;
}

bool espclaw_behavior_id_is_valid(const char *behavior_id)
{
    size_t index;

    if (behavior_id == NULL || behavior_id[0] == '\0') {
        return false;
    }
    for (index = 0; behavior_id[index] != '\0'; ++index) {
        char c = behavior_id[index];

        if (!(isalnum((unsigned char)c) || c == '_' || c == '-')) {
            return false;
        }
        if (index >= ESPCLAW_BEHAVIOR_ID_MAX) {
            return false;
        }
    }
    return true;
}

int espclaw_behavior_load(
    const char *workspace_root,
    const char *behavior_id,
    espclaw_behavior_spec_t *spec)
{
    char relative_path[128];
    char json[1024];

    if (workspace_root == NULL || spec == NULL || build_behavior_relative_path(behavior_id, relative_path, sizeof(relative_path)) != 0) {
        return -1;
    }
    if (espclaw_workspace_read_file(workspace_root, relative_path, json, sizeof(json)) != 0) {
        return -1;
    }

    memset(spec, 0, sizeof(*spec));
    if (!extract_string_after_key(json, "id", spec->behavior_id, sizeof(spec->behavior_id)) ||
        !extract_string_after_key(json, "title", spec->title, sizeof(spec->title)) ||
        !extract_string_after_key(json, "app_id", spec->app_id, sizeof(spec->app_id)) ||
        !extract_string_after_key(json, "schedule", spec->schedule, sizeof(spec->schedule)) ||
        !extract_string_after_key(json, "trigger", spec->trigger, sizeof(spec->trigger))) {
        return -1;
    }

    if (!extract_string_after_key(json, "payload", spec->payload, sizeof(spec->payload))) {
        spec->payload[0] = '\0';
    }
    if (!extract_uint_after_key(json, "period_ms", &spec->period_ms)) {
        spec->period_ms = 20;
    }
    if (!extract_uint_after_key(json, "max_iterations", &spec->max_iterations)) {
        spec->max_iterations = 0;
    }
    if (!extract_bool_after_key(json, "autostart", &spec->autostart)) {
        spec->autostart = false;
    }

    return 0;
}

int espclaw_behavior_register(
    const char *workspace_root,
    const espclaw_behavior_spec_t *spec,
    char *buffer,
    size_t buffer_size)
{
    char relative_path[128];
    char json[1024];
    espclaw_behavior_spec_t normalized;
    espclaw_app_manifest_t manifest;

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (workspace_root == NULL || spec == NULL) {
        return -1;
    }

    memset(&normalized, 0, sizeof(normalized));
    snprintf(normalized.behavior_id, sizeof(normalized.behavior_id), "%s", spec->behavior_id);
    snprintf(normalized.title, sizeof(normalized.title), "%s", spec->title[0] != '\0' ? spec->title : spec->behavior_id);
    snprintf(normalized.app_id, sizeof(normalized.app_id), "%s", spec->app_id);
    snprintf(
        normalized.schedule,
        sizeof(normalized.schedule),
        "%s",
        spec->schedule[0] != '\0' ? spec->schedule : "periodic"
    );
    snprintf(
        normalized.trigger,
        sizeof(normalized.trigger),
        "%s",
        spec->trigger[0] != '\0' ? spec->trigger : (strcmp(normalized.schedule, "event") == 0 ? "sensor" : "timer")
    );
    snprintf(normalized.payload, sizeof(normalized.payload), "%s", spec->payload);
    normalized.period_ms = strcmp(normalized.schedule, "event") == 0 ? 0 : (spec->period_ms > 0 ? spec->period_ms : 20U);
    normalized.max_iterations = spec->max_iterations;
    normalized.autostart = spec->autostart;

    if (!espclaw_behavior_id_is_valid(normalized.behavior_id) ||
        normalized.app_id[0] == '\0' ||
        normalized.trigger[0] == '\0' ||
        (strcmp(normalized.schedule, "periodic") != 0 && strcmp(normalized.schedule, "event") != 0)) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "invalid behavior spec");
        }
        return -1;
    }
    if (strcmp(normalized.schedule, "periodic") == 0 && normalized.period_ms == 0) {
        normalized.period_ms = 20;
    }
    if (espclaw_app_load_manifest(workspace_root, normalized.app_id, &manifest) != 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "app %s not found for behavior %s", normalized.app_id, normalized.behavior_id);
        }
        return -1;
    }
    if (ensure_behaviors_directory(workspace_root) != 0 ||
        build_behavior_relative_path(normalized.behavior_id, relative_path, sizeof(relative_path)) != 0 ||
        render_behavior_spec_json(&normalized, json, sizeof(json)) != 0 ||
        espclaw_workspace_write_file(workspace_root, relative_path, json) != 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "failed to save behavior");
        }
        return -1;
    }

    if (buffer != NULL && buffer_size > 0) {
        snprintf(buffer, buffer_size, "behavior %s saved", normalized.behavior_id);
    }
    return 0;
}

int espclaw_behavior_remove(
    const char *workspace_root,
    const char *behavior_id,
    char *buffer,
    size_t buffer_size)
{
    char relative_path[128];
    char absolute_path[512];

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (workspace_root == NULL || build_behavior_relative_path(behavior_id, relative_path, sizeof(relative_path)) != 0 ||
        espclaw_workspace_resolve_path(workspace_root, relative_path, absolute_path, sizeof(absolute_path)) != 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "invalid behavior id");
        }
        return -1;
    }

    (void)espclaw_task_stop(behavior_id, NULL, 0);
    if (unlink(absolute_path) != 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "failed to remove behavior %s", behavior_id != NULL ? behavior_id : "");
        }
        return -1;
    }

    if (buffer != NULL && buffer_size > 0) {
        snprintf(buffer, buffer_size, "behavior %s removed", behavior_id);
    }
    return 0;
}

size_t espclaw_behavior_snapshot_all(
    const char *workspace_root,
    espclaw_behavior_status_t *statuses,
    size_t max_statuses)
{
    char behaviors_path[512];
    espclaw_task_status_t *tasks = NULL;
    size_t task_count;
    DIR *dir;
    struct dirent *entry;
    size_t count = 0;

    if (workspace_root == NULL || statuses == NULL || max_statuses == 0) {
        return 0;
    }
    if (espclaw_workspace_resolve_path(workspace_root, "behaviors", behaviors_path, sizeof(behaviors_path)) != 0) {
        return 0;
    }

    dir = opendir(behaviors_path);
    if (dir == NULL) {
        return 0;
    }

    tasks = calloc(ESPCLAW_TASK_RUNTIME_MAX, sizeof(*tasks));
    if (tasks == NULL) {
        closedir(dir);
        return 0;
    }

    task_count = espclaw_task_snapshot_all(tasks, ESPCLAW_TASK_RUNTIME_MAX);
    while ((entry = readdir(dir)) != NULL && count < max_statuses) {
        size_t name_len;
        char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        name_len = strlen(entry->d_name);
        if (name_len <= 5 || strcmp(entry->d_name + name_len - 5, ".json") != 0) {
            continue;
        }
        if (name_len - 5 > ESPCLAW_BEHAVIOR_ID_MAX) {
            continue;
        }

        memcpy(behavior_id, entry->d_name, name_len - 5);
        behavior_id[name_len - 5] = '\0';
        memset(&statuses[count], 0, sizeof(statuses[count]));
        if (espclaw_behavior_load(workspace_root, behavior_id, &statuses[count].spec) != 0) {
            continue;
        }
        (void)merge_task_status(statuses[count].spec.behavior_id, tasks, task_count, &statuses[count]);
        count++;
    }

    closedir(dir);
    free(tasks);
    return count;
}

int espclaw_behavior_render_json(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size)
{
    espclaw_behavior_status_t *statuses = NULL;
    size_t count;
    size_t index;
    size_t written = 0;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }

    statuses = calloc(ESPCLAW_TASK_RUNTIME_MAX, sizeof(*statuses));
    if (statuses == NULL) {
        snprintf(buffer, buffer_size, "{\"behaviors\":[]}");
        return -1;
    }

    count = espclaw_behavior_snapshot_all(workspace_root, statuses, ESPCLAW_TASK_RUNTIME_MAX);

    written += (size_t)snprintf(buffer + written, buffer_size - written, "{\"behaviors\":[");
    for (index = 0; index < count && written < buffer_size; ++index) {
        char escaped_title[ESPCLAW_BEHAVIOR_TITLE_MAX * 2 + 1];
        char escaped_payload[ESPCLAW_TASK_PAYLOAD_MAX * 2 + 1];
        char escaped_result[ESPCLAW_TASK_RESULT_MAX * 2 + 1];

        json_escape_copy(statuses[index].spec.title, escaped_title, sizeof(escaped_title));
        json_escape_copy(statuses[index].spec.payload, escaped_payload, sizeof(escaped_payload));
        json_escape_copy(statuses[index].last_result, escaped_result, sizeof(escaped_result));
        written += (size_t)snprintf(
            buffer + written,
            buffer_size - written,
            "%s{\"behavior_id\":\"%s\",\"title\":\"%s\",\"app_id\":\"%s\",\"schedule\":\"%s\",\"trigger\":\"%s\","
            "\"payload\":\"%s\",\"period_ms\":%u,\"max_iterations\":%u,\"autostart\":%s,"
            "\"active\":%s,\"completed\":%s,\"stop_requested\":%s,\"iterations_completed\":%u,"
            "\"events_received\":%u,\"last_status\":%d,\"last_result\":\"%s\"}",
            index == 0 ? "" : ",",
            statuses[index].spec.behavior_id,
            escaped_title,
            statuses[index].spec.app_id,
            statuses[index].spec.schedule,
            statuses[index].spec.trigger,
            escaped_payload,
            (unsigned)statuses[index].spec.period_ms,
            (unsigned)statuses[index].spec.max_iterations,
            statuses[index].spec.autostart ? "true" : "false",
            statuses[index].active ? "true" : "false",
            statuses[index].completed ? "true" : "false",
            statuses[index].stop_requested ? "true" : "false",
            (unsigned)statuses[index].iterations_completed,
            (unsigned)statuses[index].events_received,
            statuses[index].last_status,
            escaped_result
        );
    }
    written += (size_t)snprintf(buffer + written, buffer_size - written, "]}");

    if (written >= buffer_size) {
        buffer[buffer_size - 1] = '\0';
    }
    free(statuses);
    return 0;
}

int espclaw_behavior_start(
    const char *workspace_root,
    const char *behavior_id,
    char *buffer,
    size_t buffer_size)
{
    espclaw_behavior_spec_t spec;

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    if (workspace_root == NULL || espclaw_behavior_load(workspace_root, behavior_id, &spec) != 0) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "behavior %s not found", behavior_id != NULL ? behavior_id : "");
        }
        return -1;
    }

    return espclaw_task_start_with_schedule(
        spec.behavior_id,
        workspace_root,
        spec.app_id,
        spec.schedule,
        spec.trigger,
        spec.payload,
        spec.period_ms,
        spec.max_iterations,
        buffer,
        buffer_size
    );
}

int espclaw_behavior_stop(const char *behavior_id, char *buffer, size_t buffer_size)
{
    return espclaw_task_stop(behavior_id, buffer, buffer_size);
}

int espclaw_behavior_start_autostart(
    const char *workspace_root,
    char *buffer,
    size_t buffer_size)
{
    espclaw_behavior_status_t *statuses = NULL;
    size_t count;
    size_t index;
    size_t started = 0;
    size_t used = 0;

    if (buffer != NULL && buffer_size > 0) {
        buffer[0] = '\0';
    }
    statuses = calloc(ESPCLAW_TASK_RUNTIME_MAX, sizeof(*statuses));
    if (statuses == NULL) {
        if (buffer != NULL && buffer_size > 0) {
            snprintf(buffer, buffer_size, "failed to allocate behavior snapshot");
        }
        return -1;
    }

    count = espclaw_behavior_snapshot_all(workspace_root, statuses, ESPCLAW_TASK_RUNTIME_MAX);
    for (index = 0; index < count; ++index) {
        char message[256];

        if (!statuses[index].spec.autostart || statuses[index].active) {
            continue;
        }
        if (espclaw_behavior_start(workspace_root, statuses[index].spec.behavior_id, message, sizeof(message)) == 0) {
            started++;
            if (buffer != NULL && buffer_size > 0) {
                used += (size_t)snprintf(
                    buffer + used,
                    buffer_size - used,
                    "%s%s",
                    used == 0 ? "" : "; ",
                    statuses[index].spec.behavior_id
                );
            }
        }
    }

    if (buffer != NULL && buffer_size > 0 && started == 0) {
        snprintf(buffer, buffer_size, "no autostart behaviors");
    }
    free(statuses);
    return 0;
}
