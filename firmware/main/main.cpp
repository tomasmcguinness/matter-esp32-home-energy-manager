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
#include "esp_sntp.h"
#include "lwip/netif.h"
#include "lwip/nd6.h"
#include "lwip/mld6.h"
#include "ping/ping_sock.h"

#include "managers/device_manager.h"
#include "managers/node_manager.h"
#include "matter_controller.h"
#include "web_server.h"
#include "sd_card.h"
#include "solar_forecast.h"
#include "node_power_logger.h"

#include "esp_netif_net_stack.h"

static const char *TAG = "main";

static void log_ipv6_state(void);

static EventGroupHandle_t s_net_event_group;
#define IPV6_READY_BIT  BIT0
#define SNTP_SYNCED_BIT BIT1

static void time_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "SNTP sync complete: %lld", (long long)tv->tv_sec);
    bool first = !(xEventGroupGetBits(s_net_event_group) & SNTP_SYNCED_BIT);
    xEventGroupSetBits(s_net_event_group, SNTP_SYNCED_BIT);
    // On the first sync, kick the forecast catch-up. Handles the late-sync case where
    // the boot wait timed out and solar_forecast_start_daily_job() already skipped.
    if (first)
        solar_forecast_on_time_synced();
}

static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    esp_netif_t *netif = (esp_netif_t *)arg;
    struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(netif);

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Ethernet link up");
        netif_set_flags(lwip_netif, NETIF_FLAG_MLD6);
        esp_netif_create_ip6_linklocal(netif);
        break;
    case ETHERNET_EVENT_DISCONNECTED: ESP_LOGI(TAG, "Ethernet link down"); break;
    case ETHERNET_EVENT_START:        ESP_LOGI(TAG, "Ethernet started");   break;
    case ETHERNET_EVENT_STOP:         ESP_LOGI(TAG, "Ethernet stopped");   break;
    }
}

// TEST: join the all-nodes multicast group (ff02::1) via MLD. Must run inside
// the TCPIP context (core lock held), so it's invoked via esp_netif_tcpip_exec.
// Nodes normally never send an MLD report for ff02::1, so an MLD-snooping switch
// prunes it on our port and we never receive multicast RAs. If this join makes
// the OTBR's RA start arriving in nd6_input, MLD snooping is the cause.
static esp_err_t join_all_nodes_cb(void *ctx)
{
    struct netif *lwip_netif = (struct netif *)ctx;
    ip6_addr_t allnodes;
    IP6_ADDR(&allnodes, PP_HTONL(0xff020000), PP_HTONL(0x00000000),
             PP_HTONL(0x00000000), PP_HTONL(0x00000001));
    ip6_addr_assign_zone(&allnodes, IP6_MULTICAST, lwip_netif);
    err_t err = mld6_joingroup_netif(lwip_netif, &allnodes);
    return (err == ERR_OK) ? ESP_OK : ESP_FAIL;
}

static void got_ip6_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    ip_event_got_ip6_t *event = (ip_event_got_ip6_t *)event_data;
    ESP_LOGI(TAG, "Got IPv6: " IPV6STR, IPV62STR(event->ip6_info.ip));

    struct netif *lwip_netif = (struct netif *)esp_netif_get_netif_impl(event->esp_netif);
    if (lwip_netif != NULL) {
        esp_err_t err = esp_netif_tcpip_exec(join_all_nodes_cb, lwip_netif);
        ESP_LOGW(TAG, "mld6_joingroup ff02::1 -> %s", esp_err_to_name(err));
    }

    log_ipv6_state();

    xEventGroupSetBits(s_net_event_group, IPV6_READY_BIT);
}

static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));

    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    tzset();

    sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    ESP_LOGI(TAG, "SNTP started");
}

static void log_ipv6_state(void)
{
    ESP_LOGI(TAG, "=== IPv6 Network State ===");

    struct netif *netif;
    NETIF_FOREACH(netif) {
        ESP_LOGI(TAG, "Interface %c%c%d:", netif->name[0], netif->name[1], netif->num);
        for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
            if (ip6_addr_isvalid(netif_ip6_addr_state(netif, i))) {
                ESP_LOGI(TAG, "  addr[%d]: %s", i, ip6addr_ntoa(netif_ip6_addr(netif, i)));
            }
        }
    }

    // Probe whether LwIP has a route to the Thread mesh prefix (fd43:2a42:8b75:1::/64).
    // nd6_find_route() returns the outbound netif if a route exists, NULL otherwise.
    ip6_addr_t thread_dest;
    IP6_ADDR(&thread_dest, PP_HTONL(0xfd432a42), PP_HTONL(0x8b750001),
             PP_HTONL(0x00000000), PP_HTONL(0x00000001));
    struct netif *route_netif = nd6_find_route(&thread_dest);
    if (route_netif != NULL) {
        ESP_LOGI(TAG, "Route to Thread prefix fd43:2a42:8b75:1::/64 found via %c%c%d",
                 route_netif->name[0], route_netif->name[1], route_netif->num);
    } else {
        ESP_LOGW(TAG, "No route to Thread prefix fd43:2a42:8b75:1::/64 — commissioning will fail");
    }

    ESP_LOGI(TAG, "==========================");
}

static void on_ping_success(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    uint32_t elapsed_ms;
    uint8_t ttl;
    ip_addr_t addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO,   &seqno,      sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed_ms, sizeof(elapsed_ms));
    esp_ping_get_profile(hdl, ESP_PING_PROF_TTL,     &ttl,        sizeof(ttl));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR,  &addr,       sizeof(addr));
    ESP_LOGI(TAG, "Ping reply from %s: seq=%d time=%dms ttl=%d",
             ipaddr_ntoa(&addr), seqno, elapsed_ms, ttl);
}

static void on_ping_timeout(esp_ping_handle_t hdl, void *args)
{
    uint16_t seqno;
    ip_addr_t addr;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO,  &seqno, sizeof(seqno));
    esp_ping_get_profile(hdl, ESP_PING_PROF_IPADDR, &addr,  sizeof(addr));
    ESP_LOGW(TAG, "Ping timeout to %s seq=%d", ipaddr_ntoa(&addr), seqno);
}

static void on_ping_end(esp_ping_handle_t hdl, void *args)
{
    uint32_t sent, received, total_ms;
    esp_ping_get_profile(hdl, ESP_PING_PROF_REQUEST,  &sent,     sizeof(sent));
    esp_ping_get_profile(hdl, ESP_PING_PROF_REPLY,    &received, sizeof(received));
    esp_ping_get_profile(hdl, ESP_PING_PROF_DURATION, &total_ms, sizeof(total_ms));
    ESP_LOGI(TAG, "Ping complete: sent=%d received=%d time=%dms", sent, received, total_ms);
    esp_ping_delete_session(hdl);
}

static void ping_thread_device(void)
{
    // fd43:2a42:8b75:1:35f1:c73a:1eb6:9dac — Thread device from commissioning logs
    ip6_addr_t addr6;
    IP6_ADDR(&addr6, PP_HTONL(0xfd432a42), PP_HTONL(0x8b750001),
                     PP_HTONL(0x35f1c73a), PP_HTONL(0x1eb69dac));

    ip_addr_t target;
    ip_addr_copy_from_ip6(target, addr6);

    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr  = target;
    cfg.count        = 4;
    cfg.interval_ms  = 1000;
    cfg.timeout_ms   = 2000;

    esp_ping_callbacks_t cbs = {
        .on_ping_success = on_ping_success,
        .on_ping_timeout = on_ping_timeout,
        .on_ping_end     = on_ping_end,
    };

    esp_ping_handle_t ping;
    esp_err_t err = esp_ping_new_session(&cfg, &cbs, &ping);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create ping session: %s", esp_err_to_name(err));
        return;
    }
    esp_ping_start(ping);
}

extern "C" void app_main(void)
{
    //esp_log_level_set("lwip", ESP_LOG_DEBUG);
    //esp_log_level_set("ip6", ESP_LOG_DEBUG);
    //esp_log_level_set("icmp6", ESP_LOG_DEBUG);
    //esp_log_level_set("nd6", ESP_LOG_DEBUG);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(sd_card_init());

    ESP_ERROR_CHECK(node_manager_init());
    ESP_ERROR_CHECK(device_manager_init());
    matter_controller_seed_value_cache();
    ESP_ERROR_CHECK(node_power_logger_init());

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

    ESP_LOGI(TAG, "Waiting for IPv6 addresses...");
    xEventGroupWaitBits(s_net_event_group, IPV6_READY_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(15000));
    log_ipv6_state();
    //ping_thread_device();

    ESP_LOGI(TAG, "Waiting for SNTP sync...");
    xEventGroupWaitBits(s_net_event_group, SNTP_SYNCED_BIT, pdFALSE, pdTRUE, pdMS_TO_TICKS(10000));
    if (!(xEventGroupGetBits(s_net_event_group) & SNTP_SYNCED_BIT))
        ESP_LOGW(TAG, "SNTP sync timed out — time may be incorrect");

    // Schedule the daily 2 AM forecast job. If SNTP hasn't synced yet, the job and its
    // boot catch-up self-skip until the wall clock is valid (see solar_forecast.c).
    ESP_ERROR_CHECK(solar_forecast_start_daily_job());

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
