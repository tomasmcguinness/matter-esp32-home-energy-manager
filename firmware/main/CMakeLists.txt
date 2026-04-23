idf_component_register(SRCS "main.c" "web_server.c" "settings_store.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_netif esp_eth esp_event nvs_flash esp_http_server spiffs json joltwallet__littlefs ethernet_init espressif__mdns)

spiffs_create_partition_image(storage "${CMAKE_SOURCE_DIR}/html_compiled_app" FLASH_IN_PROJECT)
