#include "espclaw/auth_store.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "espclaw/provider.h"
#include "espclaw/workspace.h"

#ifdef ESPCLAW_HOST_LUA
#include <sys/stat.h>
#else
#include "nvs.h"
#include "nvs_flash.h"
#endif

static char s_workspace_root[512];

static void copy_text(char *buffer, size_t buffer_size, const char *value)
{
    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    snprintf(buffer, buffer_size, "%s", value != NULL ? value : "");
}

void espclaw_auth_profile_default(espclaw_auth_profile_t *profile)
{
    const espclaw_provider_descriptor_t *provider = espclaw_find_provider("openai_codex");

    if (profile == NULL) {
        return;
    }

    memset(profile, 0, sizeof(*profile));
    if (provider != NULL) {
        copy_text(profile->provider_id, sizeof(profile->provider_id), provider->id);
        copy_text(profile->model, sizeof(profile->model), provider->default_model);
        copy_text(profile->base_url, sizeof(profile->base_url), provider->default_base_url);
    }
    copy_text(profile->source, sizeof(profile->source), "unset");
}

bool espclaw_auth_profile_is_ready(const espclaw_auth_profile_t *profile)
{
    const espclaw_provider_descriptor_t *provider;

    if (profile == NULL ||
        !profile->configured ||
        profile->provider_id[0] == '\0' ||
        profile->model[0] == '\0' ||
        profile->base_url[0] == '\0' ||
        profile->access_token[0] == '\0') {
        return false;
    }

    provider = espclaw_find_provider(profile->provider_id);
    if (provider != NULL && provider->requires_account_id) {
        return profile->account_id[0] != '\0';
    }

    return true;
}

int espclaw_auth_store_init(const char *workspace_root)
{
    if (workspace_root != NULL && workspace_root[0] != '\0') {
        copy_text(s_workspace_root, sizeof(s_workspace_root), workspace_root);
    }

#ifdef ESPCLAW_HOST_LUA
    return 0;
#else
    (void)workspace_root;
    return 0;
#endif
}

#ifdef ESPCLAW_HOST_LUA
static bool extract_string_value(const char *json, const char *key, char *buffer, size_t buffer_size)
{
    char pattern[64];
    const char *cursor;
    size_t used = 0;

    if (json == NULL || key == NULL || buffer == NULL || buffer_size == 0) {
        return false;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    cursor = strstr(json, pattern);
    if (cursor == NULL) {
        buffer[0] = '\0';
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

static bool extract_long_value(const char *json, const char *key, long *value)
{
    char pattern[64];
    const char *cursor;
    char *end_ptr = NULL;

    if (json == NULL || key == NULL || value == NULL) {
        return false;
    }

    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    cursor = strstr(json, pattern);
    if (cursor == NULL) {
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

    *value = strtol(cursor, &end_ptr, 10);
    return end_ptr != cursor;
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

static int auth_store_path(char *buffer, size_t buffer_size)
{
    if (s_workspace_root[0] == '\0') {
        return -1;
    }

    return espclaw_workspace_resolve_path(s_workspace_root, "config/auth.json", buffer, buffer_size);
}

static int load_profile_from_json(const char *json, espclaw_auth_profile_t *profile)
{
    long expires_at = 0;

    if (json == NULL || profile == NULL) {
        return -1;
    }

    espclaw_auth_profile_default(profile);
    extract_string_value(json, "provider_id", profile->provider_id, sizeof(profile->provider_id));
    extract_string_value(json, "model", profile->model, sizeof(profile->model));
    extract_string_value(json, "base_url", profile->base_url, sizeof(profile->base_url));
    extract_string_value(json, "access_token", profile->access_token, sizeof(profile->access_token));
    extract_string_value(json, "refresh_token", profile->refresh_token, sizeof(profile->refresh_token));
    extract_string_value(json, "account_id", profile->account_id, sizeof(profile->account_id));
    extract_string_value(json, "source", profile->source, sizeof(profile->source));
    if (extract_long_value(json, "expires_at", &expires_at)) {
        profile->expires_at = expires_at;
    }
    profile->configured = profile->access_token[0] != '\0';
    if (profile->source[0] == '\0') {
        copy_text(profile->source, sizeof(profile->source), "workspace");
    }
    return 0;
}

int espclaw_auth_store_load(espclaw_auth_profile_t *profile)
{
    char path[640];
    char json[8192];
    FILE *file;
    size_t bytes_read;

    if (profile == NULL) {
        return -1;
    }

    espclaw_auth_profile_default(profile);
    if (auth_store_path(path, sizeof(path)) != 0) {
        return -1;
    }

    file = fopen(path, "r");
    if (file == NULL) {
        return -1;
    }
    bytes_read = fread(json, 1, sizeof(json) - 1, file);
    fclose(file);
    json[bytes_read] = '\0';

    return load_profile_from_json(json, profile);
}

int espclaw_auth_store_save(const espclaw_auth_profile_t *profile)
{
    char path[640];
    char json[8192];
    FILE *file;
    size_t used = 0;

    if (profile == NULL || auth_store_path(path, sizeof(path)) != 0) {
        return -1;
    }

    if (espclaw_workspace_bootstrap(s_workspace_root) != 0) {
        return -1;
    }

    used += (size_t)snprintf(json + used, sizeof(json) - used, "{");
    used += (size_t)snprintf(json + used, sizeof(json) - used, "\"provider_id\":");
    used = append_escaped_json(json, sizeof(json), used, profile->provider_id);
    used += (size_t)snprintf(json + used, sizeof(json) - used, ",\"model\":");
    used = append_escaped_json(json, sizeof(json), used, profile->model);
    used += (size_t)snprintf(json + used, sizeof(json) - used, ",\"base_url\":");
    used = append_escaped_json(json, sizeof(json), used, profile->base_url);
    used += (size_t)snprintf(json + used, sizeof(json) - used, ",\"access_token\":");
    used = append_escaped_json(json, sizeof(json), used, profile->access_token);
    used += (size_t)snprintf(json + used, sizeof(json) - used, ",\"refresh_token\":");
    used = append_escaped_json(json, sizeof(json), used, profile->refresh_token);
    used += (size_t)snprintf(json + used, sizeof(json) - used, ",\"account_id\":");
    used = append_escaped_json(json, sizeof(json), used, profile->account_id);
    used += (size_t)snprintf(json + used, sizeof(json) - used, ",\"source\":");
    used = append_escaped_json(json, sizeof(json), used, profile->source);
    used += (size_t)snprintf(
        json + used,
        sizeof(json) - used,
        ",\"expires_at\":%ld}\n",
        profile->expires_at
    );

    file = fopen(path, "w");
    if (file == NULL) {
        return -1;
    }
    fputs(json, file);
    fclose(file);
    return 0;
}

int espclaw_auth_store_clear(void)
{
    char path[640];

    if (auth_store_path(path, sizeof(path)) != 0) {
        return -1;
    }

    if (remove(path) == 0 || errno == ENOENT) {
        return 0;
    }
    return -1;
}
#else
static int nvs_get_string(nvs_handle_t handle, const char *key, char *buffer, size_t buffer_size)
{
    size_t required = buffer_size;

    if (buffer == NULL || buffer_size == 0) {
        return -1;
    }
    buffer[0] = '\0';
    return nvs_get_str(handle, key, buffer, &required) == ESP_OK ? 0 : -1;
}

int espclaw_auth_store_load(espclaw_auth_profile_t *profile)
{
    nvs_handle_t handle;
    int ok = 0;

    if (profile == NULL) {
        return -1;
    }

    espclaw_auth_profile_default(profile);
    if (nvs_open("espclaw_auth", NVS_READONLY, &handle) != ESP_OK) {
        return -1;
    }

    ok |= nvs_get_string(handle, "provider_id", profile->provider_id, sizeof(profile->provider_id));
    ok |= nvs_get_string(handle, "model", profile->model, sizeof(profile->model));
    ok |= nvs_get_string(handle, "base_url", profile->base_url, sizeof(profile->base_url));
    ok |= nvs_get_string(handle, "access_token", profile->access_token, sizeof(profile->access_token));
    ok |= nvs_get_string(handle, "refresh_token", profile->refresh_token, sizeof(profile->refresh_token));
    ok |= nvs_get_string(handle, "account_id", profile->account_id, sizeof(profile->account_id));
    ok |= nvs_get_string(handle, "source", profile->source, sizeof(profile->source));
    nvs_get_i64(handle, "expires_at", (int64_t *)&profile->expires_at);
    nvs_close(handle);

    profile->configured = ok == 0 && profile->access_token[0] != '\0';
    if (profile->source[0] == '\0') {
        copy_text(profile->source, sizeof(profile->source), "nvs");
    }
    return profile->configured ? 0 : -1;
}

int espclaw_auth_store_save(const espclaw_auth_profile_t *profile)
{
    nvs_handle_t handle;

    if (profile == NULL) {
        return -1;
    }
    if (nvs_open("espclaw_auth", NVS_READWRITE, &handle) != ESP_OK) {
        return -1;
    }

    nvs_set_str(handle, "provider_id", profile->provider_id);
    nvs_set_str(handle, "model", profile->model);
    nvs_set_str(handle, "base_url", profile->base_url);
    nvs_set_str(handle, "access_token", profile->access_token);
    nvs_set_str(handle, "refresh_token", profile->refresh_token);
    nvs_set_str(handle, "account_id", profile->account_id);
    nvs_set_str(handle, "source", profile->source);
    nvs_set_i64(handle, "expires_at", (int64_t)profile->expires_at);
    nvs_commit(handle);
    nvs_close(handle);
    return 0;
}

int espclaw_auth_store_clear(void)
{
    nvs_handle_t handle;

    if (nvs_open("espclaw_auth", NVS_READWRITE, &handle) != ESP_OK) {
        return -1;
    }
    nvs_erase_all(handle);
    nvs_commit(handle);
    nvs_close(handle);
    return 0;
}
#endif

int espclaw_auth_store_import_codex_cli(
    const char *codex_home,
    espclaw_auth_profile_t *profile,
    char *message,
    size_t message_size
)
{
#ifdef ESPCLAW_HOST_LUA
    char path[640];
    char json[8192];
    FILE *file;
    size_t bytes_read;
    char access_token[ESPCLAW_AUTH_TOKEN_MAX + 1];
    char refresh_token[ESPCLAW_AUTH_TOKEN_MAX + 1];
    char account_id[ESPCLAW_AUTH_ACCOUNT_ID_MAX + 1];
    const char *home = codex_home;

    if (home == NULL || home[0] == '\0') {
        home = getenv("CODEX_HOME");
        if (home == NULL || home[0] == '\0') {
            home = "~/.codex";
        }
    }

    if (strncmp(home, "~/", 2) == 0) {
        const char *user_home = getenv("HOME");
        snprintf(path, sizeof(path), "%s/%s/auth.json", user_home != NULL ? user_home : "", home + 2);
    } else {
        snprintf(path, sizeof(path), "%s/auth.json", home);
    }

    file = fopen(path, "r");
    if (file == NULL) {
        snprintf(message, message_size, "unable to read %s", path);
        return -1;
    }
    bytes_read = fread(json, 1, sizeof(json) - 1, file);
    fclose(file);
    json[bytes_read] = '\0';

    access_token[0] = '\0';
    refresh_token[0] = '\0';
    account_id[0] = '\0';
    extract_string_value(json, "access_token", access_token, sizeof(access_token));
    extract_string_value(json, "refresh_token", refresh_token, sizeof(refresh_token));
    extract_string_value(json, "account_id", account_id, sizeof(account_id));
    if (access_token[0] == '\0' || account_id[0] == '\0') {
        snprintf(message, message_size, "codex auth.json is missing access_token or account_id");
        return -1;
    }

    if (profile != NULL) {
        espclaw_auth_profile_default(profile);
        profile->configured = true;
        copy_text(profile->provider_id, sizeof(profile->provider_id), "openai_codex");
        copy_text(profile->model, sizeof(profile->model), "gpt-5.3-codex");
        copy_text(profile->base_url, sizeof(profile->base_url), "https://chatgpt.com/backend-api/codex");
        copy_text(profile->access_token, sizeof(profile->access_token), access_token);
        copy_text(profile->refresh_token, sizeof(profile->refresh_token), refresh_token);
        copy_text(profile->account_id, sizeof(profile->account_id), account_id);
        copy_text(profile->source, sizeof(profile->source), "codex_cli");
        profile->expires_at = 0;
        if (espclaw_auth_store_save(profile) != 0) {
            snprintf(message, message_size, "failed to save imported Codex credentials");
            return -1;
        }
    }

    snprintf(message, message_size, "imported Codex credentials from %s", path);
    return 0;
#else
    (void)codex_home;
    (void)profile;
    snprintf(message, message_size, "Codex CLI import is only available in the host simulator");
    return -1;
#endif
}
