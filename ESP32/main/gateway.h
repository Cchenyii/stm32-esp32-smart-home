#ifndef EDGE_GATEWAY_H
#define EDGE_GATEWAY_H

#include "edge_wire.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdatomic.h>

#define GATEWAY_JSON_MAX 768U

typedef struct {
    uint8_t type;
    uint16_t seq;
    uint16_t length;
    uint8_t bytes[EDGE_MAX_PAYLOAD];
} GatewayMessage;

typedef struct {
    uint32_t id;
    uint8_t operation;
    uint32_t value;
} GatewayCommand;

typedef struct {
    QueueHandle_t sensor, status, errors, replies, commands;
    QueueHandle_t home_tx, home_state, home_ack;
    atomic_uint last_stm_ms;
    atomic_bool stm_seen;
    atomic_uint uart_errors;
    atomic_uint uart_crc_errors;
    atomic_uint home_session;
} Gateway;

uint32_t gateway_ms(void);
void gateway_uart_start(Gateway *gateway);
bool gateway_net_start(Gateway *gateway);
bool gateway_wifi_ready(void);
bool gateway_mqtt_start(Gateway *gateway);
bool gateway_decode(const char *line, size_t bytes, GatewayCommand *command, bool *is_pong);
bool gateway_serialize(char out[GATEWAY_JSON_MAX], const GatewayMessage *message);
bool gateway_response(char out[GATEWAY_JSON_MAX], GatewayCommand command, unsigned result, uint32_t value);

#endif
