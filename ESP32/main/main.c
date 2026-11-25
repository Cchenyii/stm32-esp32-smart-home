#include "gateway.h"
#include "ota_service.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include <string.h>

static Gateway gateway;

uint32_t gateway_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static bool valid_id(const char *id)
{
    size_t length = strlen(id);
    return length > 0U && length <= 31U &&
           strspn(id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-") == length;
}

void app_main(void)
{
    esp_err_t ota_result = ota_service_start();
    if (ota_result != ESP_OK) {
        ESP_LOGE("edge", "OTA service initialization failed: %s", esp_err_to_name(ota_result));
    }
    if (!valid_id(CONFIG_EDGE_DEVICE_ID)) {
        ESP_LOGE("edge", "Invalid device ID");
        return;
    }
    gateway.sensor = xQueueCreate(1, sizeof(GatewayMessage));
    gateway.status = xQueueCreate(1, sizeof(GatewayMessage));
    gateway.errors = xQueueCreate(1, sizeof(GatewayMessage));
    gateway.replies = xQueueCreate(8, sizeof(GatewayMessage));
    gateway.commands = xQueueCreate(4, sizeof(GatewayCommand));
#ifdef CONFIG_EDGE_HOME_MQTT
    gateway.home_tx = xQueueCreate(4, sizeof(GatewayMessage));
    gateway.home_state = xQueueCreate(1, sizeof(GatewayMessage));
    gateway.home_ack = xQueueCreate(4, sizeof(GatewayMessage));
    if (!gateway.home_tx || !gateway.home_state || !gateway.home_ack) {
        ESP_LOGE("edge", "Smart-home queue allocation failed");
        return;
    }
#endif
    if (!gateway.sensor || !gateway.status || !gateway.errors || !gateway.replies || !gateway.commands) {
        ESP_LOGE("edge", "Queue allocation failed");
        return;
    }
    gateway_uart_start(&gateway);
#ifndef CONFIG_EDGE_HOME_THINGSBOARD
    if (CONFIG_EDGE_SERVER_IPV4[0] == '\0') {
        ESP_LOGW("edge", "Configure device ID, Wi-Fi and PC IPv4 under Edge monitoring terminal");
        return;
    }
#endif
    if (CONFIG_EDGE_WIFI_SSID[0] == '\0') {
        ESP_LOGW("edge", "Configure the Wi-Fi SSID under Edge monitoring terminal");
        return;
    }
    if (!gateway_net_start(&gateway)) {
        ESP_LOGE("edge", "Wi-Fi/network startup failed");
    } else {
        ota_result = ota_service_network_ready();
        if (ota_result != ESP_OK) {
            ESP_LOGE("edge", "OTA HTTP initialization failed: %s", esp_err_to_name(ota_result));
        }
    }
}
