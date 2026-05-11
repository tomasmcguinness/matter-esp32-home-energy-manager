#include "sd_card.h"

#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "esp_idf_version.h"

static const char *TAG = "sd_card";

// ESP32-P4-Module-DevKit: SD card on SDMMC slot 0 (ESP-Hosted occupies slot 1)
#define SD_SLOT          SDMMC_HOST_SLOT_0
#define SD_PIN_CLK       43
#define SD_PIN_CMD       44
#define SD_PIN_D0        39
#define SD_PIN_D1        40
#define SD_PIN_D2        41
#define SD_PIN_D3        42
#define SD_PIN_PWR_RESET 45
#define SD_LDO_CHAN_ID   4

// On IDF >= 6.0, ESP-Hosted already owns the SDMMC host controller init/deinit.
// Providing dummy functions avoids a double-init error.
#if CONFIG_ESP_HOSTED_SDIO_HOST_INTERFACE && (ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(6, 0, 0))
static esp_err_t sdmmc_host_init_dummy(void)   { return ESP_OK; }
static esp_err_t sdmmc_host_deinit_dummy(void) { return ESP_OK; }
#define HOSTED_OWNS_HOST_INIT 1
#else
#define HOSTED_OWNS_HOST_INIT 0
#endif

static sdmmc_card_t *s_card = NULL;
static sd_pwr_ctrl_handle_t s_pwr_ctrl = NULL;

esp_err_t sd_card_init(void)
{
    esp_err_t ret;

    // Power-cycle the card via the on-board reset GPIO
    gpio_config_t pwr_io = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = (1ULL << SD_PIN_PWR_RESET),
    };
    ESP_ERROR_CHECK(gpio_config(&pwr_io));
    gpio_set_level(SD_PIN_PWR_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    gpio_set_level(SD_PIN_PWR_RESET, 0);

    // The SD card VDD on the P4 DevKit is supplied through internal LDO channel 4.
    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = SD_LDO_CHAN_ID };
    ret = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr_ctrl);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "LDO power ctrl init failed: %s", esp_err_to_name(ret));
        return ret;
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SD_SLOT;
    host.pwr_ctrl_handle = s_pwr_ctrl;
#if HOSTED_OWNS_HOST_INIT
    host.init   = sdmmc_host_init_dummy;
    host.deinit = sdmmc_host_deinit_dummy;
#endif

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk   = SD_PIN_CLK;
    slot.cmd   = SD_PIN_CMD;
    slot.d0    = SD_PIN_D0;
    slot.d1    = SD_PIN_D1;
    slot.d2    = SD_PIN_D2;
    slot.d3    = SD_PIN_D3;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 10,
        .allocation_unit_size   = 16 * 1024,
    };

    ret = esp_vfs_fat_sdmmc_mount(SD_CARD_MOUNT_POINT, &host, &slot, &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        sd_pwr_ctrl_del_on_chip_ldo(s_pwr_ctrl);
        s_pwr_ctrl = NULL;
        return ret;
    }

    ESP_LOGI(TAG, "SD card mounted at " SD_CARD_MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}
