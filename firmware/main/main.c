#include "espclaw/admin_api.h"
#include "espclaw/admin_server.h"
#include "espclaw/admin_ui.h"
#include "espclaw/config_render.h"
#include "espclaw/log_buffer.h"
#include "espclaw/ota_manager.h"
#include "espclaw/ota_state.h"
#include "espclaw/runtime.h"
#include "espclaw/storage.h"
#include "esp_log.h"

static const char *TAG = "espclaw";
static char s_default_config_buffer[2048];
static char s_status_buffer[512];
static const uint32_t ESPCLAW_OTA_CONFIRM_DELAY_MS = 15000;

void app_main(void)
{
    espclaw_runtime_status_t runtime_status;
    espclaw_ota_snapshot_t ota_snapshot;
    char ota_message[128];
    bool admin_started = false;
    bool operator_surfaces_started = false;
    espclaw_board_profile_id_t profile_id =
#if CONFIG_ESPCLAW_BOARD_PROFILE_ESP32CAM
        ESPCLAW_BOARD_PROFILE_ESP32CAM;
#elif defined(CONFIG_IDF_TARGET_ESP32S3) && CONFIG_IDF_TARGET_ESP32S3
        ESPCLAW_BOARD_PROFILE_ESP32S3;
#else
        ESPCLAW_BOARD_PROFILE_ESP32S3;
#endif
    espclaw_log_buffer_init();
    espclaw_ota_manager_init();
    if (espclaw_runtime_start(profile_id, &runtime_status) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start runtime");
        return;
    }
    if (espclaw_admin_server_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start admin server");
    } else {
        admin_started = true;
    }
    if (espclaw_runtime_start_operator_surfaces() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start operator surfaces");
    } else {
        operator_surfaces_started = true;
    }
    if (admin_started) {
        if (espclaw_ota_manager_schedule_confirm(ESPCLAW_OTA_CONFIRM_DELAY_MS, ota_message, sizeof(ota_message)) == ESP_OK) {
            ESP_LOGI(TAG, "%s", ota_message);
        } else {
            ESP_LOGW(TAG, "%s", ota_message);
        }
    } else {
        ESP_LOGW(TAG, "Deferring OTA confirmation because admin server is unavailable");
    }
    espclaw_ota_manager_snapshot(&ota_snapshot);

    size_t written = espclaw_render_default_config(
        &runtime_status.profile,
        s_default_config_buffer,
        sizeof(s_default_config_buffer)
    );
    size_t status_written = espclaw_render_admin_status_json(
        &runtime_status.profile,
        runtime_status.storage_backend,
        "openai_compat",
        "telegram",
        runtime_status.storage_ready,
        espclaw_runtime_get_yolo_mode(),
        &ota_snapshot.state,
        s_status_buffer,
        sizeof(s_status_buffer)
    );

    ESP_LOGI(
        TAG,
        "Booting ESPClaw profile=%s provisioning=%s storage_backend=%s storage=%d wifi=%d telegram=%d",
        runtime_status.profile.id,
        runtime_status.profile.provisioning,
        espclaw_storage_backend_name(runtime_status.storage_backend),
        runtime_status.storage_ready,
        runtime_status.wifi_ready,
        runtime_status.telegram_ready
    );
    ESP_LOGI(TAG, "Operator surfaces started=%d", operator_surfaces_started);
    ESP_LOGI(TAG, "Admin UI asset size=%u", (unsigned)espclaw_admin_ui_length());
    ESP_LOGI(TAG, "Rendered default config bytes=%u", (unsigned)written);
    ESP_LOGI(TAG, "Rendered admin status bytes=%u", (unsigned)status_written);
}
