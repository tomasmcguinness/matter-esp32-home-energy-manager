#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "ethernet_init.h"
#include "mdns.h"

#include "managers/device_manager.h"
#include "managers/node_manager.h"
#include "matter_controller.h"
#include "web_server.h"
#include "sd_card.h"
#include "power_logger.h"

static const char *TAG = "main";

static EventGroupHandle_t s_net_event_group;
#define IPV6_READY_BIT BIT0

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    esp_netif_t *netif = (esp_netif_t *)arg;
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        esp_netif_create_ip6_linklocal(netif);
        break;
    case ETHERNET_EVENT_DISCONNECTED: ESP_LOGI(TAG, "Ethernet link down"); break;
    case ETHERNET_EVENT_START:        ESP_LOGI(TAG, "Ethernet started");   break;
    case ETHERNET_EVENT_STOP:         ESP_LOGI(TAG, "Ethernet stopped");   break;
    }
}

static void got_ip6_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
    ESP_LOGI(TAG, "Got IPv6: " IPV6STR, IPV62STR(event->ip6_info.ip));
    xEventGroupSetBits(s_net_event_group, IPV6_READY_BIT);
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

    //mdns_init();
    // mdns_hostname_set("home-energy-manager");
    // mdns_instance_name_set("Home Energy Manager");
    // mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

extern "C" void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(sd_card_init());

    ESP_ERROR_CHECK(node_manager_init());
    ESP_ERROR_CHECK(device_manager_init());
    ESP_ERROR_CHECK(power_logger_init());

    uint8_t eth_port_cnt = 0;
    esp_eth_handle_t *eth_handles;
    ESP_ERROR_CHECK(example_eth_init(&eth_handles, &eth_port_cnt));

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handles[0])));

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, eth_netif));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, &got_ip6_event_handler, NULL));

    s_net_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_eth_start(eth_handles[0]));

    ESP_LOGI(TAG, "Waiting for IPv6 link-local address...");
    xEventGroupWaitBits(s_net_event_group, IPV6_READY_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));

    // TODO Wait for some updates from esp-matter to ensure the P4 works 
    // correctly before trying to use the Platform mDNS.
    //ESP_ERROR_CHECK(mdns_init());
    //mdns_hostname_set("home-energy-manager");
    //mdns_instance_name_set("Home Energy Manager");
    //mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    ESP_ERROR_CHECK(web_server_start());

    ESP_ERROR_CHECK(matter_controller_start());
    ESP_ERROR_CHECK(matter_controller_subscribe());
}
