set(wire_dir "../../STM32/Core")
set(ota_sources "")
set(ota_assets "")
if(CONFIG_EDGE_OTA)
    if(NOT CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE OR NOT CONFIG_PARTITION_TABLE_CUSTOM)
        message(FATAL_ERROR "EDGE_OTA requires rollback support and the custom OTA partition table.")
    endif()
    set(ota_sources "ota_service.c")
    set(ota_assets "ota_page.html")
endif()
idf_component_register(
    SRCS "main.c" "gateway_uart.c" "gateway_net.c" "gateway_json.c" "gateway_mqtt.c"
         "gateway_home_codec.c" "${wire_dir}/Src/edge_wire.c" ${ota_sources}
    INCLUDE_DIRS "." "${wire_dir}/Inc"
    PRIV_REQUIRES esp_event esp_netif esp_wifi esp_timer esp_hw_support nvs_flash esp_driver_uart esp_driver_gpio lwip mbedtls
                  app_update bootloader_support esp_app_format esp_http_server
    EMBED_TXTFILES ${ota_assets}
)
if(CONFIG_EDGE_OTA)
    target_link_options(${COMPONENT_LIB} INTERFACE "LINKER:--undefined=edge_ota_descriptor")
endif()
