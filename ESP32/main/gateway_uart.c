#include "gateway.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include <string.h>

static const char *const tag = "edge-uart";
enum { UART_NUMBER = UART_NUM_2 };

static void enqueue(Gateway *gateway, uint8_t type, uint16_t seq,
                    const uint8_t *payload, uint16_t length)
{
    GatewayMessage message = {.type = type, .seq = seq, .length = length};
    memcpy(message.bytes, payload, length);
    switch (type) {
    case EDGE_SENSOR:
        xQueueOverwrite(gateway->sensor, &message);
        break;
    case EDGE_STATUS:
        xQueueOverwrite(gateway->status, &message);
        break;
    case EDGE_ERROR:
        xQueueOverwrite(gateway->errors, &message);
        break;
    case EDGE_RESPONSE:
        if (xQueueSend(gateway->replies, &message, 0) != pdTRUE) {
            atomic_fetch_add(&gateway->uart_errors, 1);
        }
        break;
    case EDGE_HOME_STATE:
        if (gateway->home_state != NULL) {
            xQueueOverwrite(gateway->home_state, &message);
        }
        break;
    case EDGE_HOME_ACK:
        if (gateway->home_ack != NULL && xQueueSend(gateway->home_ack, &message, 0) != pdTRUE) {
            atomic_fetch_add(&gateway->uart_errors, 1);
        }
        break;
    default:
        break;
    }
}

static void receive_frame(const EdgeFrame *frame, void *context)
{
    Gateway *gateway = context;
    const uint8_t lengths[] = {
        [EDGE_SENSOR] = EDGE_SENSOR_LEN, [EDGE_STATUS] = EDGE_STATUS_LEN,
        [EDGE_HEARTBEAT] = EDGE_HEARTBEAT_LEN, [EDGE_RESPONSE] = EDGE_RESPONSE_LEN,
        [EDGE_ERROR] = EDGE_ERROR_LEN, [EDGE_HOME_STATE] = EDGE_HOME_STATE_LEN,
        [EDGE_HOME_ACK] = EDGE_HOME_ACK_LEN
    };
    if (frame->type >= sizeof lengths || lengths[frame->type] == 0 ||
        lengths[frame->type] != frame->length) {
        atomic_fetch_add(&gateway->uart_errors, 1);
        return;
    }
    atomic_store(&gateway->last_stm_ms, gateway_ms());
    atomic_store(&gateway->stm_seen, true);
    enqueue(gateway, frame->type, frame->sequence, frame->payload, frame->length);
#ifdef CONFIG_EDGE_HOME_MQTT
    if (frame->type == EDGE_HOME_STATE && CONFIG_EDGE_WIFI_SSID[0] == '\0') {
        static uint32_t last_report_ms;
        uint32_t now = gateway_ms();
        if (now - last_report_ms >= 3000U) {
            EdgeHomeState state = edge_home_decode_state(frame->payload);
            ESP_LOGI(tag, "STM32 home seq=%u flags=%u temp_x10=%d humidity_x10=%u light=%u",
                     (unsigned)frame->sequence, (unsigned)state.flags, (int)state.temperature_tenths,
                     (unsigned)state.humidity_tenths, (unsigned)state.light_level);
            last_report_ms = now;
        }
    }
#endif
    if (frame->type == EDGE_SENSOR &&
        (CONFIG_EDGE_WIFI_SSID[0] == '\0' || CONFIG_EDGE_SERVER_IPV4[0] == '\0')) {
        const uint8_t *data = frame->payload;
        ESP_LOGI(tag, "STM32 seq=%u flags=0x%02x temp_x10=%d humidity_x10=%u distance_mm=%u",
                 (unsigned)frame->sequence, (unsigned)edge_u16(data + 6),
                 (int)(int16_t)edge_u16(data), (unsigned)edge_u16(data + 2),
                 (unsigned)edge_u16(data + 4));
    }
}

static void uart_send(Gateway *gateway, uint8_t type, uint16_t sequence,
                      const uint8_t *payload, uint16_t length)
{
    uint8_t frame[EDGE_MAX_FRAME];
    size_t count = edge_frame_encode(frame, type, sequence, payload, length);
    if (uart_write_bytes(UART_NUMBER, frame, count) != (int)count) {
        atomic_fetch_add(&gateway->uart_errors, 1);
    }
}

static void uart_worker(void *context)
{
    Gateway *gateway = context;
    QueueHandle_t events = NULL;
    uart_config_t configuration = {
        .baud_rate = EDGE_BAUD, .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE, .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT
    };
    if (uart_param_config(UART_NUMBER, &configuration) != ESP_OK ||
        uart_set_pin(UART_NUMBER, CONFIG_EDGE_UART_TX, CONFIG_EDGE_UART_RX,
                      UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                      UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK ||
        uart_driver_install(UART_NUMBER, 1024, 512, 16, &events, 0) != ESP_OK) {
        ESP_LOGE(tag, "UART2 initialization failed");
        vTaskDelete(NULL);
        return;
    }
    EdgeParser parser = {0};
    uint16_t sequence = 0;
    uint32_t heartbeat = gateway_ms();
    for (;;) {
        uart_event_t event;
        if (xQueueReceive(events, &event, pdMS_TO_TICKS(20)) == pdTRUE) {
            if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL ||
                event.type == UART_FRAME_ERR || event.type == UART_PARITY_ERR) {
                (void)uart_flush_input(UART_NUMBER);
                xQueueReset(events);
                parser.used = 0;
                atomic_fetch_add(&gateway->uart_errors, 1);
            }
        }
        uint8_t bytes[128];
        for (unsigned batch = 0; batch < 8; ++batch) {
            int count = uart_read_bytes(UART_NUMBER, bytes, sizeof bytes, 0);
            if (count <= 0) {
                edge_parser_expire(&parser, gateway_ms());
                break;
            }
            edge_parser_feed(&parser, bytes, (size_t)count, gateway_ms(), receive_frame, gateway);
        }
        atomic_store(&gateway->uart_crc_errors, parser.bad_crc);
        if (gateway->home_tx != NULL) {
            GatewayMessage outbound;
            if (xQueueReceive(gateway->home_tx, &outbound, 0) == pdTRUE &&
                edge_u32(outbound.bytes) == atomic_load(&gateway->home_session) &&
                gateway_wifi_ready()) {
                uart_send(gateway, outbound.type, outbound.seq, outbound.bytes, outbound.length);
            }
            continue;
        }
        GatewayCommand command;
        if (xQueueReceive(gateway->commands, &command, 0) == pdTRUE) {
            uint8_t payload[EDGE_COMMAND_LEN];
            edge_put32(payload, command.id);
            payload[4] = command.operation;
            edge_put32(payload + 5, command.value);
            uart_send(gateway, EDGE_COMMAND, sequence++, payload, sizeof payload);
        }
        uint32_t now = gateway_ms();
        if (now - heartbeat >= 2000U) {
            uint8_t payload[EDGE_HEARTBEAT_LEN];
            edge_put32(payload, now);
            uart_send(gateway, EDGE_HEARTBEAT, sequence++, payload, sizeof payload);
            heartbeat = now;
        }
    }
}

static void fake_worker(void *context)
{
    Gateway *gateway = context;
    uint32_t period = EDGE_PERIOD_DEFAULT;
    uint32_t now = gateway_ms();
    uint32_t last_sample = now - period;
    uint32_t last_status = now - 5000U;
    uint16_t sequence = 0;
    for (;;) {
        GatewayCommand command;
        if (xQueueReceive(gateway->commands, &command, pdMS_TO_TICKS(40)) == pdTRUE) {
            uint8_t result = EDGE_OK;
            if (command.operation == EDGE_SET_PERIOD) {
                if (command.value < EDGE_PERIOD_MIN || command.value > EDGE_PERIOD_MAX) {
                    result = EDGE_INVALID;
                } else {
                    period = command.value;
                }
            } else {
                last_status = gateway_ms() - 5000U;
            }
            uint8_t response[EDGE_RESPONSE_LEN];
            edge_put32(response, command.id);
            response[4] = command.operation;
            response[5] = result;
            edge_put32(response + 6, period);
            enqueue(gateway, EDGE_RESPONSE, sequence++, response, sizeof response);
        }
        now = gateway_ms();
        if (now - last_sample >= period) {
            uint8_t payload[EDGE_SENSOR_LEN] = {0};
            edge_put16(payload, (uint16_t)(240U + sequence % 15U));
            edge_put16(payload + 2, (uint16_t)(540U + sequence % 25U));
            edge_put16(payload + 4, (uint16_t)(700U + sequence % 50U));
            edge_put16(payload + 6, EDGE_TH_VALID | EDGE_DISTANCE_VALID);
            edge_put32(payload + 8, now);
            enqueue(gateway, EDGE_SENSOR, sequence++, payload, sizeof payload);
            last_sample = now;
        }
        if (now - last_status >= 5000U) {
            uint8_t payload[EDGE_STATUS_LEN] = {0};
            edge_put32(payload, now);
            edge_put32(payload + 4, period);
            enqueue(gateway, EDGE_STATUS, sequence++, payload, sizeof payload);
            last_status = now;
        }
    }
}

static bool module_pin(int pin)
{
    switch (pin) {
    case 0: case 2: case 5: case 12: case 15: /* Boot strapping. */
    case 1: case 3:                           /* Console UART. */
    case 6: case 7: case 8: case 9: case 10: case 11: /* Flash. */
        return false;
    default:
        return pin >= 0;
    }
}

void gateway_uart_start(Gateway *gateway)
{
    TaskFunction_t worker = fake_worker;
#ifndef CONFIG_EDGE_SIMULATOR
    if (!module_pin(CONFIG_EDGE_UART_TX) || !module_pin(CONFIG_EDGE_UART_RX) ||
        CONFIG_EDGE_UART_TX == CONFIG_EDGE_UART_RX ||
        !GPIO_IS_VALID_OUTPUT_GPIO(CONFIG_EDGE_UART_TX) || !GPIO_IS_VALID_GPIO(CONFIG_EDGE_UART_RX)) {
        ESP_LOGW(tag, "UART disabled until the confirmed TX/RX pins are configured");
        return;
    }
    worker = uart_worker;
#else
    (void)module_pin;
    (void)uart_worker;
    ESP_LOGI(tag, "Simulation mode");
#endif
    if (xTaskCreate(worker, "edge-uart", 4096, gateway, 7, NULL) != pdPASS) {
        ESP_LOGE(tag, "Worker creation failed");
    }
}
