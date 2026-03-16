#include "espclaw/runtime.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_softap.h"
#if defined(CONFIG_BT_ENABLED) && CONFIG_BT_ENABLED
#include "wifi_provisioning/scheme_ble.h"
#define ESPCLAW_BLE_PROVISIONING_AVAILABLE 1
#else
#define ESPCLAW_BLE_PROVISIONING_AVAILABLE 0
#endif

#include "espclaw/admin_api.h"
#include "espclaw/admin_server.h"
#include "espclaw/agent_loop.h"
#include "espclaw/admin_ops.h"
#include "espclaw/app_runtime.h"
#include "espclaw/auth_store.h"
#include "espclaw/board_config.h"
#include "espclaw/board_profile.h"
#include "espclaw/session_store.h"
#include "espclaw/storage.h"
#include "espclaw/task_policy.h"
#include "espclaw/telegram_protocol.h"
#include "espclaw/workspace.h"

static const char *TAG = "espclaw_runtime";

#ifndef CONFIG_ESPCLAW_TELEGRAM_BOT_TOKEN
#define CONFIG_ESPCLAW_TELEGRAM_BOT_TOKEN ""
#endif

#ifndef CONFIG_ESPCLAW_TELEGRAM_POLL_INTERVAL_SECONDS
#define CONFIG_ESPCLAW_TELEGRAM_POLL_INTERVAL_SECONDS 5
#endif

#ifndef CONFIG_ESPCLAW_ENABLE_SD_BOOTSTRAP
#define CONFIG_ESPCLAW_ENABLE_SD_BOOTSTRAP 1
#endif

#ifndef CONFIG_ESPCLAW_ENABLE_WIFI_PROVISIONING
#define CONFIG_ESPCLAW_ENABLE_WIFI_PROVISIONING 1
#endif

#ifndef CONFIG_ESPCLAW_ENABLE_TELEGRAM
#define CONFIG_ESPCLAW_ENABLE_TELEGRAM 1
#endif

enum {
    ESPCLAW_WIFI_CONNECTED_BIT = BIT0
};

static EventGroupHandle_t s_runtime_event_group;
static espclaw_runtime_status_t s_runtime_status;
static long s_telegram_offset;

static void provisioning_event_cb(void *user_data, wifi_prov_cb_event_t event, void *event_data)
{
    (void)user_data;
    (void)event_data;

    switch (event) {
    case WIFI_PROV_START:
        s_runtime_status.provisioning_active = true;
        break;
    case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "Provisioning credentials accepted");
        break;
    case WIFI_PROV_END:
        s_runtime_status.provisioning_active = false;
        wifi_prov_mgr_deinit();
        if (espclaw_admin_server_start() != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start admin server after provisioning");
        }
        break;
    default:
        break;
    }
}

static esp_err_t read_http_response(esp_http_client_handle_t client, char *buffer, size_t buffer_size)
{
    int total_read = 0;

    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    while (total_read < (int)buffer_size - 1) {
        int read_now = esp_http_client_read(client, buffer + total_read, (int)buffer_size - 1 - total_read);
        if (read_now < 0) {
            return ESP_FAIL;
        }
        if (read_now == 0) {
            break;
        }
        total_read += read_now;
    }

    buffer[total_read] = '\0';
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_runtime_status.wifi_ready = false;
        xEventGroupClearBits(s_runtime_event_group, ESPCLAW_WIFI_CONNECTED_BIT);
        esp_wifi_connect();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        s_runtime_status.wifi_ready = true;
        xEventGroupSetBits(s_runtime_event_group, ESPCLAW_WIFI_CONNECTED_BIT);
    }
}

static esp_err_t init_nvs_flash_store(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static esp_err_t mount_workspace_storage(const espclaw_board_profile_t *profile)
{
    espclaw_storage_mount_t mount;

    if (profile == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (espclaw_storage_mount_workspace(profile, &mount) != ESP_OK) {
        ESP_LOGW(
            TAG,
            "Workspace mount failed for backend %s",
            espclaw_storage_backend_name(espclaw_storage_backend_for_profile(profile))
        );
        return ESP_FAIL;
    }

    s_runtime_status.storage_backend = mount.backend;
    s_runtime_status.storage_total_bytes = mount.total_bytes;
    s_runtime_status.storage_used_bytes = mount.used_bytes;
    snprintf(s_runtime_status.workspace_root, sizeof(s_runtime_status.workspace_root), "%s", mount.workspace_root);
    s_runtime_status.storage_ready = true;
    ESP_LOGI(
        TAG,
        "Workspace ready backend=%s root=%s",
        espclaw_storage_backend_name(s_runtime_status.storage_backend),
        s_runtime_status.workspace_root
    );
    return ESP_OK;
}

static esp_err_t init_wifi_stack(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    return ESP_OK;
}

static esp_err_t start_wifi_provisioning(const espclaw_board_profile_t *profile)
{
#if CONFIG_ESPCLAW_ENABLE_WIFI_PROVISIONING
    wifi_prov_mgr_config_t config = {0};
    bool provisioned = false;
    char service_name[32];
    const bool wants_ble = profile != NULL && profile->supports_ble_provisioning;
    const bool use_ble = wants_ble && ESPCLAW_BLE_PROVISIONING_AVAILABLE;

    if (use_ble) {
#if ESPCLAW_BLE_PROVISIONING_AVAILABLE
        config.scheme = wifi_prov_scheme_ble;
        config.scheme_event_handler = (wifi_prov_event_handler_t)WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;
#endif
    } else {
        config.scheme = wifi_prov_scheme_softap;
        config.scheme_event_handler = (wifi_prov_event_handler_t)WIFI_PROV_EVENT_HANDLER_NONE;
        if (wants_ble) {
            ESP_LOGW(TAG, "BLE provisioning requested by board profile but BT is disabled in sdkconfig, using SoftAP");
            s_runtime_status.profile.provisioning = "softap";
        }
    }
    config.app_event_handler.event_cb = provisioning_event_cb;
    config.app_event_handler.user_data = NULL;
    config.wifi_prov_conn_cfg.wifi_conn_attempts = 0;

    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (!provisioned) {
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(service_name, sizeof(service_name), "ESPClaw-%02X%02X%02X", mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Starting provisioning service %s", service_name);
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
            WIFI_PROV_SECURITY_1,
            "espclaw-pass",
            service_name,
            NULL
        ));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Wi-Fi already provisioned, starting station mode");
    wifi_prov_mgr_deinit();
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_runtime_status.provisioning_active = false;
    return ESP_OK;
#else
    (void)profile;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

static esp_err_t telegram_send_message(const char *token, const char *chat_id, const char *text)
{
    char url[256];
    char payload[1152];
    esp_http_client_config_t config = {0};
    esp_http_client_handle_t client;

    if (token == NULL || chat_id == NULL || text == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/sendMessage", token);
    espclaw_telegram_build_send_message_payload(chat_id, text, payload, sizeof(payload));

    config.url = url;
    config.transport_type = HTTP_TRANSPORT_OVER_SSL;
    config.timeout_ms = 15000;
    client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, (int)strlen(payload));
    if (esp_http_client_perform(client) != ESP_OK) {
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    esp_http_client_cleanup(client);
    return ESP_OK;
}

static void build_status_reply(char *buffer, size_t buffer_size)
{
    espclaw_ota_state_t ota_state = espclaw_ota_state_init();
    char admin_status[384];
    espclaw_auth_profile_t profile;

    espclaw_auth_profile_default(&profile);
    espclaw_auth_store_load(&profile);

    espclaw_render_admin_status_json(
        &s_runtime_status.profile,
        s_runtime_status.storage_backend,
        profile.provider_id,
        "telegram",
        s_runtime_status.storage_ready,
        &ota_state,
        admin_status,
        sizeof(admin_status)
    );
    snprintf(buffer, buffer_size, "ESPClaw status: %s", admin_status);
}

static void build_apps_list_reply(char *buffer, size_t buffer_size)
{
    char ids[8][ESPCLAW_APP_ID_MAX + 1];
    size_t count = 0;
    size_t index;
    size_t written;

    if (!s_runtime_status.storage_ready ||
        espclaw_app_collect_ids(s_runtime_status.workspace_root, ids, 8, &count) != 0 ||
        count == 0) {
        snprintf(buffer, buffer_size, "No apps installed.");
        return;
    }

    written = (size_t)snprintf(buffer, buffer_size, "Installed apps:");
    for (index = 0; index < count && written < buffer_size; ++index) {
        espclaw_app_manifest_t manifest;

        if (espclaw_app_load_manifest(s_runtime_status.workspace_root, ids[index], &manifest) == 0) {
            written += (size_t)snprintf(
                buffer + written,
                buffer_size - written,
                "%s%s(%s)",
                index == 0 ? " " : ", ",
                manifest.app_id,
                manifest.version
            );
        } else {
            written += (size_t)snprintf(buffer + written, buffer_size - written, "%s%s", index == 0 ? " " : ", ", ids[index]);
        }
    }
}

static bool parse_app_command(
    const char *message,
    char *app_id,
    size_t app_id_size,
    char *payload,
    size_t payload_size
)
{
    const char *cursor = NULL;
    const char *payload_start = NULL;
    size_t app_id_length = 0;

    if (message == NULL || app_id == NULL || payload == NULL || strncmp(message, "/app ", 5) != 0) {
        return false;
    }

    cursor = message + 5;
    while (*cursor == ' ') {
        cursor++;
    }
    if (*cursor == '\0') {
        return false;
    }

    payload_start = strchr(cursor, ' ');
    app_id_length = payload_start == NULL ? strlen(cursor) : (size_t)(payload_start - cursor);
    if (app_id_length == 0 || app_id_length >= app_id_size) {
        return false;
    }

    memcpy(app_id, cursor, app_id_length);
    app_id[app_id_length] = '\0';

    if (payload_start == NULL) {
        payload[0] = '\0';
    } else {
        while (*payload_start == ' ') {
            payload_start++;
        }
        snprintf(payload, payload_size, "%s", payload_start);
    }

    return espclaw_app_id_is_valid(app_id);
}

static bool parse_new_app_command(
    const char *message,
    char *app_id,
    size_t app_id_size
)
{
    const char *cursor = NULL;

    if (message == NULL || app_id == NULL || strncmp(message, "/newapp ", 8) != 0) {
        return false;
    }

    cursor = message + 8;
    while (*cursor == ' ') {
        cursor++;
    }
    if (*cursor == '\0' || strlen(cursor) >= app_id_size) {
        return false;
    }

    snprintf(app_id, app_id_size, "%s", cursor);
    return espclaw_app_id_is_valid(app_id);
}

static bool parse_remove_app_command(
    const char *message,
    char *app_id,
    size_t app_id_size
)
{
    const char *cursor = NULL;

    if (message == NULL || app_id == NULL || strncmp(message, "/rmapp ", 7) != 0) {
        return false;
    }

    cursor = message + 7;
    while (*cursor == ' ') {
        cursor++;
    }
    if (*cursor == '\0' || strlen(cursor) >= app_id_size) {
        return false;
    }

    snprintf(app_id, app_id_size, "%s", cursor);
    return espclaw_app_id_is_valid(app_id);
}

static void telegram_polling_task(void *arg)
{
    const char *token = (const char *)arg;

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            s_runtime_event_group,
            ESPCLAW_WIFI_CONNECTED_BIT,
            pdFALSE,
            pdTRUE,
            pdMS_TO_TICKS(5000)
        );

        if ((bits & ESPCLAW_WIFI_CONNECTED_BIT) == 0) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        {
            char url[512];
            char response[8192];
            char reply[768];
            esp_http_client_config_t config = {0};
            esp_http_client_handle_t client;
            espclaw_telegram_update_t update;

            snprintf(
                url,
                sizeof(url),
                "https://api.telegram.org/bot%s/getUpdates?timeout=5&offset=%ld",
                token,
                s_telegram_offset
            );

            config.url = url;
            config.transport_type = HTTP_TRANSPORT_OVER_SSL;
            config.timeout_ms = 15000;
            client = esp_http_client_init(&config);
            if (client == NULL) {
                vTaskDelay(pdMS_TO_TICKS(2000));
                continue;
            }

            esp_http_client_set_method(client, HTTP_METHOD_GET);
            if (esp_http_client_perform(client) == ESP_OK &&
                read_http_response(client, response, sizeof(response)) == ESP_OK &&
                espclaw_telegram_extract_update(response, &update)) {
                bool append_exchange = true;

                s_telegram_offset = update.update_id + 1;

                if (strcmp(update.text, "/status") == 0) {
                    build_status_reply(reply, sizeof(reply));
                } else if (strcmp(update.text, "/apps") == 0) {
                    build_apps_list_reply(reply, sizeof(reply));
                } else if (strncmp(update.text, "/newapp ", 8) == 0) {
                    char app_id[ESPCLAW_APP_ID_MAX + 1];

                    if (!s_runtime_status.storage_ready) {
                        snprintf(reply, sizeof(reply), "Workspace storage is not available.");
                    } else if (!parse_new_app_command(update.text, app_id, sizeof(app_id))) {
                        snprintf(reply, sizeof(reply), "Usage: /newapp <app_id>");
                    } else {
                        if (espclaw_admin_scaffold_default_app(s_runtime_status.workspace_root, app_id) == 0) {
                            snprintf(reply, sizeof(reply), "Created app %s. Run it with /app %s", app_id, app_id);
                        } else {
                            snprintf(reply, sizeof(reply), "Failed to create app %s", app_id);
                        }
                    }
                } else if (strncmp(update.text, "/app ", 5) == 0) {
                    char app_id[ESPCLAW_APP_ID_MAX + 1];
                    char app_payload[256];

                    if (!s_runtime_status.storage_ready) {
                        snprintf(reply, sizeof(reply), "Workspace storage is not available.");
                    } else if (!parse_app_command(update.text, app_id, sizeof(app_id), app_payload, sizeof(app_payload))) {
                        snprintf(reply, sizeof(reply), "Usage: /app <app_id> [payload]");
                    } else if (espclaw_app_run(
                                   s_runtime_status.workspace_root,
                                   app_id,
                                   "telegram",
                                   app_payload,
                                   reply,
                                   sizeof(reply)) != 0 && reply[0] == '\0') {
                        snprintf(reply, sizeof(reply), "Failed to run app %s", app_id);
                    }
                } else if (strncmp(update.text, "/rmapp ", 7) == 0) {
                    char app_id[ESPCLAW_APP_ID_MAX + 1];

                    if (!s_runtime_status.storage_ready) {
                        snprintf(reply, sizeof(reply), "Workspace storage is not available.");
                    } else if (!parse_remove_app_command(update.text, app_id, sizeof(app_id))) {
                        snprintf(reply, sizeof(reply), "Usage: /rmapp <app_id>");
                    } else if (espclaw_app_remove(s_runtime_status.workspace_root, app_id) == 0) {
                        snprintf(reply, sizeof(reply), "Removed app %s", app_id);
                    } else {
                        snprintf(reply, sizeof(reply), "Failed to remove app %s", app_id);
                    }
                } else {
                    espclaw_agent_run_result_t run_result;

                    append_exchange = false;
                    if (!s_runtime_status.storage_ready) {
                        snprintf(reply, sizeof(reply), "Workspace storage is not available.");
                    } else if (espclaw_agent_loop_run(
                                   s_runtime_status.workspace_root,
                                   update.chat_id,
                                   update.text,
                                   false,
                                   &run_result) == 0 || run_result.ok) {
                        snprintf(reply, sizeof(reply), "%s", run_result.final_text);
                    } else {
                        snprintf(reply, sizeof(reply), "%s", run_result.final_text);
                    }
                }

                if (s_runtime_status.storage_ready && append_exchange) {
                    espclaw_session_append_message(
                        s_runtime_status.workspace_root,
                        update.chat_id,
                        "user",
                        update.text
                    );
                    espclaw_session_append_message(
                        s_runtime_status.workspace_root,
                        update.chat_id,
                        "assistant",
                        reply
                    );
                }

                if (telegram_send_message(token, update.chat_id, reply) == ESP_OK) {
                    s_runtime_status.telegram_ready = true;
                }
            }

            esp_http_client_cleanup(client);
        }

        vTaskDelay(pdMS_TO_TICKS(CONFIG_ESPCLAW_TELEGRAM_POLL_INTERVAL_SECONDS * 1000));
    }
}

static esp_err_t maybe_start_telegram(void)
{
#if CONFIG_ESPCLAW_ENABLE_TELEGRAM
    int core = espclaw_task_policy_core_for(ESPCLAW_TASK_KIND_TELEGRAM);

    if (strlen(CONFIG_ESPCLAW_TELEGRAM_BOT_TOKEN) == 0) {
        ESP_LOGI(TAG, "Telegram polling disabled: empty bot token");
        return ESP_OK;
    }

    if (xTaskCreatePinnedToCore(
            telegram_polling_task,
            "espclaw_tg",
            8192,
            (void *)CONFIG_ESPCLAW_TELEGRAM_BOT_TOKEN,
            5,
            NULL,
            core >= 0 ? core : tskNO_AFFINITY) != pdPASS) {
        return ESP_FAIL;
    }
    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t espclaw_runtime_start(espclaw_board_profile_id_t profile_id, espclaw_runtime_status_t *status)
{
    memset(&s_runtime_status, 0, sizeof(s_runtime_status));
    s_runtime_status.profile = espclaw_board_profile_for(profile_id);
    espclaw_task_policy_select(&s_runtime_status.profile);
    espclaw_board_configure_current(NULL, &s_runtime_status.profile);

    if (s_runtime_event_group == NULL) {
        s_runtime_event_group = xEventGroupCreate();
        if (s_runtime_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_ERROR_CHECK(init_nvs_flash_store());
    ESP_ERROR_CHECK(init_wifi_stack());

    if (mount_workspace_storage(&s_runtime_status.profile) != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without workspace storage");
    } else {
        char boot_log[512];

        espclaw_board_configure_current(s_runtime_status.workspace_root, &s_runtime_status.profile);
        espclaw_auth_store_init(s_runtime_status.workspace_root);
        if (espclaw_app_run_boot_apps(s_runtime_status.workspace_root, boot_log, sizeof(boot_log)) == 0 &&
            boot_log[0] != '\0') {
            ESP_LOGI(TAG, "Boot apps: %s", boot_log);
        }
    }

    ESP_ERROR_CHECK(start_wifi_provisioning(&s_runtime_status.profile));
    ESP_ERROR_CHECK(maybe_start_telegram());

    if (status != NULL) {
        *status = s_runtime_status;
    }
    return ESP_OK;
}

const espclaw_runtime_status_t *espclaw_runtime_status(void)
{
    return &s_runtime_status;
}
