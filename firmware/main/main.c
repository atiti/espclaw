#include "espclaw/admin_api.h"
#include "espclaw/admin_server.h"
#include "espclaw/admin_ui.h"
#include "espclaw/config_render.h"
#include "espclaw/ota_state.h"
#include "espclaw/runtime.h"
#include "esp_log.h"

static const char *TAG = "espclaw";

void app_main(void)
{
    char config_buffer[2048];
    char status_buffer[512];
    espclaw_ota_state_t ota_state = espclaw_ota_state_init();
    espclaw_runtime_status_t runtime_status;
    espclaw_board_profile_id_t profile_id =
#if CONFIG_ESPCLAW_BOARD_PROFILE_ESP32CAM
        ESPCLAW_BOARD_PROFILE_ESP32CAM;
#else
        ESPCLAW_BOARD_PROFILE_ESP32S3;
#endif
    if (espclaw_runtime_start(profile_id, &runtime_status) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start runtime");
        return;
    }
    if (espclaw_admin_server_start() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start admin server");
    }

    size_t written = espclaw_render_default_config(&runtime_status.profile, config_buffer, sizeof(config_buffer));
    size_t status_written = espclaw_render_admin_status_json(
        &runtime_status.profile,
        "openai_compat",
        "telegram",
        runtime_status.storage_ready,
        &ota_state,
        status_buffer,
        sizeof(status_buffer)
    );

    ESP_LOGI(
        TAG,
        "Booting ESPClaw profile=%s provisioning=%s storage=%d wifi=%d telegram=%d",
        runtime_status.profile.id,
        runtime_status.profile.provisioning,
        runtime_status.storage_ready,
        runtime_status.wifi_ready,
        runtime_status.telegram_ready
    );
    ESP_LOGI(TAG, "Admin UI asset size=%u", (unsigned)espclaw_admin_ui_length());
    ESP_LOGI(TAG, "Rendered default config bytes=%u", (unsigned)written);
    ESP_LOGI(TAG, "Rendered admin status bytes=%u", (unsigned)status_written);
}
