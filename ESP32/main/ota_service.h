#pragma once
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef CONFIG_EDGE_OTA
esp_err_t ota_service_start(void);
void ota_service_uart_alive(void);
esp_err_t ota_service_network_ready(void);
#else
static inline esp_err_t ota_service_start(void) { return ESP_OK; }
static inline void ota_service_uart_alive(void) {}
static inline esp_err_t ota_service_network_ready(void) { return ESP_OK; }
#endif
