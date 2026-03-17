#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <unistd.h>

#include "espclaw/admin_api.h"
#include "espclaw/agent_loop.h"
#include "espclaw/admin_ops.h"
#include "espclaw/admin_ui.h"
#include "espclaw/app_runtime.h"
#include "espclaw/auth_store.h"
#include "espclaw/behavior_runtime.h"
#include "espclaw/board_config.h"
#include "espclaw/board_profile.h"
#include "espclaw/control_loop.h"
#include "espclaw/event_watch.h"
#include "espclaw/ota_state.h"
#include "espclaw/provisioning.h"
#include "espclaw/system_monitor.h"
#include "espclaw/task_policy.h"
#include "espclaw/task_runtime.h"
#include "espclaw/workspace.h"

#define ESPCLAW_SIM_REQUEST_MAX 65536
#define ESPCLAW_SIM_RESPONSE_MAX 16384

typedef struct {
    espclaw_board_profile_t profile;
    char workspace_root[512];
    int port;
} espclaw_simulator_config_t;

typedef struct {
    char ssid[33];
    int rssi;
    int channel;
    bool secure;
} espclaw_sim_wifi_network_t;

static volatile sig_atomic_t s_should_stop = 0;
static bool s_sim_wifi_ready = false;
static bool s_sim_wifi_provisioning_active = true;
static char s_sim_wifi_ssid[33];
static char s_sim_onboarding_ssid[33];
static const espclaw_sim_wifi_network_t s_sim_wifi_networks[] = {
    {.ssid = "ESPClawLab", .rssi = -38, .channel = 1, .secure = true},
    {.ssid = "Attila-5G", .rssi = -52, .channel = 36, .secure = true},
    {.ssid = "FieldRouter", .rssi = -67, .channel = 11, .secure = false},
};

static size_t append_escaped_json(char *buffer, size_t buffer_size, size_t used, const char *value);

static void describe_sim_provisioning(
    const espclaw_simulator_config_t *config,
    espclaw_provisioning_descriptor_t *descriptor
)
{
    const char *transport = "softap";
    const char *pop = "";
    const char *admin_url = "";

    if (config != NULL && strcmp(config->profile.provisioning, "ble") == 0) {
        transport = "ble";
        pop = "espclaw-pass";
    } else if (s_sim_wifi_provisioning_active) {
        admin_url = "http://192.168.4.1/";
    }

    if (!s_sim_wifi_provisioning_active) {
        transport = "";
    }
    if (strcmp(transport, "softap") == 0 && s_sim_wifi_provisioning_active) {
        admin_url = "http://192.168.4.1/";
    }

    espclaw_provisioning_build_descriptor(
        s_sim_wifi_provisioning_active,
        transport,
        s_sim_onboarding_ssid,
        "",
        pop,
        admin_url,
        descriptor
    );
}

static void handle_signal(int signum)
{
    (void)signum;
    s_should_stop = 1;
}

static void print_usage(const char *argv0)
{
    fprintf(
        stderr,
        "Usage: %s [--workspace PATH] [--port PORT] [--profile esp32s3|esp32cam] [--self-test]\n",
        argv0
    );
}

static void send_http_response(int client_fd, int status_code, const char *status_text, const char *content_type, const char *body)
{
    char header[512];
    size_t body_length = body != NULL ? strlen(body) : 0;
    int header_length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code,
        status_text,
        content_type,
        body_length
    );

    if (header_length > 0) {
        send(client_fd, header, (size_t)header_length, 0);
    }
    if (body_length > 0) {
        send(client_fd, body, body_length, 0);
    }
}

static const char *media_content_type(const char *relative_path)
{
    const char *extension;

    if (relative_path == NULL) {
        return "application/octet-stream";
    }
    extension = strrchr(relative_path, '.');
    if (extension == NULL) {
        return "application/octet-stream";
    }
    if (strcasecmp(extension, ".jpg") == 0 || strcasecmp(extension, ".jpeg") == 0) {
        return "image/jpeg";
    }
    if (strcasecmp(extension, ".png") == 0) {
        return "image/png";
    }
    if (strcasecmp(extension, ".gif") == 0) {
        return "image/gif";
    }
    if (strcasecmp(extension, ".webp") == 0) {
        return "image/webp";
    }
    if (strcasecmp(extension, ".txt") == 0 || strcasecmp(extension, ".log") == 0) {
        return "text/plain; charset=utf-8";
    }
    if (strcasecmp(extension, ".json") == 0) {
        return "application/json";
    }
    return "application/octet-stream";
}

static void send_http_file_response(int client_fd, int status_code, const char *status_text, const char *content_type, const char *absolute_path)
{
    FILE *file = NULL;
    char header[512];
    char chunk[2048];
    struct stat file_stat;
    int header_length;

    if (absolute_path == NULL || stat(absolute_path, &file_stat) != 0 || !S_ISREG(file_stat.st_mode)) {
        send_http_response(client_fd, 404, "Not Found", "application/json", "{\"ok\":false,\"message\":\"media file not found\"}");
        return;
    }

    file = fopen(absolute_path, "rb");
    if (file == NULL) {
        send_http_response(client_fd, 500, "Internal Server Error", "application/json", "{\"ok\":false,\"message\":\"failed to open media file\"}");
        return;
    }

    header_length = snprintf(
        header,
        sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Cache-Control: no-store\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code,
        status_text,
        content_type,
        (size_t)file_stat.st_size
    );
    if (header_length > 0) {
        send(client_fd, header, (size_t)header_length, 0);
    }
    while (!feof(file)) {
        size_t read_size = fread(chunk, 1, sizeof(chunk), file);

        if (read_size == 0) {
            break;
        }
        send(client_fd, chunk, read_size, 0);
    }
    fclose(file);
}

static bool parse_request(
    int client_fd,
    char *method,
    size_t method_size,
    char *target,
    size_t target_size,
    char *body,
    size_t body_size
)
{
    char request[ESPCLAW_SIM_REQUEST_MAX];
    ssize_t total = 0;
    char *header_end = NULL;
    size_t content_length = 0;

    while (total < (ssize_t)sizeof(request) - 1) {
        ssize_t received = recv(client_fd, request + total, sizeof(request) - 1 - (size_t)total, 0);
        if (received <= 0) {
            return false;
        }
        total += received;
        request[total] = '\0';

        if (header_end == NULL) {
            header_end = strstr(request, "\r\n\r\n");
            if (header_end != NULL) {
                char *content_length_header = strstr(request, "Content-Length:");

                if (content_length_header != NULL && content_length_header < header_end) {
                    content_length = (size_t)strtoul(content_length_header + 15, NULL, 10);
                }
            }
        }

        if (header_end != NULL) {
            size_t header_size = (size_t)(header_end + 4 - request);
            if ((size_t)total >= header_size + content_length) {
                break;
            }
        }
    }

    if (sscanf(request, "%7s %1023s", method, target) != 2) {
        return false;
    }

    if (body != NULL && body_size > 0) {
        body[0] = '\0';
        if (header_end != NULL && content_length > 0) {
            size_t header_size = (size_t)(header_end + 4 - request);
            size_t copy_length = content_length;

            if (copy_length >= body_size) {
                copy_length = body_size - 1;
            }
            memcpy(body, request + header_size, copy_length);
            body[copy_length] = '\0';
        }
    }

    return true;
}

static void split_target(char *target, char **path_out, char **query_out)
{
    char *query = strchr(target, '?');

    *path_out = target;
    *query_out = NULL;
    if (query != NULL) {
        *query = '\0';
        *query_out = query + 1;
    }
}

static size_t render_sim_wifi_status_json(
    const espclaw_simulator_config_t *config,
    char *buffer,
    size_t buffer_size,
    const char *message
)
{
    size_t used;
    espclaw_provisioning_descriptor_t descriptor;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }
    espclaw_provisioning_descriptor_init(&descriptor);
    describe_sim_provisioning(config, &descriptor);

    used = (size_t)snprintf(
        buffer,
        buffer_size,
        "{\"ok\":true,\"wifi_ready\":%s,\"provisioning_active\":%s,\"provisioning_transport\":",
        s_sim_wifi_ready ? "true" : "false",
        s_sim_wifi_provisioning_active ? "true" : "false"
    );
    used = append_escaped_json(buffer, buffer_size, used, descriptor.transport);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"ssid\":");
    used = append_escaped_json(buffer, buffer_size, used, s_sim_wifi_ssid);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"onboarding_ssid\":");
    used = append_escaped_json(buffer, buffer_size, used, s_sim_onboarding_ssid);
    used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"admin_url\":");
    used = append_escaped_json(buffer, buffer_size, used, descriptor.admin_url);
    if (message != NULL && message[0] != '\0') {
        used += (size_t)snprintf(buffer + used, buffer_size - used, ",\"message\":");
        used = append_escaped_json(buffer, buffer_size, used, message);
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "}");
    return used >= buffer_size ? buffer_size - 1 : used;
}

static size_t render_sim_wifi_scan_json(char *buffer, size_t buffer_size)
{
    size_t used = 0;
    size_t index;

    if (buffer == NULL || buffer_size == 0) {
        return 0;
    }

    used = (size_t)snprintf(buffer, buffer_size, "{\"ok\":true,\"networks\":[");
    for (index = 0; index < sizeof(s_sim_wifi_networks) / sizeof(s_sim_wifi_networks[0]); ++index) {
        if (index > 0) {
            used += (size_t)snprintf(buffer + used, buffer_size - used, ",");
        }
        used += (size_t)snprintf(buffer + used, buffer_size - used, "{\"ssid\":");
        used = append_escaped_json(buffer, buffer_size, used, s_sim_wifi_networks[index].ssid);
        used += (size_t)snprintf(
            buffer + used,
            buffer_size - used,
            ",\"rssi\":%d,\"channel\":%d,\"secure\":%s}",
            s_sim_wifi_networks[index].rssi,
            s_sim_wifi_networks[index].channel,
            s_sim_wifi_networks[index].secure ? "true" : "false"
        );
    }
    used += (size_t)snprintf(buffer + used, buffer_size - used, "]}");
    return used >= buffer_size ? buffer_size - 1 : used;
}

static uint32_t query_u32_or_default(const char *query, const char *key, uint32_t fallback)
{
    char value[32];

    if (query != NULL && espclaw_admin_query_value(query, key, value, sizeof(value))) {
        return (uint32_t)strtoul(value, NULL, 10);
    }

    return fallback;
}

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

static void merge_auth_profile_from_body(espclaw_auth_profile_t *profile, const char *body)
{
    long expires_at = 0;
    char value[ESPCLAW_AUTH_TOKEN_MAX + 1];

    if (profile == NULL || body == NULL) {
        return;
    }

    if (espclaw_admin_json_string_value(body, "provider_id", value, sizeof(value))) {
        copy_text(profile->provider_id, sizeof(profile->provider_id), value);
    }
    if (espclaw_admin_json_string_value(body, "model", value, sizeof(value))) {
        copy_text(profile->model, sizeof(profile->model), value);
    }
    if (espclaw_admin_json_string_value(body, "base_url", value, sizeof(value))) {
        copy_text(profile->base_url, sizeof(profile->base_url), value);
    }
    if (espclaw_admin_json_string_value(body, "access_token", value, sizeof(value))) {
        copy_text(profile->access_token, sizeof(profile->access_token), value);
    }
    if (espclaw_admin_json_string_value(body, "refresh_token", value, sizeof(value))) {
        copy_text(profile->refresh_token, sizeof(profile->refresh_token), value);
    }
    if (espclaw_admin_json_string_value(body, "account_id", value, sizeof(value))) {
        copy_text(profile->account_id, sizeof(profile->account_id), value);
    }
    if (espclaw_admin_json_string_value(body, "source", value, sizeof(value))) {
        copy_text(profile->source, sizeof(profile->source), value);
    }
    if (espclaw_admin_json_long_value(body, "expires_at", &expires_at)) {
        profile->expires_at = expires_at;
    }
    profile->configured = profile->access_token[0] != '\0';
    if (profile->source[0] == '\0') {
        copy_text(profile->source, sizeof(profile->source), "admin");
    }
}

static void handle_api_request(
    int client_fd,
    const espclaw_simulator_config_t *config,
    const char *method,
    const char *path,
    const char *query,
    const char *body
)
{
    char response[ESPCLAW_SIM_RESPONSE_MAX];
    espclaw_ota_state_t ota_state = espclaw_ota_state_init();

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) {
        espclaw_auth_profile_t profile;

        espclaw_auth_profile_default(&profile);
        espclaw_auth_store_load(&profile);
        espclaw_render_admin_status_json(
            &config->profile,
            config->profile.default_storage_backend,
            profile.provider_id,
            "telegram",
            true,
            &ota_state,
            response,
            sizeof(response)
        );
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/board") == 0) {
        espclaw_render_board_json(espclaw_board_current(), response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/board/presets") == 0) {
        espclaw_render_board_presets_json(&config->profile, response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/board/config") == 0) {
        espclaw_render_board_config_json(
            config->workspace_root,
            &config->profile,
            espclaw_board_current(),
            response,
            sizeof(response)
        );
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "PUT") == 0 && strcmp(path, "/api/board/config") == 0) {
        if (espclaw_workspace_write_file(config->workspace_root, "config/board.json", body != NULL ? body : "") != 0 ||
            espclaw_board_configure_current(config->workspace_root, &config->profile) != 0) {
            espclaw_admin_render_result_json(false, "failed to save board config", response, sizeof(response));
            send_http_response(client_fd, 500, "Internal Server Error", "application/json", response);
            return;
        }
        espclaw_render_board_config_json(
            config->workspace_root,
            &config->profile,
            espclaw_board_current(),
            response,
            sizeof(response)
        );
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/board/apply") == 0) {
        char variant_id[ESPCLAW_BOARD_VARIANT_MAX + 1];

        if (!espclaw_admin_query_value(query, "variant_id", variant_id, sizeof(variant_id))) {
            espclaw_admin_render_result_json(false, "missing variant_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }
        if (espclaw_board_write_variant_config(config->workspace_root, variant_id) != 0 ||
            espclaw_board_configure_current(config->workspace_root, &config->profile) != 0) {
            espclaw_admin_render_result_json(false, "failed to apply board preset", response, sizeof(response));
            send_http_response(client_fd, 500, "Internal Server Error", "application/json", response);
            return;
        }
        espclaw_render_board_json(espclaw_board_current(), response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/network/status") == 0) {
        render_sim_wifi_status_json(config, response, sizeof(response), NULL);
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/network/provisioning") == 0) {
        espclaw_provisioning_descriptor_t descriptor;

        espclaw_provisioning_descriptor_init(&descriptor);
        describe_sim_provisioning(config, &descriptor);
        espclaw_provisioning_render_json(&descriptor, response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/network/scan") == 0) {
        render_sim_wifi_scan_json(response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/network/join") == 0) {
        char ssid[64];

        if (!espclaw_admin_json_string_value(body != NULL ? body : "", "ssid", ssid, sizeof(ssid))) {
            espclaw_admin_render_result_json(false, "missing ssid", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }

        snprintf(s_sim_wifi_ssid, sizeof(s_sim_wifi_ssid), "%s", ssid);
        s_sim_wifi_ready = true;
        s_sim_wifi_provisioning_active = false;
        s_sim_onboarding_ssid[0] = '\0';
        render_sim_wifi_status_json(config, response, sizeof(response), "simulator connected");
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/auth/status") == 0) {
        espclaw_auth_profile_t profile;

        espclaw_auth_profile_default(&profile);
        if (espclaw_auth_store_load(&profile) != 0) {
            profile.configured = false;
        }
        espclaw_render_auth_profile_json(&profile, response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "PUT") == 0 && strcmp(path, "/api/auth/codex") == 0) {
        espclaw_auth_profile_t profile;

        espclaw_auth_profile_default(&profile);
        espclaw_auth_store_load(&profile);
        merge_auth_profile_from_body(&profile, body != NULL ? body : "");
        if (!espclaw_auth_profile_is_ready(&profile)) {
            espclaw_admin_render_result_json(false, "provider credentials are incomplete for the selected provider", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }
        if (espclaw_auth_store_save(&profile) != 0) {
            espclaw_admin_render_result_json(false, "failed to save credentials", response, sizeof(response));
            send_http_response(client_fd, 500, "Internal Server Error", "application/json", response);
            return;
        }
        espclaw_render_auth_profile_json(&profile, response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "DELETE") == 0 && strcmp(path, "/api/auth/codex") == 0) {
        if (espclaw_auth_store_clear() != 0) {
            espclaw_admin_render_result_json(false, "failed to clear credentials", response, sizeof(response));
            send_http_response(client_fd, 500, "Internal Server Error", "application/json", response);
        } else {
            espclaw_admin_render_result_json(true, "credentials cleared", response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/auth/import-json") == 0) {
        espclaw_auth_profile_t profile;

        if (espclaw_auth_store_import_json(body != NULL ? body : "", &profile, response, sizeof(response)) != 0) {
            espclaw_admin_render_result_json(false, response, response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
        } else {
            espclaw_render_auth_profile_json(&profile, response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/auth/import-codex-cli") == 0) {
        espclaw_auth_profile_t profile;

        if (espclaw_auth_store_import_codex_cli(NULL, &profile, response, sizeof(response)) != 0) {
            espclaw_admin_render_result_json(false, response, response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
        } else {
            espclaw_render_auth_profile_json(&profile, response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/workspace/files") == 0) {
        espclaw_render_workspace_files_json(config->workspace_root, response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/monitor") == 0) {
        espclaw_system_monitor_snapshot_t snapshot;
        struct statvfs fs_stats;
        size_t total_bytes = 0;
        size_t used_bytes = 0;

        memset(&snapshot, 0, sizeof(snapshot));
        if (statvfs(config->workspace_root, &fs_stats) == 0) {
            total_bytes = (size_t)fs_stats.f_blocks * (size_t)fs_stats.f_frsize;
            used_bytes = total_bytes - ((size_t)fs_stats.f_bavail * (size_t)fs_stats.f_frsize);
        }
        espclaw_system_monitor_snapshot(&config->profile, total_bytes, used_bytes, &snapshot);
        espclaw_render_system_monitor_json(&snapshot, response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/apps") == 0) {
        espclaw_render_apps_json(config->workspace_root, response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/behaviors") == 0) {
        espclaw_render_behaviors_json(config->workspace_root, response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/tools") == 0) {
        espclaw_render_tools_json(response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/hardware") == 0) {
        espclaw_render_hardware_json(espclaw_board_current(), response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/loops") == 0) {
        espclaw_control_loop_render_json(response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/apps/detail") == 0) {
        char app_id[ESPCLAW_APP_ID_MAX + 1];

        if (!espclaw_admin_query_value(query, "app_id", app_id, sizeof(app_id))) {
            espclaw_admin_render_result_json(false, "missing app_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }

        espclaw_render_app_detail_json(config->workspace_root, app_id, response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/apps/scaffold") == 0) {
        char app_id[ESPCLAW_APP_ID_MAX + 1];

        if (!espclaw_admin_query_value(query, "app_id", app_id, sizeof(app_id))) {
            espclaw_admin_render_result_json(false, "missing app_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }

        if (espclaw_admin_scaffold_default_app(config->workspace_root, app_id) == 0) {
            espclaw_admin_render_result_json(true, "app scaffolded", response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        } else {
            espclaw_admin_render_result_json(false, "failed to scaffold app", response, sizeof(response));
            send_http_response(client_fd, 500, "Internal Server Error", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "PUT") == 0 && strcmp(path, "/api/apps/source") == 0) {
        char app_id[ESPCLAW_APP_ID_MAX + 1];

        if (!espclaw_admin_query_value(query, "app_id", app_id, sizeof(app_id))) {
            espclaw_admin_render_result_json(false, "missing app_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }

        if (espclaw_app_update_source(config->workspace_root, app_id, body != NULL ? body : "") == 0) {
            espclaw_admin_render_result_json(true, "app source updated", response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        } else {
            espclaw_admin_render_result_json(false, "failed to update app source", response, sizeof(response));
            send_http_response(client_fd, 500, "Internal Server Error", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "DELETE") == 0 && strcmp(path, "/api/apps") == 0) {
        char app_id[ESPCLAW_APP_ID_MAX + 1];

        if (!espclaw_admin_query_value(query, "app_id", app_id, sizeof(app_id))) {
            espclaw_admin_render_result_json(false, "missing app_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }

        if (espclaw_app_remove(config->workspace_root, app_id) == 0) {
            espclaw_admin_render_result_json(true, "app removed", response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        } else {
            espclaw_admin_render_result_json(false, "failed to remove app", response, sizeof(response));
            send_http_response(client_fd, 500, "Internal Server Error", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "DELETE") == 0 && strcmp(path, "/api/behaviors") == 0) {
        char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];
        char message[256];

        if (!espclaw_admin_query_value(query, "behavior_id", behavior_id, sizeof(behavior_id))) {
            espclaw_admin_render_result_json(false, "missing behavior_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }

        if (espclaw_behavior_remove(config->workspace_root, behavior_id, message, sizeof(message)) == 0) {
            espclaw_render_behaviors_json(config->workspace_root, response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        } else {
            espclaw_admin_render_result_json(false, message, response, sizeof(response));
            send_http_response(client_fd, 404, "Not Found", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/apps/run") == 0) {
        char app_id[ESPCLAW_APP_ID_MAX + 1];
        char trigger[ESPCLAW_APP_TRIGGER_NAME_MAX + 1];
        char result[1024];

        if (!espclaw_admin_query_value(query, "app_id", app_id, sizeof(app_id))) {
            espclaw_admin_render_result_json(false, "missing app_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }
        if (!espclaw_admin_query_value(query, "trigger", trigger, sizeof(trigger))) {
            snprintf(trigger, sizeof(trigger), "manual");
        }

        if (espclaw_app_run(config->workspace_root, app_id, trigger, body != NULL ? body : "", result, sizeof(result)) == 0) {
            espclaw_admin_render_run_result_json(app_id, trigger, true, result, response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        } else {
            espclaw_admin_render_run_result_json(app_id, trigger, false, result, response, sizeof(response));
            send_http_response(client_fd, 500, "Internal Server Error", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/chat/run") == 0) {
        char session_id[ESPCLAW_AGENT_SESSION_ID_MAX + 1];
        espclaw_agent_run_result_t result;
        bool yolo_mode = false;

        if (!espclaw_admin_query_value(query, "session_id", session_id, sizeof(session_id))) {
            copy_text(session_id, sizeof(session_id), "admin");
        }
        yolo_mode = query_u32_or_default(query, "yolo", 0) != 0;

        if (espclaw_agent_loop_run(config->workspace_root, session_id, body != NULL ? body : "", true, yolo_mode, &result) != 0 && !result.ok) {
            espclaw_admin_render_result_json(false, result.final_text, response, sizeof(response));
            send_http_response(client_fd, 500, "Internal Server Error", "application/json", response);
        } else {
            snprintf(
                response,
                sizeof(response),
                "{\"ok\":true,\"session_id\":\"%s\",\"iterations\":%u,\"used_tools\":%s,\"response_id\":\"%s\",\"final_text\":",
                session_id,
                result.iterations,
                result.used_tools ? "true" : "false",
                result.response_id
            );
            append_escaped_json(response, sizeof(response), strlen(response), result.final_text);
            strncat(response, "}", sizeof(response) - strlen(response) - 1);
            send_http_response(client_fd, 200, "OK", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "GET") == 0 && strncmp(path, "/media/", 7) == 0) {
        char relative_path[256];
        char absolute_path[512];

        snprintf(relative_path, sizeof(relative_path), "media/%s", path + 7);
        if (espclaw_workspace_resolve_path(config->workspace_root, relative_path, absolute_path, sizeof(absolute_path)) != 0) {
            send_http_response(client_fd, 400, "Bad Request", "application/json", "{\"ok\":false,\"message\":\"invalid media path\"}");
            return;
        }
        send_http_file_response(client_fd, 200, "OK", media_content_type(relative_path), absolute_path);
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/chat/session") == 0) {
        char session_id[ESPCLAW_AGENT_SESSION_ID_MAX + 1];

        if (!espclaw_admin_query_value(query, "session_id", session_id, sizeof(session_id))) {
            copy_text(session_id, sizeof(session_id), "admin");
        }
        espclaw_render_session_transcript_json(config->workspace_root, session_id, response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/loops/start") == 0) {
        char loop_id[ESPCLAW_CONTROL_LOOP_ID_MAX + 1];
        char app_id[ESPCLAW_APP_ID_MAX + 1];
        char trigger[ESPCLAW_APP_TRIGGER_NAME_MAX + 1];
        char message[256];

        if (!espclaw_admin_query_value(query, "loop_id", loop_id, sizeof(loop_id)) ||
            !espclaw_admin_query_value(query, "app_id", app_id, sizeof(app_id))) {
            espclaw_admin_render_result_json(false, "missing loop_id or app_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }
        if (!espclaw_admin_query_value(query, "trigger", trigger, sizeof(trigger))) {
            snprintf(trigger, sizeof(trigger), "manual");
        }

        if (espclaw_control_loop_start(
                loop_id,
                config->workspace_root,
                app_id,
                trigger,
                body != NULL ? body : "",
                query_u32_or_default(query, "period_ms", 20),
                query_u32_or_default(query, "iterations", 0),
                message,
                sizeof(message)) == 0) {
            espclaw_control_loop_render_json(response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        } else {
            espclaw_admin_render_result_json(false, message, response, sizeof(response));
            send_http_response(client_fd, 500, "Internal Server Error", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/loops/stop") == 0) {
        char loop_id[ESPCLAW_CONTROL_LOOP_ID_MAX + 1];
        char message[256];

        if (!espclaw_admin_query_value(query, "loop_id", loop_id, sizeof(loop_id))) {
            espclaw_admin_render_result_json(false, "missing loop_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }

        if (espclaw_control_loop_stop(loop_id, message, sizeof(message)) == 0) {
            espclaw_admin_render_result_json(true, message, response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        } else {
            espclaw_admin_render_result_json(false, message, response, sizeof(response));
            send_http_response(client_fd, 404, "Not Found", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/behaviors/register") == 0) {
        char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];
        char app_id[ESPCLAW_APP_ID_MAX + 1];
        char schedule[ESPCLAW_TASK_SCHEDULE_MAX + 1];
        char trigger[ESPCLAW_TASK_TRIGGER_MAX + 1];
        char message[256];
        espclaw_behavior_spec_t spec;

        if (!espclaw_admin_query_value(query, "behavior_id", behavior_id, sizeof(behavior_id)) ||
            !espclaw_admin_query_value(query, "app_id", app_id, sizeof(app_id))) {
            espclaw_admin_render_result_json(false, "missing behavior_id or app_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }
        if (!espclaw_admin_query_value(query, "schedule", schedule, sizeof(schedule))) {
            snprintf(schedule, sizeof(schedule), "periodic");
        }
        if (!espclaw_admin_query_value(query, "trigger", trigger, sizeof(trigger))) {
            snprintf(trigger, sizeof(trigger), "%s", strcmp(schedule, "event") == 0 ? "sensor" : "timer");
        }

        memset(&spec, 0, sizeof(spec));
        snprintf(spec.behavior_id, sizeof(spec.behavior_id), "%s", behavior_id);
        snprintf(spec.title, sizeof(spec.title), "%s", behavior_id);
        snprintf(spec.app_id, sizeof(spec.app_id), "%s", app_id);
        snprintf(spec.schedule, sizeof(spec.schedule), "%s", schedule);
        snprintf(spec.trigger, sizeof(spec.trigger), "%s", trigger);
        snprintf(spec.payload, sizeof(spec.payload), "%s", body != NULL ? body : "");
        spec.period_ms = query_u32_or_default(query, "period_ms", 20);
        spec.max_iterations = query_u32_or_default(query, "iterations", 0);
        spec.autostart = query_u32_or_default(query, "autostart", 0) != 0;

        if (espclaw_behavior_register(config->workspace_root, &spec, message, sizeof(message)) == 0) {
            espclaw_render_behaviors_json(config->workspace_root, response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        } else {
            espclaw_admin_render_result_json(false, message, response, sizeof(response));
            send_http_response(client_fd, 500, "Internal Server Error", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/behaviors/start") == 0) {
        char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];
        char message[256];

        if (!espclaw_admin_query_value(query, "behavior_id", behavior_id, sizeof(behavior_id))) {
            espclaw_admin_render_result_json(false, "missing behavior_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }

        if (espclaw_behavior_start(config->workspace_root, behavior_id, message, sizeof(message)) == 0) {
            espclaw_render_behaviors_json(config->workspace_root, response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        } else {
            espclaw_admin_render_result_json(false, message, response, sizeof(response));
            send_http_response(client_fd, 404, "Not Found", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/behaviors/stop") == 0) {
        char behavior_id[ESPCLAW_BEHAVIOR_ID_MAX + 1];
        char message[256];

        if (!espclaw_admin_query_value(query, "behavior_id", behavior_id, sizeof(behavior_id))) {
            espclaw_admin_render_result_json(false, "missing behavior_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }

        if (espclaw_behavior_stop(behavior_id, message, sizeof(message)) == 0) {
            espclaw_render_behaviors_json(config->workspace_root, response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        } else {
            espclaw_admin_render_result_json(false, message, response, sizeof(response));
            send_http_response(client_fd, 404, "Not Found", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/tasks") == 0) {
        espclaw_task_render_json(response, sizeof(response));
        send_http_response(client_fd, 200, "OK", "application/json", response);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/tasks/start") == 0) {
        char task_id[ESPCLAW_TASK_ID_MAX + 1];
        char app_id[ESPCLAW_APP_ID_MAX + 1];
        char schedule[ESPCLAW_TASK_SCHEDULE_MAX + 1];
        char trigger[ESPCLAW_APP_TRIGGER_NAME_MAX + 1];
        char message[256];

        if (!espclaw_admin_query_value(query, "task_id", task_id, sizeof(task_id)) ||
            !espclaw_admin_query_value(query, "app_id", app_id, sizeof(app_id))) {
            espclaw_admin_render_result_json(false, "missing task_id or app_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }
        if (!espclaw_admin_query_value(query, "schedule", schedule, sizeof(schedule))) {
            snprintf(schedule, sizeof(schedule), "periodic");
        }
        if (!espclaw_admin_query_value(query, "trigger", trigger, sizeof(trigger))) {
            snprintf(trigger, sizeof(trigger), "%s", strcmp(schedule, "event") == 0 ? "sensor" : "timer");
        }

        if (espclaw_task_start_with_schedule(
                task_id,
                config->workspace_root,
                app_id,
                schedule,
                trigger,
                body != NULL ? body : "",
                query_u32_or_default(query, "period_ms", 20),
                query_u32_or_default(query, "iterations", 0),
                message,
                sizeof(message)) == 0) {
            espclaw_task_render_json(response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        } else {
            espclaw_admin_render_result_json(false, message, response, sizeof(response));
            send_http_response(client_fd, 500, "Internal Server Error", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/events/emit") == 0) {
        char event_name[ESPCLAW_TASK_TRIGGER_MAX + 1];
        char message[256];

        if (!espclaw_admin_query_value(query, "name", event_name, sizeof(event_name))) {
            espclaw_admin_render_result_json(false, "missing name query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }

        if (espclaw_task_emit_event(event_name, body != NULL ? body : "", message, sizeof(message)) == 0) {
            espclaw_admin_render_result_json(true, message, response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        } else {
            espclaw_admin_render_result_json(false, message, response, sizeof(response));
            send_http_response(client_fd, 404, "Not Found", "application/json", response);
        }
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/tasks/stop") == 0) {
        char task_id[ESPCLAW_TASK_ID_MAX + 1];
        char message[256];

        if (!espclaw_admin_query_value(query, "task_id", task_id, sizeof(task_id))) {
            espclaw_admin_render_result_json(false, "missing task_id query parameter", response, sizeof(response));
            send_http_response(client_fd, 400, "Bad Request", "application/json", response);
            return;
        }

        if (espclaw_task_stop(task_id, message, sizeof(message)) == 0) {
            espclaw_admin_render_result_json(true, message, response, sizeof(response));
            send_http_response(client_fd, 200, "OK", "application/json", response);
        } else {
            espclaw_admin_render_result_json(false, message, response, sizeof(response));
            send_http_response(client_fd, 404, "Not Found", "application/json", response);
        }
        return;
    }

    espclaw_admin_render_result_json(false, "route not found", response, sizeof(response));
    send_http_response(client_fd, 404, "Not Found", "application/json", response);
}

static void handle_client(int client_fd, const espclaw_simulator_config_t *config)
{
    char method[8];
    char target[1024];
    char body[4096];
    char *path = NULL;
    char *query = NULL;

    if (!parse_request(client_fd, method, sizeof(method), target, sizeof(target), body, sizeof(body))) {
        send_http_response(client_fd, 400, "Bad Request", "text/plain; charset=utf-8", "invalid request");
        return;
    }

    split_target(target, &path, &query);
    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        send_http_response(client_fd, 200, "OK", "text/html; charset=utf-8", espclaw_admin_ui_html());
        return;
    }

    handle_api_request(client_fd, config, method, path, query != NULL ? query : "", body);
}

static int run_self_test(const espclaw_simulator_config_t *config)
{
    char status_json[512];
    char apps_json[1024];
    char boot_log[512];
    espclaw_ota_state_t ota_state = espclaw_ota_state_init();

    if (espclaw_workspace_bootstrap(config->workspace_root) != 0) {
        fprintf(stderr, "failed to bootstrap workspace %s\n", config->workspace_root);
        return 1;
    }
    espclaw_auth_store_init(config->workspace_root);
    espclaw_task_policy_select(&config->profile);
    espclaw_board_configure_current(config->workspace_root, &config->profile);
    s_sim_wifi_ready = false;
    s_sim_wifi_provisioning_active = true;
    s_sim_wifi_ssid[0] = '\0';
    snprintf(s_sim_onboarding_ssid, sizeof(s_sim_onboarding_ssid), "ESPClaw-Sim");

    if (espclaw_admin_scaffold_default_app(config->workspace_root, "sim_demo") != 0) {
        fprintf(stderr, "failed to scaffold simulator demo app\n");
        return 1;
    }

    if (espclaw_app_run_boot_apps(config->workspace_root, boot_log, sizeof(boot_log)) != 0) {
        fprintf(stderr, "failed to run boot apps in simulator self-test\n");
        return 1;
    }

    espclaw_render_admin_status_json(
        &config->profile,
        config->profile.default_storage_backend,
        "openai_compat",
        "telegram",
        true,
        &ota_state,
        status_json,
        sizeof(status_json)
    );
    espclaw_render_apps_json(config->workspace_root, apps_json, sizeof(apps_json));

    printf("%s\n%s\n", status_json, apps_json);
    return 0;
}

int main(int argc, char **argv)
{
    espclaw_simulator_config_t config = {
        .profile = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32S3),
        .port = 8080,
    };
    int server_fd;
    bool self_test = false;
    int index;

    snprintf(config.workspace_root, sizeof(config.workspace_root), ".espclaw-sim-workspace");
    s_sim_wifi_ready = false;
    s_sim_wifi_provisioning_active = true;
    s_sim_wifi_ssid[0] = '\0';
    snprintf(s_sim_onboarding_ssid, sizeof(s_sim_onboarding_ssid), "ESPClaw-Sim");

    for (index = 1; index < argc; ++index) {
        if (strcmp(argv[index], "--workspace") == 0 && index + 1 < argc) {
            snprintf(config.workspace_root, sizeof(config.workspace_root), "%s", argv[++index]);
        } else if (strcmp(argv[index], "--port") == 0 && index + 1 < argc) {
            config.port = atoi(argv[++index]);
        } else if (strcmp(argv[index], "--profile") == 0 && index + 1 < argc) {
            const char *profile = argv[++index];

            if (strcmp(profile, "esp32cam") == 0) {
                config.profile = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32CAM);
            } else {
                config.profile = espclaw_board_profile_for(ESPCLAW_BOARD_PROFILE_ESP32S3);
            }
        } else if (strcmp(argv[index], "--self-test") == 0) {
            self_test = true;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (self_test) {
        return run_self_test(&config);
    }

    if (espclaw_workspace_bootstrap(config.workspace_root) != 0) {
        fprintf(stderr, "failed to bootstrap workspace %s\n", config.workspace_root);
        return 1;
    }
    espclaw_auth_store_init(config.workspace_root);
    espclaw_task_policy_select(&config.profile);
    espclaw_board_configure_current(config.workspace_root, &config.profile);
    if (espclaw_event_watch_runtime_start() != 0) {
        fprintf(stderr, "failed to start event watch runtime\n");
        return 1;
    }

    {
        char boot_log[512];
        char behavior_log[512];

        if (espclaw_app_run_boot_apps(config.workspace_root, boot_log, sizeof(boot_log)) == 0 && boot_log[0] != '\0') {
            fprintf(stderr, "boot apps: %s\n", boot_log);
        }
        if (espclaw_behavior_start_autostart(config.workspace_root, behavior_log, sizeof(behavior_log)) == 0 &&
            behavior_log[0] != '\0') {
            fprintf(stderr, "autostart behaviors: %s\n", behavior_log);
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    {
        int reuse = 1;
        struct sockaddr_in address;

        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        memset(&address, 0, sizeof(address));
        address.sin_family = AF_INET;
        address.sin_port = htons((uint16_t)config.port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
            perror("bind");
            close(server_fd);
            return 1;
        }
    }

    if (listen(server_fd, 8) != 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    fprintf(stderr, "ESPClaw simulator listening at http://127.0.0.1:%d\n", config.port);
    fprintf(stderr, "Workspace: %s\n", config.workspace_root);

    while (!s_should_stop) {
        int client_fd = accept(server_fd, NULL, NULL);

        if (client_fd < 0) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }

        handle_client(client_fd, &config);
        close(client_fd);
    }

    espclaw_control_loop_shutdown_all();
    espclaw_event_watch_runtime_shutdown();
    close(server_fd);
    return 0;
}
