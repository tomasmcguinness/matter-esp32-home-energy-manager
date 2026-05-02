#include "matter_controller.h"

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_matter_controller_client.h>
#include <esp_matter_controller_console.h>

static const char *TAG = "matter_controller";

static void app_event_cb(const chip::DeviceLayer::ChipDeviceEvent *event, intptr_t)
{
    if (event->Type == chip::DeviceLayer::DeviceEventType::kCommissioningComplete) {
        ESP_LOGI(TAG, "kCommissioningComplete");
    }
}

esp_err_t matter_controller_start(void)
{
    #if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::controller_register_commands();
    esp_matter::console::init();
    #endif // CONFIG_ENABLE_CHIP_SHELL

    esp_err_t err = esp_matter::start(app_event_cb);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_matter::start failed: 0x%x", err);
        return err;
    }

    auto &controller_instance = esp_matter::controller::matter_controller_client::get_instance();

    ESP_LOGI(TAG, "Matter controller started");

    return ESP_OK;
}
