#include "espclaw/admin_server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_log.h"

#include "espclaw/admin_api.h"
#include "espclaw/agent_loop.h"
#include "espclaw/admin_ops.h"
#include "espclaw/admin_ui.h"
#include "espclaw/app_runtime.h"
#include "espclaw/auth_store.h"
#include "espclaw/control_loop.h"
#include "espclaw/ota_state.h"
#include "espclaw/runtime.h"

static const char *TAG = "espclaw_admin";
static httpd_handle_t s_admin_server;

#ifndef CONFIG_ESPCLAW_ADMIN_PORT
#define CONFIG_ESPCLAW_ADMIN_PORT 8080
#endif

static esp_err_t send_body(httpd_req_t *req, const char *content_type, const char *body)
{
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body != NULL ? body : "");
}

static esp_err_t send_json(httpd_req_t *req, const char *body)
{
    return send_body(req, "application/json", body);
}

static esp_err_t send_json_status(httpd_req_t *req, const char *body, int status_code, const char *status_text)
{
    char status[32];

    snprintf(status, sizeof(status), "%d %s", status_code, status_text);
    httpd_resp_set_status(req, status);
    return send_json(req, body);
}

static bool load_query_value(httpd_req_t *req, const char *key, char *buffer, size_t buffer_size)
{
    size_t query_length;
    char query[256];

    if (req == NULL || key == NULL || buffer == NULL || buffer_size == 0) {
        return false;
    }

    query_length = httpd_req_get_url_query_len(req);
    if (query_length == 0 || query_length >= sizeof(query)) {
        buffer[0] = '\0';
        return false;
    }

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        buffer[0] = '\0';
        return false;
    }

    return espclaw_admin_query_value(query, key, buffer, buffer_size);
}

static uint32_t load_query_u32(httpd_req_t *req, const char *key, uint32_t fallback)
{
    char value[32];

    if (load_query_value(req, key, value, sizeof(value))) {
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

static void append_text(char *buffer, size_t buffer_size, const char *value)
{
    size_t used = strlen(buffer);

    if (used >= buffer_size - 1) {
        return;
    }
    snprintf(buffer + used, buffer_size - used, "%s", value != NULL ? value : "");
}

static esp_err_t read_request_body(httpd_req_t *req, char *buffer, size_t buffer_size)
{
    int total = 0;

    if (req == NULL || buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (total < req->content_len && total < (int)buffer_size - 1) {
        int received = httpd_req_recv(req, buffer + total, (int)buffer_size - 1 - total);
        if (received <= 0) {
            return ESP_FAIL;
        }
        total += received;
    }

    buffer[total] = '\0';
    return total == req->content_len ? ESP_OK : ESP_FAIL;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    return send_body(req, "text/html; charset=utf-8", espclaw_admin_ui_html());
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char buffer[512];
    espclaw_ota_state_t ota_state = espclaw_ota_state_init();
    const espclaw_runtime_status_t *status = espclaw_runtime_status();
    espclaw_auth_profile_t profile;

    espclaw_auth_profile_default(&profile);
    espclaw_auth_store_load(&profile);

    espclaw_render_admin_status_json(
        status != NULL ? &status->profile : NULL,
        profile.provider_id,
        "telegram",
        status != NULL && status->storage_ready,
        &ota_state,
        buffer,
        sizeof(buffer)
    );
    return send_json(req, buffer);
}

static esp_err_t auth_status_get_handler(httpd_req_t *req)
{
    char buffer[768];
    espclaw_auth_profile_t profile;

    espclaw_auth_profile_default(&profile);
    if (espclaw_auth_store_load(&profile) != 0) {
        profile.configured = false;
    }
    espclaw_render_auth_profile_json(&profile, buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t tools_get_handler(httpd_req_t *req)
{
    char buffer[12288];

    espclaw_render_tools_json(buffer, sizeof(buffer));
    return send_json(req, buffer);
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

static esp_err_t auth_put_handler(httpd_req_t *req)
{
    char body[4096];
    char response[768];
    espclaw_auth_profile_t profile;

    espclaw_auth_profile_default(&profile);
    espclaw_auth_store_load(&profile);
    if (read_request_body(req, body, sizeof(body)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to read request body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    merge_auth_profile_from_body(&profile, body);
    if (!espclaw_auth_profile_is_ready(&profile)) {
        espclaw_admin_render_result_json(false, "provider credentials are incomplete for the selected provider", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (espclaw_auth_store_save(&profile) != 0) {
        espclaw_admin_render_result_json(false, "failed to save credentials", response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }

    espclaw_render_auth_profile_json(&profile, response, sizeof(response));
    return send_json(req, response);
}

static esp_err_t auth_delete_handler(httpd_req_t *req)
{
    char response[256];

    if (espclaw_auth_store_clear() != 0) {
        espclaw_admin_render_result_json(false, "failed to clear credentials", response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }

    espclaw_admin_render_result_json(true, "credentials cleared", response, sizeof(response));
    return send_json(req, response);
}

static esp_err_t auth_import_codex_cli_post_handler(httpd_req_t *req)
{
    char response[768];
    espclaw_auth_profile_t profile;

    if (espclaw_auth_store_import_codex_cli(NULL, &profile, response, sizeof(response)) != 0) {
        espclaw_admin_render_result_json(false, response, response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    espclaw_render_auth_profile_json(&profile, response, sizeof(response));
    return send_json(req, response);
}

static esp_err_t chat_run_post_handler(httpd_req_t *req)
{
    char session_id[ESPCLAW_AGENT_SESSION_ID_MAX + 1];
    char body[2048];
    char response[12288];
    espclaw_agent_run_result_t result;
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "session_id", session_id, sizeof(session_id))) {
        copy_text(session_id, sizeof(session_id), "admin");
    }
    if (req->content_len <= 0 || read_request_body(req, body, sizeof(body)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "missing chat body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (espclaw_agent_loop_run(status->workspace_root, session_id, body, false, &result) != 0 && !result.ok) {
        espclaw_admin_render_result_json(false, result.final_text, response, sizeof(response));
        return send_json_status(req, response, 500, "Internal Server Error");
    }

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
    append_text(response, sizeof(response), "}");
    return send_json(req, response);
}

static esp_err_t chat_session_get_handler(httpd_req_t *req)
{
    char session_id[ESPCLAW_AGENT_SESSION_ID_MAX + 1];
    char response[9216];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "session_id", session_id, sizeof(session_id))) {
        copy_text(session_id, sizeof(session_id), "admin");
    }

    espclaw_render_session_transcript_json(status->workspace_root, session_id, response, sizeof(response));
    return send_json(req, response);
}

static esp_err_t workspace_get_handler(httpd_req_t *req)
{
    char buffer[1024];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    espclaw_render_workspace_files_json(
        status != NULL && status->storage_ready ? status->workspace_root : NULL,
        buffer,
        sizeof(buffer)
    );
    return send_json(req, buffer);
}

static esp_err_t apps_get_handler(httpd_req_t *req)
{
    char buffer[2048];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    espclaw_render_apps_json(
        status != NULL && status->storage_ready ? status->workspace_root : NULL,
        buffer,
        sizeof(buffer)
    );
    return send_json(req, buffer);
}

static esp_err_t loops_get_handler(httpd_req_t *req)
{
    char buffer[4096];

    espclaw_control_loop_render_json(buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t app_detail_get_handler(httpd_req_t *req)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char buffer[4096];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing app_id query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }

    espclaw_render_app_detail_json(status->workspace_root, app_id, buffer, sizeof(buffer));
    return send_json(req, buffer);
}

static esp_err_t app_scaffold_post_handler(httpd_req_t *req)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing app_id query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }

    if (espclaw_admin_scaffold_default_app(status->workspace_root, app_id) == 0) {
        espclaw_admin_render_result_json(true, "app scaffolded", buffer, sizeof(buffer));
        return send_json(req, buffer);
    }

    espclaw_admin_render_result_json(false, "failed to scaffold app", buffer, sizeof(buffer));
    return send_json_status(req, buffer, 500, "Internal Server Error");
}

static esp_err_t app_source_put_handler(httpd_req_t *req)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char source[4096];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing app_id query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }
    if (read_request_body(req, source, sizeof(source)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to read request body", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }

    if (espclaw_app_update_source(status->workspace_root, app_id, source) == 0) {
        espclaw_admin_render_result_json(true, "app source updated", buffer, sizeof(buffer));
        return send_json(req, buffer);
    }

    espclaw_admin_render_result_json(false, "failed to update app source", buffer, sizeof(buffer));
    return send_json_status(req, buffer, 500, "Internal Server Error");
}

static esp_err_t app_run_post_handler(httpd_req_t *req)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char trigger[ESPCLAW_APP_TRIGGER_NAME_MAX + 1];
    char payload[1024];
    char result[1024];
    char response[1400];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing app_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (!load_query_value(req, "trigger", trigger, sizeof(trigger))) {
        snprintf(trigger, sizeof(trigger), "manual");
    }
    payload[0] = '\0';
    if (req->content_len > 0 && read_request_body(req, payload, sizeof(payload)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to read request body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    if (espclaw_app_run(status->workspace_root, app_id, trigger, payload, result, sizeof(result)) == 0) {
        espclaw_admin_render_run_result_json(app_id, trigger, true, result, response, sizeof(response));
        return send_json(req, response);
    }

    espclaw_admin_render_run_result_json(app_id, trigger, false, result, response, sizeof(response));
    return send_json_status(req, response, 500, "Internal Server Error");
}

static esp_err_t apps_delete_handler(httpd_req_t *req)
{
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char buffer[256];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing app_id query parameter", buffer, sizeof(buffer));
        return send_json_status(req, buffer, 400, "Bad Request");
    }

    if (espclaw_app_remove(status->workspace_root, app_id) == 0) {
        espclaw_admin_render_result_json(true, "app removed", buffer, sizeof(buffer));
        return send_json(req, buffer);
    }

    espclaw_admin_render_result_json(false, "failed to remove app", buffer, sizeof(buffer));
    return send_json_status(req, buffer, 500, "Internal Server Error");
}

static esp_err_t loop_start_post_handler(httpd_req_t *req)
{
    char loop_id[ESPCLAW_CONTROL_LOOP_ID_MAX + 1];
    char app_id[ESPCLAW_APP_ID_MAX + 1];
    char trigger[ESPCLAW_APP_TRIGGER_NAME_MAX + 1];
    char payload[1024];
    char message[256];
    char response[4096];
    const espclaw_runtime_status_t *status = espclaw_runtime_status();

    if (status == NULL || !status->storage_ready) {
        espclaw_admin_render_result_json(false, "workspace storage is not available", response, sizeof(response));
        return send_json_status(req, response, 503, "Service Unavailable");
    }
    if (!load_query_value(req, "loop_id", loop_id, sizeof(loop_id)) ||
        !load_query_value(req, "app_id", app_id, sizeof(app_id))) {
        espclaw_admin_render_result_json(false, "missing loop_id or app_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }
    if (!load_query_value(req, "trigger", trigger, sizeof(trigger))) {
        snprintf(trigger, sizeof(trigger), "manual");
    }
    payload[0] = '\0';
    if (req->content_len > 0 && read_request_body(req, payload, sizeof(payload)) != ESP_OK) {
        espclaw_admin_render_result_json(false, "failed to read request body", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    if (espclaw_control_loop_start(
            loop_id,
            status->workspace_root,
            app_id,
            trigger,
            payload,
            load_query_u32(req, "period_ms", 20),
            load_query_u32(req, "iterations", 0),
            message,
            sizeof(message)) == 0) {
        espclaw_control_loop_render_json(response, sizeof(response));
        return send_json(req, response);
    }

    espclaw_admin_render_result_json(false, message, response, sizeof(response));
    return send_json_status(req, response, 500, "Internal Server Error");
}

static esp_err_t loop_stop_post_handler(httpd_req_t *req)
{
    char loop_id[ESPCLAW_CONTROL_LOOP_ID_MAX + 1];
    char message[256];
    char response[512];

    if (!load_query_value(req, "loop_id", loop_id, sizeof(loop_id))) {
        espclaw_admin_render_result_json(false, "missing loop_id query parameter", response, sizeof(response));
        return send_json_status(req, response, 400, "Bad Request");
    }

    if (espclaw_control_loop_stop(loop_id, message, sizeof(message)) == 0) {
        espclaw_admin_render_result_json(true, message, response, sizeof(response));
        return send_json(req, response);
    }

    espclaw_admin_render_result_json(false, message, response, sizeof(response));
    return send_json_status(req, response, 404, "Not Found");
}

esp_err_t espclaw_admin_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL},
        {.uri = "/api/auth/status", .method = HTTP_GET, .handler = auth_status_get_handler, .user_ctx = NULL},
        {.uri = "/api/auth/codex", .method = HTTP_PUT, .handler = auth_put_handler, .user_ctx = NULL},
        {.uri = "/api/auth/codex", .method = HTTP_DELETE, .handler = auth_delete_handler, .user_ctx = NULL},
        {.uri = "/api/auth/import-codex-cli", .method = HTTP_POST, .handler = auth_import_codex_cli_post_handler, .user_ctx = NULL},
        {.uri = "/api/workspace/files", .method = HTTP_GET, .handler = workspace_get_handler, .user_ctx = NULL},
        {.uri = "/api/tools", .method = HTTP_GET, .handler = tools_get_handler, .user_ctx = NULL},
        {.uri = "/api/apps", .method = HTTP_GET, .handler = apps_get_handler, .user_ctx = NULL},
        {.uri = "/api/apps", .method = HTTP_DELETE, .handler = apps_delete_handler, .user_ctx = NULL},
        {.uri = "/api/loops", .method = HTTP_GET, .handler = loops_get_handler, .user_ctx = NULL},
        {.uri = "/api/apps/detail", .method = HTTP_GET, .handler = app_detail_get_handler, .user_ctx = NULL},
        {.uri = "/api/apps/scaffold", .method = HTTP_POST, .handler = app_scaffold_post_handler, .user_ctx = NULL},
        {.uri = "/api/apps/source", .method = HTTP_PUT, .handler = app_source_put_handler, .user_ctx = NULL},
        {.uri = "/api/apps/run", .method = HTTP_POST, .handler = app_run_post_handler, .user_ctx = NULL},
        {.uri = "/api/chat/run", .method = HTTP_POST, .handler = chat_run_post_handler, .user_ctx = NULL},
        {.uri = "/api/chat/session", .method = HTTP_GET, .handler = chat_session_get_handler, .user_ctx = NULL},
        {.uri = "/api/loops/start", .method = HTTP_POST, .handler = loop_start_post_handler, .user_ctx = NULL},
        {.uri = "/api/loops/stop", .method = HTTP_POST, .handler = loop_stop_post_handler, .user_ctx = NULL},
    };
    size_t index;

    if (s_admin_server != NULL) {
        return ESP_OK;
    }

    config.server_port = CONFIG_ESPCLAW_ADMIN_PORT;
    config.max_uri_handlers = 24;
    if (httpd_start(&s_admin_server, &config) != ESP_OK) {
        return ESP_FAIL;
    }

    for (index = 0; index < sizeof(routes) / sizeof(routes[0]); ++index) {
        if (httpd_register_uri_handler(s_admin_server, &routes[index]) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to register route %s", routes[index].uri);
            httpd_stop(s_admin_server);
            s_admin_server = NULL;
            return ESP_FAIL;
        }
    }

    ESP_LOGI(TAG, "Admin server listening on port %d", config.server_port);
    return ESP_OK;
}
