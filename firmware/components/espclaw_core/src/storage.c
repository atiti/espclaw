#include "espclaw/storage.h"

#include <stdio.h>
#include <string.h>

#include "espclaw/workspace.h"

const char *espclaw_storage_describe_workspace_root(const char *workspace_root)
{
    if (workspace_root == NULL || workspace_root[0] == '\0') {
        return "";
    }
    if (strncmp(workspace_root, "/sdcard/", 8) == 0 || strcmp(workspace_root, "/sdcard") == 0) {
        return "sdcard";
    }
    if (strncmp(workspace_root, "/workspace", 10) == 0) {
        return "littlefs";
    }
    return "host";
}

#ifdef ESP_PLATFORM

#include "driver/spi_common.h"
#include "driver/spi_master.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_default_configs.h"
#include "driver/sdmmc_host.h"
#endif
#include "driver/sdspi_host.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "soc/soc_caps.h"

#ifndef CONFIG_ESPCLAW_SD_MOUNT_POINT
#define CONFIG_ESPCLAW_SD_MOUNT_POINT "/sdcard"
#endif

#ifndef CONFIG_ESPCLAW_SD_SPI_MOSI
#define CONFIG_ESPCLAW_SD_SPI_MOSI -1
#endif

#ifndef CONFIG_ESPCLAW_SD_SPI_MISO
#define CONFIG_ESPCLAW_SD_SPI_MISO -1
#endif

#ifndef CONFIG_ESPCLAW_SD_SPI_SCLK
#define CONFIG_ESPCLAW_SD_SPI_SCLK -1
#endif

#ifndef CONFIG_ESPCLAW_SD_SPI_CS
#define CONFIG_ESPCLAW_SD_SPI_CS -1
#endif

#ifndef CONFIG_ESPCLAW_FLASH_MOUNT_POINT
#define CONFIG_ESPCLAW_FLASH_MOUNT_POINT "/workspace"
#endif

#ifndef CONFIG_ESPCLAW_FLASH_PARTITION_LABEL
#define CONFIG_ESPCLAW_FLASH_PARTITION_LABEL "workspace"
#endif

static const char *TAG = "espclaw_storage";

espclaw_storage_backend_t espclaw_storage_backend_for_profile(const espclaw_board_profile_t *profile)
{
#if defined(CONFIG_ESPCLAW_STORAGE_BACKEND_SD)
    (void)profile;
    return ESPCLAW_STORAGE_BACKEND_SD_CARD;
#elif defined(CONFIG_ESPCLAW_STORAGE_BACKEND_LITTLEFS)
    (void)profile;
    return ESPCLAW_STORAGE_BACKEND_LITTLEFS;
#else
    if (profile == NULL) {
        return ESPCLAW_STORAGE_BACKEND_SD_CARD;
    }
    return profile->default_storage_backend;
#endif
}

static esp_err_t mount_sd_workspace(const espclaw_board_profile_t *profile, espclaw_storage_mount_t *mount)
{
    char workspace_root[sizeof(mount->workspace_root)];

    if (profile == NULL || mount == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

#if !CONFIG_ESPCLAW_ENABLE_SD_BOOTSTRAP
    return ESP_ERR_NOT_SUPPORTED;
#else
    if (profile->profile_id == ESPCLAW_BOARD_PROFILE_ESP32CAM) {
#if SOC_SDMMC_HOST_SUPPORTED
        sdmmc_host_t host = SDMMC_HOST_DEFAULT();
        sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 8,
            .allocation_unit_size = 16 * 1024,
        };
        sdmmc_card_t *card = NULL;

        slot_config.width = 1;
        host.max_freq_khz = SDMMC_FREQ_DEFAULT;
        if (esp_vfs_fat_sdmmc_mount(CONFIG_ESPCLAW_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card) != ESP_OK) {
            ESP_LOGW(TAG, "SDMMC mount failed");
            return ESP_FAIL;
        }
#else
        ESP_LOGW(TAG, "Board profile %s requires SDMMC host support, which this target does not provide", profile->id);
        return ESP_ERR_NOT_SUPPORTED;
#endif
    } else if (CONFIG_ESPCLAW_SD_SPI_MOSI >= 0 &&
               CONFIG_ESPCLAW_SD_SPI_MISO >= 0 &&
               CONFIG_ESPCLAW_SD_SPI_SCLK >= 0 &&
               CONFIG_ESPCLAW_SD_SPI_CS >= 0) {
        spi_bus_config_t bus_cfg = {
            .mosi_io_num = CONFIG_ESPCLAW_SD_SPI_MOSI,
            .miso_io_num = CONFIG_ESPCLAW_SD_SPI_MISO,
            .sclk_io_num = CONFIG_ESPCLAW_SD_SPI_SCLK,
            .quadwp_io_num = -1,
            .quadhd_io_num = -1,
            .max_transfer_sz = 16 * 1024,
        };
        sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
        sdmmc_host_t host = SDSPI_HOST_DEFAULT();
        esp_vfs_fat_sdmmc_mount_config_t mount_config = {
            .format_if_mount_failed = false,
            .max_files = 8,
            .allocation_unit_size = 16 * 1024,
        };
        sdmmc_card_t *card = NULL;
        esp_err_t err;

        host.slot = SPI2_HOST;
        slot_config.gpio_cs = CONFIG_ESPCLAW_SD_SPI_CS;
        slot_config.host_id = host.slot;
        err = spi_bus_initialize(host.slot, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "SDSPI bus init failed");
            return ESP_FAIL;
        }
        if (esp_vfs_fat_sdspi_mount(CONFIG_ESPCLAW_SD_MOUNT_POINT, &host, &slot_config, &mount_config, &card) != ESP_OK) {
            spi_bus_free(host.slot);
            ESP_LOGW(TAG, "SDSPI mount failed");
            return ESP_FAIL;
        }
    } else {
        ESP_LOGW(TAG, "No SD pin configuration set for board profile %s", profile->id);
        return ESP_FAIL;
    }
#endif

    snprintf(workspace_root, sizeof(workspace_root), "%s/workspace", CONFIG_ESPCLAW_SD_MOUNT_POINT);
    if (espclaw_workspace_bootstrap(workspace_root) != 0) {
        ESP_LOGE(TAG, "Failed to bootstrap SD workspace at %s", workspace_root);
        return ESP_FAIL;
    }

    memset(mount, 0, sizeof(*mount));
    mount->backend = ESPCLAW_STORAGE_BACKEND_SD_CARD;
    snprintf(mount->workspace_root, sizeof(mount->workspace_root), "%s", workspace_root);
    return ESP_OK;
}

static esp_err_t mount_littlefs_workspace(espclaw_storage_mount_t *mount)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = CONFIG_ESPCLAW_FLASH_MOUNT_POINT,
        .partition_label = CONFIG_ESPCLAW_FLASH_PARTITION_LABEL,
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err;

    if (mount == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "LittleFS mount failed for partition %s", CONFIG_ESPCLAW_FLASH_PARTITION_LABEL);
        return err;
    }

    if (espclaw_workspace_bootstrap(CONFIG_ESPCLAW_FLASH_MOUNT_POINT) != 0) {
        ESP_LOGE(TAG, "Failed to bootstrap LittleFS workspace at %s", CONFIG_ESPCLAW_FLASH_MOUNT_POINT);
        return ESP_FAIL;
    }

    memset(mount, 0, sizeof(*mount));
    mount->backend = ESPCLAW_STORAGE_BACKEND_LITTLEFS;
    snprintf(mount->workspace_root, sizeof(mount->workspace_root), "%s", CONFIG_ESPCLAW_FLASH_MOUNT_POINT);
    if (esp_littlefs_info(CONFIG_ESPCLAW_FLASH_PARTITION_LABEL, &mount->total_bytes, &mount->used_bytes) != ESP_OK) {
        mount->total_bytes = 0;
        mount->used_bytes = 0;
    }
    return ESP_OK;
}

esp_err_t espclaw_storage_mount_workspace(const espclaw_board_profile_t *profile, espclaw_storage_mount_t *mount)
{
    espclaw_storage_backend_t backend = espclaw_storage_backend_for_profile(profile);

    if (backend == ESPCLAW_STORAGE_BACKEND_LITTLEFS) {
        return mount_littlefs_workspace(mount);
    }
    return mount_sd_workspace(profile, mount);
}

#endif
