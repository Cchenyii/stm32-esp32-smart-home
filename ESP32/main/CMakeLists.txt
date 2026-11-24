set(wire_dir "../../STM32/Core")
idf_component_register(
    SRCS "main.c" "gateway_uart.c" "gateway_net.c" "gateway_json.c" "gateway_mqtt.c"
         "gateway_home_codec.c" "${wire_dir}/Src/edge_wire.c"
    INCLUDE_DIRS "." "${wire_dir}/Inc"
    PRIV_REQUIRES esp_event esp_netif esp_wifi esp_timer esp_hw_support nvs_flash esp_driver_uart esp_driver_gpio lwip mbedtls
)
