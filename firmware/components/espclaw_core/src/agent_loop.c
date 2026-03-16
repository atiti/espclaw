#include "espclaw/agent_loop.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef ESPCLAW_HOST_LUA
#include <unistd.h>
#else
#include "esp_http_client.h"
#endif

#include "espclaw/admin_api.h"
#include "espclaw/app_runtime.h"
#include "espclaw/auth_store.h"
#include "espclaw/hardware.h"
#include "espclaw/provider.h"
#include "espclaw/session_store.h"
#include "espclaw/storage.h"
#include "espclaw/tool_catalog.h"
#include "espclaw/workspace.h"

#define ESPCLAW_AGENT_HISTORY_MAX 24
#define ESPCLAW_AGENT_HTTP_BUFFER_MAX 65536

typedef struct {
    char role[16];
    char content[1024];
} espclaw_history_message_t;

typedef struct {
    char id[ESPCLAW_AGENT_RESPONSE_ID_MAX + 1];
    char text[ESPCLAW_AGENT_TEXT_MAX + 1];
    espclaw_agent_tool_call_t tool_calls[ESPCLAW_AGENT_TOOL_CALL_MAX];
    size_t tool_call_count;
} espclaw_provider_response_t;

static espclaw_agent_http_adapter_t s_http_adapter;
static void *s_http_adapter_user_data;

static void copy_text(char *buffer, size_t buffer_size, const char *value)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", value != NULL ? value : "");
}

static size_t append_escaped_json(char *buffer, size_t buffer_size, size_t used, const char *value)
{
    const char *cursor = value != NULL ? value : "";

    if (buffer == NULL || buffer_size == 0 || used >= buffer_size) {
        return used;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"");
    while (*cursor != '\0' && used + 2 < buffer_size) {
        switch (*cursor) {
        case '\\':
        case '"':
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\\%c", *cursor);
            break;
        case '\n':
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\\n");
            break;
        case '\r':
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\\r");
            break;
        case '\t':
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\\t");
            break;
        default:
            buffer[used++] = *cursor;
            buffer[used] = '\0';
            break;
        }
        cursor++;
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"");
    return used >= buffer_size ? buffer_size - 1 : used;
}

static bool is_tool_wire_safe(unsigned char value)
{
    return isalnum(value) || value == '-';
}

static void encode_tool_name(const char *runtime_name, char *wire_name, size_t wire_name_size)
{
    const unsigned char *cursor = (const unsigned char *)(runtime_name != NULL ? runtime_name : "");
    size_t used = 0;

    if (wire_name == NULL || wire_name_size == 0) {
        return;
    }

    while (*cursor != '\0' && used + 1 < wire_name_size) {
        if (is_tool_wire_safe(*cursor)) {
            wire_name[used++] = (char)*cursor;
            wire_name[used] = '\0';
        } else {
            int written = snprintf(wire_name + used, wire_name_size - used, "_x%02X_", *cursor);

            if (written <= 0 || (size_t)written >= wire_name_size - used) {
                break;
            }
            used += (size_t)written;
        }
        cursor++;
    }
    wire_name[used] = '\0';
}

static void decode_tool_name(const char *wire_name, char *runtime_name, size_t runtime_name_size)
{
    const char *cursor = wire_name != NULL ? wire_name : "";
    size_t used = 0;

    if (runtime_name == NULL || runtime_name_size == 0) {
        return;
    }

    while (*cursor != '\0' && used + 1 < runtime_name_size) {
        if (cursor[0] == '_' &&
            cursor[1] == 'x' &&
            isxdigit((unsigned char)cursor[2]) &&
            isxdigit((unsigned char)cursor[3]) &&
            cursor[4] == '_') {
            char hex[3];

            hex[0] = cursor[2];
            hex[1] = cursor[3];
            hex[2] = '\0';
            runtime_name[used++] = (char)strtol(hex, NULL, 16);
            runtime_name[used] = '\0';
            cursor += 5;
            continue;
        }
        runtime_name[used++] = *cursor++;
        runtime_name[used] = '\0';
    }
    runtime_name[used] = '\0';
}

static const char *find_json_key_after(const char *json, const char *key, const char *after)
{
    char pattern[64];
    const char *cursor = after != NULL ? after : json;

    if (json == NULL || key == NULL) {
        return NULL;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    return strstr(cursor, pattern);
}

static bool extract_json_string_from(const char *json, const char *key, const char *after, char *buffer, size_t buffer_size)
{
    const char *cursor = find_json_key_after(json, key, after);
    size_t used = 0;

    if (cursor == NULL || buffer == NULL || buffer_size == 0) {
        if (buffer != NULL && buffer_size > 0) {
            buffer[0] = '\0';
        }
        return false;
    }

    cursor = strchr(cursor, ':');
    if (cursor == NULL) {
        buffer[0] = '\0';
        return false;
    }
    cursor++;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor != '"') {
        buffer[0] = '\0';
        return false;
    }
    cursor++;

    while (*cursor != '\0' && *cursor != '"' && used + 1 < buffer_size) {
        if (*cursor == '\\' && cursor[1] != '\0') {
            cursor++;
            switch (*cursor) {
            case 'n':
                buffer[used++] = '\n';
                break;
            case 'r':
                buffer[used++] = '\r';
                break;
            case 't':
                buffer[used++] = '\t';
                break;
            default:
                buffer[used++] = *cursor;
                break;
            }
        } else {
            buffer[used++] = *cursor;
        }
        cursor++;
    }
    buffer[used] = '\0';
    return true;
}

#ifdef ESPCLAW_HOST_LUA
static bool contains_case_insensitive(const char *haystack, const char *needle)
{
    size_t needle_length;
    const char *cursor;

    if (haystack == NULL || needle == NULL) {
        return false;
    }

    needle_length = strlen(needle);
    if (needle_length == 0) {
        return true;
    }

    for (cursor = haystack; *cursor != '\0'; ++cursor) {
        size_t index = 0;

        while (index < needle_length &&
               cursor[index] != '\0' &&
               tolower((unsigned char)cursor[index]) == tolower((unsigned char)needle[index])) {
            index++;
        }
        if (index == needle_length) {
            return true;
        }
    }

    return false;
}

static bool extract_last_user_message(const char *request_body, char *buffer, size_t buffer_size)
{
    const char *cursor;
    bool found = false;

    if (buffer == NULL || buffer_size == 0) {
        return false;
    }

    buffer[0] = '\0';
    if (request_body == NULL) {
        return false;
    }

    cursor = request_body;
    while ((cursor = strstr(cursor, "\"role\":\"user\"")) != NULL) {
        if (extract_json_string_from(request_body, "content", cursor, buffer, buffer_size)) {
            found = true;
        }
        cursor += strlen("\"role\":\"user\"");
    }

    return found;
}

static bool user_message_requests_tool_listing(const char *request_body)
{
    char user_message[1024];

    if (!extract_last_user_message(request_body, user_message, sizeof(user_message))) {
        return false;
    }

    return contains_case_insensitive(user_message, "available tools") ||
           contains_case_insensitive(user_message, "what tools") ||
           contains_case_insensitive(user_message, "what capabilities") ||
           contains_case_insensitive(user_message, "list out all the available tools");
}
#endif

static int load_system_prompt(const char *workspace_root, char *buffer, size_t buffer_size)
{
    static const char *CONTROL_FILES[] = {"AGENTS.md", "IDENTITY.md", "USER.md", "HEARTBEAT.md", "memory/MEMORY.md"};
    size_t index;
    size_t used = 0;

    if (workspace_root == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    buffer[0] = '\0';
    for (index = 0; index < sizeof(CONTROL_FILES) / sizeof(CONTROL_FILES[0]); ++index) {
        char content[2048];

        if (espclaw_workspace_read_file(workspace_root, CONTROL_FILES[index], content, sizeof(content)) != 0) {
            continue;
        }
        if (used > 0 && used + 2 < buffer_size) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, "\n\n");
        }
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%s", content);
        if (used >= buffer_size) {
            buffer[buffer_size - 1] = '\0';
            return 0;
        }
    }

    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        "\n\n# Tool policy\n"
        "- Use tools when they materially reduce guesswork.\n"
        "- When the user asks what tools or capabilities are available, call tool.list before answering.\n"
        "- Read-only tools are preferred when exploring.\n"
        "- Mutating tools require explicit confirmation from the user before execution.\n"
        "- When a tool reports confirmation_required, ask the user for confirmation instead of retrying the same call.\n"
    );
    return 0;
}

static int load_history(const char *workspace_root, const char *session_id, espclaw_history_message_t *messages, size_t max_messages, size_t *count_out)
{
    char transcript[16384];
    const char *cursor;
    size_t count = 0;

    if (count_out == NULL) {
        return -1;
    }
    *count_out = 0;
    if (workspace_root == NULL || session_id == NULL || messages == NULL || max_messages == 0) {
        return -1;
    }
    if (espclaw_session_read_transcript(workspace_root, session_id, transcript, sizeof(transcript)) != 0) {
        return 0;
    }

    cursor = transcript;
    while (*cursor != '\0') {
        const char *line_end = strchr(cursor, '\n');
        char line[1200];
        size_t line_len = line_end != NULL ? (size_t)(line_end - cursor) : strlen(cursor);

        if (line_len >= sizeof(line)) {
            line_len = sizeof(line) - 1;
        }
        memcpy(line, cursor, line_len);
        line[line_len] = '\0';

        if (count < max_messages) {
            if (extract_json_string_from(line, "role", NULL, messages[count].role, sizeof(messages[count].role)) &&
                extract_json_string_from(line, "content", NULL, messages[count].content, sizeof(messages[count].content))) {
                count++;
            }
        } else {
            memmove(messages, messages + 1, (max_messages - 1) * sizeof(messages[0]));
            if (extract_json_string_from(line, "role", NULL, messages[max_messages - 1].role, sizeof(messages[max_messages - 1].role)) &&
                extract_json_string_from(line, "content", NULL, messages[max_messages - 1].content, sizeof(messages[max_messages - 1].content))) {
                count = max_messages;
            }
        }

        if (line_end == NULL) {
            break;
        }
        cursor = line_end + 1;
    }

    *count_out = count;
    return 0;
}

static size_t append_tool_schemas(char *buffer, size_t buffer_size, size_t used)
{
    size_t index;

    used += (size_t)snprintf(buffer + used, buffer_size - used, "[");
    for (index = 0; index < espclaw_tool_count(); ++index) {
        const espclaw_tool_descriptor_t *tool = espclaw_tool_at(index);
        char wire_name[96];

        if (tool == NULL) {
            continue;
        }
        encode_tool_name(tool->name, wire_name, sizeof(wire_name));
        if (index > 0) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "{\"type\":\"function\",\"name\":"
        );
        used = append_escaped_json(buffer, buffer_size, used, wire_name);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"description\":");
        used = append_escaped_json(buffer, buffer_size, used, tool->summary);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"parameters\":%s}", tool->parameters_json);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]");
    return used >= buffer_size ? buffer_size - 1 : used;
}

static size_t append_history_items(
    char *buffer,
    size_t buffer_size,
    size_t used,
    const espclaw_history_message_t *messages,
    size_t count
)
{
    size_t index;

    for (index = 0; index < count; ++index) {
        const char *role = messages[index].role;
        char content[1152];

        if (index > 0) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        if (strcmp(role, "tool") == 0) {
            role = "user";
            snprintf(content, sizeof(content), "Tool output:\n%s", messages[index].content);
        } else {
            copy_text(content, sizeof(content), messages[index].content);
        }

        used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"role\":");
        used = append_escaped_json(buffer, buffer_size, used, role);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"content\":");
        used = append_escaped_json(buffer, buffer_size, used, content);
        used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    }
    return used >= buffer_size ? buffer_size - 1 : used;
}

static size_t append_history_input(
    char *buffer,
    size_t buffer_size,
    size_t used,
    const espclaw_history_message_t *messages,
    size_t count
)
{
    used += (size_t)snprintf(buffer + used, buffer_size - used, "[");
    used = append_history_items(buffer, buffer_size, used, messages, count);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]");
    return used >= buffer_size ? buffer_size - 1 : used;
}

static int extract_sse_completed_response_json(const char *payload, char *buffer, size_t buffer_size)
{
    const char *event;
    const char *data;
    const char *json_start;
    const char *json_end;
    size_t length;

    if (payload == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    event = strstr(payload, "event: response.completed");
    if (event == NULL) {
        return -1;
    }

    data = strstr(event, "\ndata: ");
    if (data == NULL) {
        return -1;
    }
    json_start = data + strlen("\ndata: ");
    json_end = strstr(json_start, "\n\n");
    if (json_end == NULL) {
        json_end = json_start + strlen(json_start);
    }

    length = (size_t)(json_end - json_start);
    if (length >= buffer_size) {
        length = buffer_size - 1;
    }
    memcpy(buffer, json_start, length);
    buffer[length] = '\0';
    return length > 0 ? 0 : -1;
}

static int build_initial_request_body(
    const espclaw_auth_profile_t *profile,
    const char *instructions,
    const espclaw_history_message_t *history,
    size_t history_count,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;

    if (profile == NULL || instructions == NULL || history == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"model\":");
    used = append_escaped_json(buffer, buffer_size, used, profile->model);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"instructions\":");
    used = append_escaped_json(buffer, buffer_size, used, instructions);
    // The ChatGPT Codex backend rejects omitted store settings, so keep the
    // request explicit and compatible across both initial and follow-up rounds.
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        ",\"store\":false,\"stream\":true,\"parallel_tool_calls\":false,\"input\":"
    );
    used = append_history_input(buffer, buffer_size, used, history, history_count);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"tools\":");
    used = append_tool_schemas(buffer, buffer_size, used);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "}");

    return 0;
}

static int build_followup_request_body(
    const espclaw_auth_profile_t *profile,
    const char *instructions,
    const char *response_id,
    const espclaw_agent_tool_call_t *tool_calls,
    const char results[][1024],
    size_t tool_count,
    char *buffer,
    size_t buffer_size
)
{
    size_t index;
    size_t used = 0;

    if (profile == NULL || instructions == NULL || response_id == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"model\":");
    used = append_escaped_json(buffer, buffer_size, used, profile->model);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"instructions\":");
    used = append_escaped_json(buffer, buffer_size, used, instructions);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"previous_response_id\":");
    used = append_escaped_json(buffer, buffer_size, used, response_id);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"store\":false,\"stream\":true,\"input\":[");
    for (index = 0; index < tool_count; ++index) {
        if (index > 0) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "{\"type\":\"function_call_output\",\"call_id\":"
        );
        used = append_escaped_json(buffer, buffer_size, used, tool_calls[index].call_id);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"output\":");
        used = append_escaped_json(buffer, buffer_size, used, results[index]);
        used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    return 0;
}

static size_t append_codex_tool_round_items(
    char *buffer,
    size_t buffer_size,
    size_t used,
    const espclaw_agent_tool_call_t *tool_calls,
    const char results[][1024],
    size_t tool_count
)
{
    size_t index;

    for (index = 0; index < tool_count; ++index) {
        char wire_name[sizeof(tool_calls[index].name) * 4];

        if (used > 0) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }

        encode_tool_name(tool_calls[index].name, wire_name, sizeof(wire_name));
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "{\"type\":\"function_call\",\"call_id\":"
        );
        used = append_escaped_json(buffer, buffer_size, used, tool_calls[index].call_id);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"name\":");
        used = append_escaped_json(buffer, buffer_size, used, wire_name);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"arguments\":");
        used = append_escaped_json(buffer, buffer_size, used, tool_calls[index].arguments_json);
        used += (size_t)snprintf(buffer + used, buffer_size - used, "},");

        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "{\"type\":\"function_call_output\",\"call_id\":"
        );
        used = append_escaped_json(buffer, buffer_size, used, tool_calls[index].call_id);
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"output\":");
        used = append_escaped_json(buffer, buffer_size, used, results[index]);
        used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    }

    return used >= buffer_size ? buffer_size - 1 : used;
}

static int build_codex_followup_request_body(
    const espclaw_auth_profile_t *profile,
    const char *instructions,
    const espclaw_history_message_t *history,
    size_t history_count,
    const char *codex_items_json,
    char *buffer,
    size_t buffer_size
)
{
    size_t used = 0;
    bool has_history = history != NULL && history_count > 0;
    bool has_codex_items = codex_items_json != NULL && codex_items_json[0] != '\0';

    if (profile == NULL || instructions == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "\"model\":");
    used = append_escaped_json(buffer, buffer_size, used, profile->model);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"instructions\":");
    used = append_escaped_json(buffer, buffer_size, used, instructions);
    used += (size_t)snprintf(
        buffer + used,
        buffer_size - used,
        ",\"store\":false,\"stream\":true,\"parallel_tool_calls\":false,\"input\":["
    );
    if (has_history) {
        used = append_history_items(buffer, buffer_size, used, history, history_count);
    }
    if (has_codex_items) {
        if (has_history) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        used += (size_t)snprintf(buffer + used, buffer_size - used, "%s", codex_items_json);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]");
    used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    return 0;
}

static void append_text_segment(char *buffer, size_t buffer_size, const char *value)
{
    size_t used = strlen(buffer);

    if (used >= buffer_size - 1) {
        return;
    }
    snprintf(buffer + used, buffer_size - used, "%s", value != NULL ? value : "");
}

static int parse_provider_response(const char *json, espclaw_provider_response_t *response)
{
    const char *cursor;

    if (json == NULL || response == NULL) {
        return -1;
    }

    memset(response, 0, sizeof(*response));
    extract_json_string_from(json, "id", NULL, response->id, sizeof(response->id));

    cursor = json;
    while ((cursor = strstr(cursor, "\"type\":\"output_text\"")) != NULL) {
        char text[1024];

        if (extract_json_string_from(json, "text", cursor, text, sizeof(text))) {
            append_text_segment(response->text, sizeof(response->text), text);
        }
        cursor += strlen("\"type\":\"output_text\"");
    }

    cursor = json;
    while ((cursor = strstr(cursor, "\"type\":\"function_call\"")) != NULL &&
           response->tool_call_count < ESPCLAW_AGENT_TOOL_CALL_MAX) {
        espclaw_agent_tool_call_t *tool_call = &response->tool_calls[response->tool_call_count];
        char wire_name[sizeof(tool_call->name)];

        if (extract_json_string_from(json, "call_id", cursor, tool_call->call_id, sizeof(tool_call->call_id)) &&
            extract_json_string_from(json, "name", cursor, wire_name, sizeof(wire_name)) &&
            extract_json_string_from(json, "arguments", cursor, tool_call->arguments_json, sizeof(tool_call->arguments_json))) {
            decode_tool_name(wire_name, tool_call->name, sizeof(tool_call->name));
            response->tool_call_count++;
        }
        cursor += strlen("\"type\":\"function_call\"");
    }

    return response->id[0] != '\0' ? 0 : -1;
}

static bool json_argument_string(const char *arguments_json, const char *key, char *buffer, size_t buffer_size)
{
    return extract_json_string_from(arguments_json, key, NULL, buffer, buffer_size);
}

static bool json_argument_int(const char *arguments_json, const char *key, int *value_out)
{
    const char *cursor = find_json_key_after(arguments_json, key, NULL);
    char *end_ptr = NULL;
    long parsed = 0;

    if (cursor == NULL || value_out == NULL) {
        return false;
    }

    cursor = strchr(cursor, ':');
    if (cursor == NULL) {
        return false;
    }
    cursor++;
    while (*cursor != '\0' && isspace((unsigned char)*cursor)) {
        cursor++;
    }

    parsed = strtol(cursor, &end_ptr, 10);
    if (end_ptr == cursor) {
        return false;
    }

    *value_out = (int)parsed;
    return true;
}

static int tool_fs_read(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char path[256];
    char content[1536];

    if (!json_argument_string(arguments_json, "path", path, sizeof(path))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing path\"}");
        return -1;
    }
    if (espclaw_workspace_read_file(workspace_root, path, content, sizeof(content)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"failed to read file\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"path\":\"%s\",\"content\":", path);
    append_escaped_json(buffer, buffer_size, strlen(buffer), content);
    append_text_segment(buffer, buffer_size, "}");
    return 0;
}

static int tool_fs_list(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char relative_path[256];
    char absolute_path[512];
    DIR *dir;
    struct dirent *entry;
    size_t used = 0;
    bool first = true;

    if (!json_argument_string(arguments_json, "path", relative_path, sizeof(relative_path))) {
        copy_text(relative_path, sizeof(relative_path), ".");
    }
    if (strcmp(relative_path, ".") == 0) {
        copy_text(absolute_path, sizeof(absolute_path), workspace_root);
    } else if (espclaw_workspace_resolve_path(workspace_root, relative_path, absolute_path, sizeof(absolute_path)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"invalid path\"}");
        return -1;
    }

    dir = opendir(absolute_path);
    if (dir == NULL) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"failed to open directory\"}");
        return -1;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ok\":true,\"path\":");
    used = append_escaped_json(buffer, buffer_size, used, relative_path);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"entries\":[");

    while ((entry = readdir(dir)) != NULL && used + 8 < buffer_size) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        if (!first) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        used = append_escaped_json(buffer, buffer_size, used, entry->d_name);
        first = false;
    }

    closedir(dir);
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    return 0;
}

static int tool_system_info(const char *workspace_root, char *buffer, size_t buffer_size)
{
    struct stat statbuf;
    char apps_json[1024];
    bool workspace_ready = workspace_root != NULL && stat(workspace_root, &statbuf) == 0;
    const char *storage_backend = espclaw_storage_describe_workspace_root(workspace_root);

    espclaw_render_apps_json(workspace_root, apps_json, sizeof(apps_json));
    snprintf(
        buffer,
        buffer_size,
        "{\"ok\":true,\"workspace_ready\":%s,\"workspace_root\":\"%s\",\"storage_backend\":\"%s\",\"tool_count\":%u,\"apps\":%s}",
        workspace_ready ? "true" : "false",
        workspace_root != NULL ? workspace_root : "",
        storage_backend,
        (unsigned)espclaw_tool_count(),
        apps_json
    );
    return 0;
}

static int tool_list_tools(char *buffer, size_t buffer_size)
{
    espclaw_render_tools_json(buffer, buffer_size);
    return 0;
}

static int tool_app_list(const char *workspace_root, char *buffer, size_t buffer_size)
{
    espclaw_render_apps_json(workspace_root, buffer, buffer_size);
    return 0;
}

static int tool_app_run(const char *workspace_root, const char *arguments_json, char *buffer, size_t buffer_size)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char trigger[ESPCLAW_APP_TRIGGER_NAME_MAX + 1];
    char payload[512];
    char result[1024];

    if (!json_argument_string(arguments_json, "app_id", app_id, sizeof(app_id))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing app_id\"}");
        return -1;
    }
    if (!json_argument_string(arguments_json, "trigger", trigger, sizeof(trigger))) {
        copy_text(trigger, sizeof(trigger), "manual");
    }
    if (!json_argument_string(arguments_json, "payload", payload, sizeof(payload))) {
        payload[0] = '\0';
    }

    if (espclaw_app_run(workspace_root, app_id, trigger, payload, result, sizeof(result)) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"app run failed\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"app_id\":\"%s\",\"result\":", app_id);
    append_escaped_json(buffer, buffer_size, strlen(buffer), result);
    append_text_segment(buffer, buffer_size, "}");
    return 0;
}

static int tool_uart_read(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int port = 0;
    int length = 256;
    uint8_t data[512];
    size_t bytes_read = 0;

    (void)json_argument_int(arguments_json, "port", &port);
    if (json_argument_int(arguments_json, "length", &length) && (length <= 0 || length > (int)sizeof(data))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"invalid length\"}");
        return -1;
    }
    if (!json_argument_int(arguments_json, "length", &length)) {
        length = 256;
    }
    if (espclaw_hw_uart_read(port, data, (size_t)length, &bytes_read) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"uart read failed\"}");
        return -1;
    }

    data[bytes_read < sizeof(data) ? bytes_read : sizeof(data) - 1] = '\0';
    snprintf(buffer, buffer_size, "{\"ok\":true,\"port\":%d,\"bytes_read\":%u,\"data\":", port, (unsigned)bytes_read);
    append_escaped_json(buffer, buffer_size, strlen(buffer), (const char *)data);
    append_text_segment(buffer, buffer_size, "}");
    return 0;
}

static int tool_uart_write(const char *arguments_json, char *buffer, size_t buffer_size)
{
    int port = 0;
    char data[512];
    size_t bytes_written = 0;

    if (!json_argument_int(arguments_json, "port", &port)) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing port\"}");
        return -1;
    }
    if (!json_argument_string(arguments_json, "data", data, sizeof(data))) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"missing data\"}");
        return -1;
    }
    if (espclaw_hw_uart_write(port, (const uint8_t *)data, strlen(data), &bytes_written) != 0) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"uart write failed\"}");
        return -1;
    }

    snprintf(buffer, buffer_size, "{\"ok\":true,\"port\":%d,\"bytes_written\":%u}", port, (unsigned)bytes_written);
    return 0;
}

static int tool_execute(
    const char *workspace_root,
    const espclaw_agent_tool_call_t *tool_call,
    bool allow_mutations,
    char *buffer,
    size_t buffer_size
)
{
    if (tool_call == NULL || buffer == NULL || buffer_size == 0) {
        return -1;
    }

    if (espclaw_tool_requires_confirmation(tool_call->name) && !allow_mutations) {
        snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"confirmation_required\"}");
        return -1;
    }

    if (strcmp(tool_call->name, "fs.read") == 0) {
        return tool_fs_read(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "fs.list") == 0) {
        return tool_fs_list(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "system.info") == 0) {
        return tool_system_info(workspace_root, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "tool.list") == 0) {
        return tool_list_tools(buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "wifi.status") == 0) {
        snprintf(buffer, buffer_size, "{\"ok\":true,\"connected\":false,\"mode\":\"simulated\"}");
        return 0;
    }
    if (strcmp(tool_call->name, "app.list") == 0) {
        return tool_app_list(workspace_root, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "app.run") == 0) {
        return tool_app_run(workspace_root, tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "uart.read") == 0) {
        return tool_uart_read(tool_call->arguments_json, buffer, buffer_size);
    }
    if (strcmp(tool_call->name, "uart.write") == 0) {
        return tool_uart_write(tool_call->arguments_json, buffer, buffer_size);
    }

    snprintf(buffer, buffer_size, "{\"ok\":false,\"error\":\"unsupported_tool\"}");
    return -1;
}

#ifdef ESPCLAW_HOST_LUA
static int mock_transport(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size
)
{
    (void)profile;

    if (strncmp(url, "mock://", 7) != 0) {
        return -1;
    }

    if (strstr(url, "tool-loop") != NULL) {
        if (user_message_requests_tool_listing(body)) {
            if (strstr(body, "\"type\":\"function_call_output\"") != NULL || strstr(body, "\"previous_response_id\"") != NULL) {
                snprintf(
                    response,
                    response_size,
                    "{"
                    "\"id\":\"resp_mock_tools_final\","
                    "\"status\":\"completed\","
                    "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"I can inspect files, apps, hardware, networking, and OTA state.\"}]}]"
                    "}"
                );
                return 0;
            }

            snprintf(
                response,
                response_size,
                "{"
                "\"id\":\"resp_mock_tools_first\","
                "\"status\":\"completed\","
                "\"output\":["
                "{\"type\":\"function_call\",\"call_id\":\"call_tool_list\",\"name\":\"tool_x2E_list\",\"arguments\":\"{}\",\"status\":\"completed\"}"
                "]"
                "}"
            );
            return 0;
        }

        if (strstr(body, "\"type\":\"function_call_output\"") != NULL || strstr(body, "\"previous_response_id\"") != NULL) {
            snprintf(
                response,
                response_size,
                "{"
                "\"id\":\"resp_mock_final\","
                "\"status\":\"completed\","
                "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"I checked the device and listed the installed apps.\"}]}]"
                "}"
            );
            return 0;
        }

        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_mock_first\","
            "\"status\":\"completed\","
            "\"output\":["
            "{\"type\":\"function_call\",\"call_id\":\"call_sys\",\"name\":\"system.info\",\"arguments\":\"{}\",\"status\":\"completed\"},"
            "{\"type\":\"function_call\",\"call_id\":\"call_apps\",\"name\":\"app.list\",\"arguments\":\"{}\",\"status\":\"completed\"}"
            "]"
            "}"
        );
        return 0;
    }

    if (strstr(url, "fs-read") != NULL) {
        if (strstr(body, "\"type\":\"function_call_output\"") != NULL || strstr(body, "\"previous_response_id\"") != NULL) {
            snprintf(
                response,
                response_size,
                "{"
                "\"id\":\"resp_mock_fs_final\","
                "\"status\":\"completed\","
                "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"I read the requested file and summarized it.\"}]}]"
                "}"
            );
            return 0;
        }

        snprintf(
            response,
            response_size,
            "{"
            "\"id\":\"resp_mock_fs_first\","
            "\"status\":\"completed\","
            "\"output\":["
            "{\"type\":\"function_call\",\"call_id\":\"call_read\",\"name\":\"fs.read\",\"arguments\":\"{\\\"path\\\":\\\"AGENTS.md\\\"}\",\"status\":\"completed\"}"
            "]"
            "}"
        );
        return 0;
    }

    snprintf(
        response,
        response_size,
        "{"
        "\"id\":\"resp_mock_text\","
        "\"status\":\"completed\","
        "\"output\":[{\"type\":\"message\",\"role\":\"assistant\",\"content\":[{\"type\":\"output_text\",\"text\":\"Mock provider reply.\"}]}]"
        "}"
    );
    return 0;
}

static int host_http_post(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size
)
{
    char body_path[] = "/tmp/espclaw-body-XXXXXX";
    char command[4096];
    FILE *body_file;
    FILE *pipe;
    int fd;
    size_t total_read = 0;
    char discard[1024];
    bool is_codex = profile != NULL && strcmp(profile->provider_id, "openai_codex") == 0;

    if (strncmp(url, "mock://", 7) == 0) {
        return mock_transport(url, profile, body, response, response_size);
    }

    fd = mkstemp(body_path);
    if (fd < 0) {
        return -1;
    }
    body_file = fdopen(fd, "w");
    if (body_file == NULL) {
        close(fd);
        unlink(body_path);
        return -1;
    }
    fputs(body, body_file);
    fclose(body_file);

    if (is_codex && profile->account_id[0] != '\0') {
        snprintf(
            command,
            sizeof(command),
            "curl -sS -N -X POST '%s/responses' "
            "-H 'Content-Type: application/json' "
            "-H 'Authorization: Bearer %s' "
            "-H 'originator: codex_cli_rs' "
            "-H 'OpenAI-Beta: responses=experimental' "
            "-H 'Chatgpt-Account-Id: %s' "
            "--data-binary @%s 2>/dev/null",
            url,
            profile->access_token,
            profile->account_id,
            body_path
        );
    } else if (is_codex) {
        snprintf(
            command,
            sizeof(command),
            "curl -sS -N -X POST '%s/responses' "
            "-H 'Content-Type: application/json' "
            "-H 'Authorization: Bearer %s' "
            "-H 'originator: codex_cli_rs' "
            "-H 'OpenAI-Beta: responses=experimental' "
            "--data-binary @%s 2>/dev/null",
            url,
            profile->access_token,
            body_path
        );
    } else {
        snprintf(
            command,
            sizeof(command),
            "curl -sS -X POST '%s/responses' "
            "-H 'Content-Type: application/json' "
            "-H 'Authorization: Bearer %s' "
            "--data-binary @%s 2>/dev/null",
            url,
            profile->access_token,
            body_path
        );
    }

    pipe = popen(command, "r");
    if (pipe == NULL) {
        unlink(body_path);
        return -1;
    }
    while (total_read + 1 < response_size) {
        size_t n = fread(response + total_read, 1, response_size - 1 - total_read, pipe);
        if (n == 0) {
            break;
        }
        total_read += n;
    }
    while (fread(discard, 1, sizeof(discard), pipe) > 0) {
    }
    response[total_read] = '\0';
    pclose(pipe);
    unlink(body_path);
    if (is_codex && total_read > 0) {
        char completed[ESPCLAW_AGENT_HTTP_BUFFER_MAX];

        if (extract_sse_completed_response_json(response, completed, sizeof(completed)) == 0) {
            copy_text(response, response_size, completed);
            total_read = strlen(response);
        }
    }
    return total_read > 0 ? 0 : -1;
}
#else
static int host_http_post(
    const char *url,
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size
)
{
    esp_http_client_config_t config = {
        .url = url,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .timeout_ms = 30000,
    };
    esp_http_client_handle_t client;
    char authorization[ESPCLAW_AUTH_TOKEN_MAX + 16];
    int total_read = 0;
    char endpoint[256];
    bool is_codex = profile != NULL && strcmp(profile->provider_id, "openai_codex") == 0;

    snprintf(endpoint, sizeof(endpoint), "%s/responses", url);
    config.url = endpoint;
    client = esp_http_client_init(&config);
    if (client == NULL) {
        return -1;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    snprintf(authorization, sizeof(authorization), "Bearer %s", profile->access_token);
    esp_http_client_set_header(client, "Authorization", authorization);
    if (is_codex) {
        esp_http_client_set_header(client, "originator", "codex_cli_rs");
        esp_http_client_set_header(client, "OpenAI-Beta", "responses=experimental");
    }
    if (is_codex && profile->account_id[0] != '\0') {
        esp_http_client_set_header(client, "Chatgpt-Account-Id", profile->account_id);
    }
    esp_http_client_set_post_field(client, body, (int)strlen(body));
    if (esp_http_client_perform(client) != 0) {
        esp_http_client_cleanup(client);
        return -1;
    }
    while (total_read < (int)response_size - 1) {
        int read_now = esp_http_client_read(client, response + total_read, (int)response_size - 1 - total_read);
        if (read_now <= 0) {
            break;
        }
        total_read += read_now;
    }
    response[total_read] = '\0';
    esp_http_client_cleanup(client);
    if (is_codex && total_read > 0) {
        char completed[ESPCLAW_AGENT_HTTP_BUFFER_MAX];

        if (extract_sse_completed_response_json(response, completed, sizeof(completed)) == 0) {
            copy_text(response, response_size, completed);
            total_read = (int)strlen(response);
        }
    }
    return total_read > 0 ? 0 : -1;
}
#endif

static int call_provider(
    const espclaw_auth_profile_t *profile,
    const char *body,
    char *response,
    size_t response_size
)
{
    if (s_http_adapter != NULL) {
        return s_http_adapter(profile->base_url, profile, body, response, response_size, s_http_adapter_user_data);
    }
    return host_http_post(profile->base_url, profile, body, response, response_size);
}

static void build_tool_summary(
    const espclaw_provider_response_t *provider_response,
    char *buffer,
    size_t buffer_size
)
{
    size_t index;
    size_t used = 0;

    if (buffer == NULL || buffer_size == 0 || provider_response == NULL) {
        return;
    }

    used += (size_t)snprintf(buffer + used, buffer_size - used, "Requested tools:");
    for (index = 0; index < provider_response->tool_call_count; ++index) {
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            "%s %s",
            index == 0 ? "" : ",",
            provider_response->tool_calls[index].name
        );
    }
}

int espclaw_agent_loop_run(
    const char *workspace_root,
    const char *session_id,
    const char *user_message,
    bool allow_mutations,
    espclaw_agent_run_result_t *result
)
{
    espclaw_auth_profile_t profile;
    espclaw_history_message_t history[ESPCLAW_AGENT_HISTORY_MAX];
    char *request_body = NULL;
    char *response_body = NULL;
    char codex_items_json[16384];
    char instructions[6144];
    char previous_response_id[ESPCLAW_AGENT_RESPONSE_ID_MAX + 1];
    size_t history_count = 0;
    unsigned int iteration = 0;

    if (workspace_root == NULL || session_id == NULL || user_message == NULL || result == NULL) {
        return -1;
    }

    memset(result, 0, sizeof(*result));
    if (espclaw_auth_store_load(&profile) != 0 || !espclaw_auth_profile_is_ready(&profile)) {
        copy_text(result->final_text, sizeof(result->final_text), "Provider credentials are not configured.");
        return -1;
    }

    request_body = (char *)calloc(1, ESPCLAW_AGENT_HTTP_BUFFER_MAX);
    response_body = (char *)calloc(1, ESPCLAW_AGENT_HTTP_BUFFER_MAX);
    if (request_body == NULL || response_body == NULL) {
        free(request_body);
        free(response_body);
        copy_text(result->final_text, sizeof(result->final_text), "Out of memory building model request.");
        return -1;
    }

    if (load_system_prompt(workspace_root, instructions, sizeof(instructions)) != 0) {
        free(request_body);
        free(response_body);
        copy_text(result->final_text, sizeof(result->final_text), "Failed to load system prompt.");
        return -1;
    }

    codex_items_json[0] = '\0';
    espclaw_session_append_message(workspace_root, session_id, "user", user_message);
    load_history(workspace_root, session_id, history, ESPCLAW_AGENT_HISTORY_MAX, &history_count);
    if (build_initial_request_body(&profile, instructions, history, history_count, request_body, ESPCLAW_AGENT_HTTP_BUFFER_MAX) != 0) {
        free(request_body);
        free(response_body);
        copy_text(result->final_text, sizeof(result->final_text), "Failed to build the model request.");
        return -1;
    }

    previous_response_id[0] = '\0';
    while (iteration < 8) {
        espclaw_provider_response_t provider_response;
        char tool_results[ESPCLAW_AGENT_TOOL_CALL_MAX][1024];
        size_t tool_index;

        memset(response_body, 0, ESPCLAW_AGENT_HTTP_BUFFER_MAX);
        if (call_provider(&profile, request_body, response_body, ESPCLAW_AGENT_HTTP_BUFFER_MAX) != 0 ||
            parse_provider_response(response_body, &provider_response) != 0) {
            free(request_body);
            free(response_body);
            copy_text(result->final_text, sizeof(result->final_text), "Model call failed.");
            return -1;
        }

        copy_text(previous_response_id, sizeof(previous_response_id), provider_response.id);
        copy_text(result->response_id, sizeof(result->response_id), provider_response.id);
        iteration++;
        result->iterations = iteration;

        if (provider_response.tool_call_count == 0) {
            result->ok = true;
            copy_text(result->final_text, sizeof(result->final_text), provider_response.text);
            espclaw_session_append_message(workspace_root, session_id, "assistant", result->final_text);
            free(request_body);
            free(response_body);
            return 0;
        }

        result->used_tools = true;
        if (provider_response.text[0] != '\0') {
            espclaw_session_append_message(workspace_root, session_id, "assistant", provider_response.text);
        } else {
            char tool_summary[256];

            build_tool_summary(&provider_response, tool_summary, sizeof(tool_summary));
            espclaw_session_append_message(workspace_root, session_id, "assistant", tool_summary);
        }

        for (tool_index = 0; tool_index < provider_response.tool_call_count; ++tool_index) {
            tool_execute(
                workspace_root,
                &provider_response.tool_calls[tool_index],
                allow_mutations,
                tool_results[tool_index],
                sizeof(tool_results[tool_index])
            );
            espclaw_session_append_message(workspace_root, session_id, "tool", tool_results[tool_index]);
        }

        memset(request_body, 0, ESPCLAW_AGENT_HTTP_BUFFER_MAX);
        if (strcmp(profile.provider_id, "openai_codex") == 0) {
            size_t codex_items_used = strlen(codex_items_json);

            codex_items_used = append_codex_tool_round_items(
                codex_items_json,
                sizeof(codex_items_json),
                codex_items_used,
                provider_response.tool_calls,
                tool_results,
                provider_response.tool_call_count
            );
            if (build_codex_followup_request_body(
                    &profile,
                    instructions,
                    history,
                    history_count,
                    codex_items_json,
                    request_body,
                    ESPCLAW_AGENT_HTTP_BUFFER_MAX) != 0) {
                free(request_body);
                free(response_body);
                copy_text(result->final_text, sizeof(result->final_text), "Failed to continue after tool execution.");
                return -1;
            }
        } else if (build_followup_request_body(
                       &profile,
                       instructions,
                       previous_response_id,
                       provider_response.tool_calls,
                       tool_results,
                       provider_response.tool_call_count,
                       request_body,
                       ESPCLAW_AGENT_HTTP_BUFFER_MAX) != 0) {
            free(request_body);
            free(response_body);
            copy_text(result->final_text, sizeof(result->final_text), "Failed to continue after tool execution.");
            return -1;
        }
    }

    result->hit_iteration_limit = true;
    copy_text(result->final_text, sizeof(result->final_text), "Agent run hit the tool iteration limit.");
    espclaw_session_append_message(workspace_root, session_id, "assistant", result->final_text);
    free(request_body);
    free(response_body);
    return -1;
}

void espclaw_agent_set_http_adapter(espclaw_agent_http_adapter_t adapter, void *user_data)
{
    s_http_adapter = adapter;
    s_http_adapter_user_data = user_data;
}
