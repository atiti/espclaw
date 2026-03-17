#include "espclaw/runtime.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "wifi_provisioning/manager.h"
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
#include "espclaw/behavior_runtime.h"
#include "espclaw/board_config.h"
#include "espclaw/event_watch.h"
#include "espclaw/board_profile.h"
#include "espclaw/session_store.h"
#include "espclaw/storage.h"
#include "espclaw/system_monitor.h"
#include "espclaw/task_policy.h"
#include "espclaw/telegram_protocol.h"
#include "espclaw/workspace.h"

static const char *TAG = "espclaw_runtime";
static const char *ESPCLAW_PROVISIONING_POP = "espclaw-pass";

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
static bool s_softap_onboarding_active;
static bool s_ble_provisioning_active;
static bool s_sta_connect_enabled;
static bool s_time_sync_started;
static bool s_time_sync_completed;
static esp_sntp_config_t s_sntp_config;

bool espclaw_runtime_should_defer_wifi_boot(const espclaw_board_profile_t *profile, bool storage_ready)
{
    if (profile == NULL) {
        return false;
    }

    /* ESP32-CAM boards often brown out when failed SD bootstrap is followed
     * immediately by STA Wi-Fi start and full RF calibration. */
    return profile->profile_id == ESPCLAW_BOARD_PROFILE_ESP32CAM && !storage_ready;
}

bool espclaw_runtime_should_force_softap_only_boot(
    const espclaw_board_profile_t *profile,
    bool storage_ready,
    bool has_saved_wifi_credentials
)
{
    return espclaw_runtime_should_defer_wifi_boot(profile, storage_ready) && !has_saved_wifi_credentials;
}

static bool system_time_sane(void)
{
    time_t now = 0;

    time(&now);
    return now >= 1704067200; /* 2024-01-01 UTC */
}

static void maybe_start_time_sync(void)
{
    const esp_sntp_config_t default_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.google.com");

    if (s_time_sync_started) {
        return;
    }
    if (!s_runtime_status.wifi_ready) {
        ESP_LOGW(TAG, "Deferring SNTP start until Wi-Fi is ready");
        return;
    }

    s_time_sync_completed = false;
    s_sntp_config = default_config;
    /* Keep the config in static storage and rely on sync_wait() instead of an
     * async callback so time-sync startup stays predictable on constrained boards. */
    s_sntp_config.sync_cb = NULL;
    if (esp_netif_sntp_init(&s_sntp_config) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start SNTP time sync");
        return;
    }
    s_time_sync_started = true;
    ESP_LOGI(TAG, "Starting SNTP time sync");
}

static esp_err_t build_runtime_provisioning_descriptor(espclaw_provisioning_descriptor_t *descriptor)
{
    const char *transport = "";
    const char *admin_url = "";
    const char *pop = "";

    if (descriptor == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (s_runtime_status.provisioning_active) {
        if (s_ble_provisioning_active) {
            transport = "ble";
            pop = ESPCLAW_PROVISIONING_POP;
        } else if (s_softap_onboarding_active) {
            transport = "softap";
            admin_url = "http://192.168.4.1/";
        } else if (s_runtime_status.profile.provisioning != NULL) {
            transport = s_runtime_status.profile.provisioning;
        }
    }

    if (espclaw_provisioning_build_descriptor(
            s_runtime_status.provisioning_active,
            transport,
            s_runtime_status.onboarding_ssid,
            "",
            pop,
            admin_url,
            descriptor) != 0) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void clear_onboarding_state(void)
{
    s_softap_onboarding_active = false;
    s_ble_provisioning_active = false;
    s_runtime_status.provisioning_active = false;
    s_runtime_status.onboarding_ssid[0] = '\0';
}

static void refresh_wifi_ssid(void)
{
    wifi_config_t wifi_cfg = {0};

    s_runtime_status.wifi_ssid[0] = '\0';
    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK) {
        return;
    }

    snprintf(s_runtime_status.wifi_ssid, sizeof(s_runtime_status.wifi_ssid), "%s", (const char *)wifi_cfg.sta.ssid);
}

static bool has_saved_sta_credentials(void)
{
    wifi_config_t wifi_cfg = {0};

    if (esp_wifi_get_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK) {
        return false;
    }

    return wifi_cfg.sta.ssid[0] != '\0';
}

static void provisioning_event_cb(void *user_data, wifi_prov_cb_event_t event, void *event_data)
{
    (void)user_data;
    (void)event_data;

    switch (event) {
    case WIFI_PROV_START:
        s_ble_provisioning_active = true;
        s_runtime_status.provisioning_active = true;
        break;
    case WIFI_PROV_CRED_SUCCESS:
        ESP_LOGI(TAG, "Provisioning credentials accepted");
        refresh_wifi_ssid();
        break;
    case WIFI_PROV_END:
        clear_onboarding_state();
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
        if (s_sta_connect_enabled) {
            esp_wifi_connect();
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_runtime_status.wifi_ready = false;
        xEventGroupClearBits(s_runtime_event_group, ESPCLAW_WIFI_CONNECTED_BIT);
        if (s_sta_connect_enabled) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        refresh_wifi_ssid();
        s_runtime_status.wifi_ready = true;
        xEventGroupSetBits(s_runtime_event_group, ESPCLAW_WIFI_CONNECTED_BIT);
        if (s_softap_onboarding_active) {
            ESP_LOGI(TAG, "Wi-Fi joined, disabling onboarding SoftAP");
            clear_onboarding_state();
            if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to disable SoftAP after onboarding");
            }
        }
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
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_FLASH));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    return ESP_OK;
}

static esp_err_t start_softap_onboarding(bool force_ap_only)
{
    wifi_config_t sta_cfg = {0};
    wifi_config_t ap_cfg = {0};
    uint8_t mac[6] = {0};
    char service_name[32];

    if (!force_ap_only &&
        esp_wifi_get_config(WIFI_IF_STA, &sta_cfg) == ESP_OK &&
        sta_cfg.sta.ssid[0] != '\0') {
        ESP_LOGI(TAG, "Wi-Fi already configured, starting station mode");
        s_sta_connect_enabled = true;
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        refresh_wifi_ssid();
        clear_onboarding_state();
        return ESP_OK;
    }

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(service_name, sizeof(service_name), "ESPClaw-%02X%02X%02X", mac[3], mac[4], mac[5]);

    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", service_name);
    ap_cfg.ap.ssid_len = (uint8_t)strlen((const char *)ap_cfg.ap.ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
    ap_cfg.ap.ssid_hidden = 0;
    ap_cfg.ap.pmf_cfg.required = false;

    s_sta_connect_enabled = false;
    s_softap_onboarding_active = true;
    s_runtime_status.provisioning_active = true;
    snprintf(s_runtime_status.onboarding_ssid, sizeof(s_runtime_status.onboarding_ssid), "%s", service_name);

    ESP_ERROR_CHECK(esp_wifi_set_mode(force_ap_only ? WIFI_MODE_AP : WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Starting onboarding SoftAP %s%s", service_name, force_ap_only ? " (AP-only safe-start)" : "");
    ESP_LOGI(TAG, "Admin UI available at http://192.168.4.1/");
    return ESP_OK;
}

static esp_err_t start_wifi_provisioning(const espclaw_board_profile_t *profile)
{
#if CONFIG_ESPCLAW_ENABLE_WIFI_PROVISIONING
    wifi_prov_mgr_config_t config = {0};
    bool provisioned = false;
    const bool wants_ble = profile != NULL && profile->supports_ble_provisioning;
    const bool use_ble = wants_ble && ESPCLAW_BLE_PROVISIONING_AVAILABLE;

    if (use_ble) {
#if ESPCLAW_BLE_PROVISIONING_AVAILABLE
        config.scheme = wifi_prov_scheme_ble;
        config.scheme_event_handler = (wifi_prov_event_handler_t)WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM;
#endif
    } else {
        if (wants_ble) {
            ESP_LOGW(TAG, "BLE provisioning requested by board profile but BT is disabled in sdkconfig, using ESPClaw SoftAP onboarding");
            s_runtime_status.profile.provisioning = "softap";
        }
        return start_softap_onboarding(false);
    }
    config.app_event_handler.event_cb = provisioning_event_cb;
    config.app_event_handler.user_data = NULL;
    config.wifi_prov_conn_cfg.wifi_conn_attempts = 0;

    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (!provisioned) {
        char service_name[32];
        espclaw_provisioning_descriptor_t descriptor;
        uint8_t mac[6] = {0};
        esp_read_mac(mac, ESP_MAC_WIFI_STA);
        snprintf(service_name, sizeof(service_name), "ESPClaw-%02X%02X%02X", mac[3], mac[4], mac[5]);
        ESP_LOGI(TAG, "Starting provisioning service %s", service_name);
        snprintf(s_runtime_status.onboarding_ssid, sizeof(s_runtime_status.onboarding_ssid), "%s", service_name);
        if (espclaw_provisioning_build_descriptor(
                true,
                "ble",
                service_name,
                "",
                ESPCLAW_PROVISIONING_POP,
                "",
                &descriptor) == 0) {
            ESP_LOGI(TAG, "BLE provisioning PoP: %s", descriptor.pop);
            if (descriptor.qr_url[0] != '\0') {
                ESP_LOGI(TAG, "BLE provisioning helper URL: %s", descriptor.qr_url);
            }
        }
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(
            WIFI_PROV_SECURITY_1,
            ESPCLAW_PROVISIONING_POP,
            service_name,
            NULL
        ));
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Wi-Fi already provisioned, starting station mode");
    wifi_prov_mgr_deinit();
    s_sta_connect_enabled = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    refresh_wifi_ssid();
    clear_onboarding_state();
    return ESP_OK;
#else
    (void)profile;
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

esp_err_t espclaw_runtime_wifi_scan(
    espclaw_wifi_network_t *networks,
    size_t max_networks,
    size_t *count_out
)
{
    uint16_t ap_count = 0;
    uint16_t desired = 0;
    wifi_scan_config_t scan_config = {0};
    wifi_ap_record_t records[16];
    uint16_t record_count = sizeof(records) / sizeof(records[0]);
    uint16_t index;

    if (count_out == NULL || networks == NULL || max_networks == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    *count_out = 0;
    if (esp_wifi_scan_start(&scan_config, true) != ESP_OK) {
        return ESP_FAIL;
    }
    if (esp_wifi_scan_get_ap_num(&ap_count) != ESP_OK) {
        return ESP_FAIL;
    }

    desired = ap_count < record_count ? ap_count : record_count;
    if (desired == 0) {
        return ESP_OK;
    }
    if (esp_wifi_scan_get_ap_records(&desired, records) != ESP_OK) {
        return ESP_FAIL;
    }

    for (index = 0; index < desired && index < max_networks; ++index) {
        snprintf(networks[index].ssid, sizeof(networks[index].ssid), "%s", (const char *)records[index].ssid);
        networks[index].rssi = records[index].rssi;
        networks[index].channel = records[index].primary;
        networks[index].secure = records[index].authmode != WIFI_AUTH_OPEN;
        (*count_out)++;
    }

    return ESP_OK;
}

esp_err_t espclaw_runtime_wifi_join(
    const char *ssid,
    const char *password,
    char *message,
    size_t message_size
)
{
    wifi_config_t wifi_cfg = {0};

    if (message != NULL && message_size > 0) {
        message[0] = '\0';
    }
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", ssid);
    snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", password != NULL ? password : "");
    wifi_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    if (s_runtime_status.provisioning_active) {
        if (s_ble_provisioning_active) {
            if (wifi_prov_mgr_configure_sta(&wifi_cfg) != ESP_OK) {
                if (message != NULL && message_size > 0) {
                    snprintf(message, message_size, "failed to submit Wi-Fi credentials");
                }
                return ESP_FAIL;
            }
            if (message != NULL && message_size > 0) {
                snprintf(message, message_size, "submitted credentials for %s", ssid);
            }
            snprintf(s_runtime_status.wifi_ssid, sizeof(s_runtime_status.wifi_ssid), "%s", ssid);
            return ESP_OK;
        }
    }

    s_sta_connect_enabled = true;
    if (esp_wifi_set_mode(s_softap_onboarding_active ? WIFI_MODE_APSTA : WIFI_MODE_STA) != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg) != ESP_OK ||
        esp_wifi_connect() != ESP_OK) {
        if (message != NULL && message_size > 0) {
            snprintf(message, message_size, "failed to connect to %s", ssid);
        }
        return ESP_FAIL;
    }

    if (message != NULL && message_size > 0) {
        snprintf(message, message_size, "connecting to %s", ssid);
    }
    snprintf(s_runtime_status.wifi_ssid, sizeof(s_runtime_status.wifi_ssid), "%s", ssid);
    return ESP_OK;
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
    bool saved_wifi_credentials = false;

    memset(&s_runtime_status, 0, sizeof(s_runtime_status));
    s_softap_onboarding_active = false;
    s_ble_provisioning_active = false;
    s_sta_connect_enabled = false;
    s_time_sync_started = false;
    s_time_sync_completed = false;
    memset(&s_sntp_config, 0, sizeof(s_sntp_config));
    s_runtime_status.profile = espclaw_board_profile_for(profile_id);
    espclaw_task_policy_select(&s_runtime_status.profile);
    espclaw_board_configure_current(NULL, &s_runtime_status.profile);
    if (espclaw_system_monitor_init(&s_runtime_status.profile) != 0) {
        return ESP_FAIL;
    }
    if (espclaw_event_watch_runtime_start() != 0) {
        return ESP_FAIL;
    }

    if (s_runtime_event_group == NULL) {
        s_runtime_event_group = xEventGroupCreate();
        if (s_runtime_event_group == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_ERROR_CHECK(init_nvs_flash_store());
    ESP_ERROR_CHECK(init_wifi_stack());
    saved_wifi_credentials = has_saved_sta_credentials();

    if (mount_workspace_storage(&s_runtime_status.profile) != ESP_OK) {
        ESP_LOGW(TAG, "Continuing without workspace storage");
    } else {
        char boot_log[512];
        char behavior_log[512];

        espclaw_board_configure_current(s_runtime_status.workspace_root, &s_runtime_status.profile);
        espclaw_auth_store_init(s_runtime_status.workspace_root);
        if (espclaw_app_run_boot_apps(s_runtime_status.workspace_root, boot_log, sizeof(boot_log)) == 0 &&
            boot_log[0] != '\0') {
            ESP_LOGI(TAG, "Boot apps: %s", boot_log);
        }
        if (espclaw_behavior_start_autostart(s_runtime_status.workspace_root, behavior_log, sizeof(behavior_log)) == 0 &&
            behavior_log[0] != '\0') {
            ESP_LOGI(TAG, "Autostart behaviors: %s", behavior_log);
        }
    }

    if (espclaw_runtime_should_defer_wifi_boot(&s_runtime_status.profile, s_runtime_status.storage_ready)) {
        s_runtime_status.wifi_boot_deferred = true;
        if (saved_wifi_credentials) {
            ESP_LOGW(
                TAG,
                "ESP32-CAM safe-start active: workspace storage is unavailable, but saved Wi-Fi credentials exist. Preferring station boot over AP-only onboarding."
            );
            if (start_softap_onboarding(false) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to start station boot with saved Wi-Fi credentials during safe-start");
            } else {
                ESP_ERROR_CHECK(maybe_start_telegram());
            }
        } else if (espclaw_runtime_should_force_softap_only_boot(
                       &s_runtime_status.profile,
                       s_runtime_status.storage_ready,
                       saved_wifi_credentials)) {
            ESP_LOGW(
                TAG,
                "ESP32-CAM safe-start active: workspace storage is unavailable, deferring Wi-Fi boot. Check SD card media/wiring and board power, then reboot."
            );
            if (start_softap_onboarding(true) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to start AP-only onboarding during safe-start");
            }
        }
    } else {
        ESP_ERROR_CHECK(start_wifi_provisioning(&s_runtime_status.profile));
        ESP_ERROR_CHECK(maybe_start_telegram());
    }

    if (status != NULL) {
        *status = s_runtime_status;
    }
    return ESP_OK;
}

const espclaw_runtime_status_t *espclaw_runtime_status(void)
{
    return &s_runtime_status;
}

bool espclaw_runtime_time_is_sane(void)
{
    return system_time_sane();
}

bool espclaw_runtime_wait_for_time_sync(uint32_t timeout_ms)
{
    if (system_time_sane() && s_time_sync_completed) {
        return true;
    }
    if (!s_time_sync_started) {
        maybe_start_time_sync();
    }
    if (!s_time_sync_started) {
        return false;
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) != ESP_OK) {
        return system_time_sane() && s_time_sync_completed;
    }
    s_time_sync_completed = system_time_sane();
    return system_time_sane();
}

esp_err_t espclaw_runtime_get_provisioning_descriptor(espclaw_provisioning_descriptor_t *descriptor)
{
    return build_runtime_provisioning_descriptor(descriptor);
}
