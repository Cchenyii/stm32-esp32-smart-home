#ifndef GATEWAY_HOME_CODEC_H
#define GATEWAY_HOME_CODEC_H

#include "edge_wire.h"

#define TB_RPC_REQUEST_PREFIX "v1/devices/me/rpc/request/"
#define TB_RPC_RESPONSE_PREFIX "v1/devices/me/rpc/response/"
#define TB_TELEMETRY_TOPIC "v1/devices/me/telemetry"

enum { HOME_MQTT_COMMAND_MAX = 192, TB_STATE_MAX = 512 };

typedef enum {
    TB_RPC_INVALID, TB_RPC_UNSUPPORTED, TB_RPC_GET, TB_RPC_SET
} TbRpcMethod;

bool gateway_home_decode_command(const char *data, size_t length, EdgeHomeCommand *command);
bool gateway_tb_request_id(const char *topic, size_t length, uint32_t *id);
TbRpcMethod gateway_tb_decode_rpc(const char *data, size_t length, EdgeHomeCommand *command);
/* A null state means the STM32 has no current UART snapshot. */
int gateway_tb_state_json(char *out, size_t size, const EdgeHomeState *state,
                          bool for_rpc, uint32_t last_id);
bool gateway_tb_significant_change(const EdgeHomeState *a, const EdgeHomeState *b);

#endif
