idf_component_register(SRCS
        "main.cpp"
        "web_server.c"
        "settings_store.c"
        "matter_controller.cpp"
        "managers/device_manager.cpp"
        "managers/node_config_manager.cpp"
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS  "." "${ESP_MATTER_PATH}/examples/common/utils"
    LDFRAGMENTS "linker.lf"    
)

if(CONFIG_SPIFFS_ATTESTATION_TRUST_STORE)
    spiffs_create_partition_image(paa_cert ${CMAKE_SOURCE_DIR}/paa_cert FLASH_IN_PROJECT)
endif()

add_custom_target(react_build
    COMMAND npm run build --emptyOutDir
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/html_app
    COMMENT "Building React app..."
)

spiffs_create_partition_image(storage "${CMAKE_SOURCE_DIR}/html_compiled_app" FLASH_IN_PROJECT DEPENDS react_build)

# idf_component_get_property(matter_lib espressif__esp_matter COMPONENT_LIB)
# set_source_files_properties(
#     "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_matter/connectedhomeip/connectedhomeip/src/platform/ESP32/NetworkCommissioningDriver_Ethernet.cpp"
#     TARGET_DIRECTORY ${matter_lib}
#     PROPERTIES HEADER_FILE_ONLY TRUE
# )

# # esp-matter's CMakeLists excludes ESP32DnssdImpl.cpp when Wi-Fi is disabled, but
# # DnssdImpl.cpp (included when CONFIG_USE_MINIMAL_MDNS=n) depends on its symbols.
# # Add it back so platform mDNS links on Ethernet-only builds.
# target_sources(${matter_lib} PRIVATE
#     "${CMAKE_SOURCE_DIR}/managed_components/espressif__esp_matter/connectedhomeip/connectedhomeip/src/platform/ESP32/ESP32DnssdImpl.cpp"
# )

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")