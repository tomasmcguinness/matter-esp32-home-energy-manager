idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_netif esp_eth esp_event nvs_flash ethernet_init)
