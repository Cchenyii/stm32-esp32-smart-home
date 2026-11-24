#include "gateway_home_codec.h"
#include "cJSON.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static cJSON *parse_object(const char *data, size_t length)
{
    if (length == 0 || length > HOME_MQTT_COMMAND_MAX) {
        return NULL;
    }
    char json[HOME_MQTT_COMMAND_MAX + 1];
    memcpy(json, data, length);
    json[length] = '\0';
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, length + 1, &end, true);
    if (!cJSON_IsObject(root) || end != json + length) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

static bool integer_field(const cJSON *object, const char *key, uint32_t max, uint32_t *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) || item->valuedouble < 0.0 ||
        item->valuedouble > (double)max || floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    *value = (uint32_t)item->valuedouble;
    return true;
}

static bool command_fields(const cJSON *root, EdgeHomeCommand *command)
{
    uint32_t session, id, operation, value;
    if (!cJSON_IsObject(root) || cJSON_GetArraySize(root) != 4 ||
        !integer_field(root, "session", UINT32_MAX, &session) ||
        !integer_field(root, "id", UINT32_MAX, &id) ||
        !integer_field(root, "op", UINT8_MAX, &operation) ||
        !integer_field(root, "value", UINT16_MAX, &value) ||
        session == 0U || id == 0U || operation == 0U) {
        return false;
    }
    *command = (EdgeHomeCommand){
        .session = session, .id = id, .value = (uint16_t)value, .operation = (uint8_t)operation
    };
    return true;
}

bool gateway_home_decode_command(const char *data, size_t length, EdgeHomeCommand *command)
{
    cJSON *root = parse_object(data, length);
    bool valid = command_fields(root, command);
    cJSON_Delete(root);
    return valid;
}

bool gateway_tb_request_id(const char *topic, size_t length, uint32_t *id)
{
    const size_t prefix_length = sizeof TB_RPC_REQUEST_PREFIX - 1U;
    if (length <= prefix_length || length > prefix_length + 10U ||
        memcmp(topic, TB_RPC_REQUEST_PREFIX, prefix_length) != 0 ||
        (topic[prefix_length] == '0' && length != prefix_length + 1U)) {
        return false;
    }
    uint32_t value = 0;
    for (size_t i = prefix_length; i < length; ++i) {
        unsigned digit = (unsigned char)topic[i] - '0';
        if (digit > 9U || value > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    *id = value;
    return true;
}

TbRpcMethod gateway_tb_decode_rpc(const char *data, size_t length, EdgeHomeCommand *command)
{
    cJSON *root = parse_object(data, length);
    const cJSON *method = cJSON_GetObjectItemCaseSensitive(root, "method");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(root, "params");
    TbRpcMethod type = TB_RPC_INVALID;
    if (cJSON_IsString(method) && cJSON_GetArraySize(root) == (params != NULL ? 2 : 1)) {
        if (strcmp(method->valuestring, "getHome") == 0) {
            if (params == NULL || cJSON_IsNull(params) ||
                (cJSON_IsObject(params) && cJSON_GetArraySize(params) == 0)) {
                type = TB_RPC_GET;
            }
        } else if (strcmp(method->valuestring, "setHome") == 0) {
            if (command_fields(params, command)) {
                type = TB_RPC_SET;
            }
        } else {
            type = TB_RPC_UNSUPPORTED;
        }
    }
    cJSON_Delete(root);
    return type;
}

static const char *json_bool(bool value)
{
    return value ? "true" : "false";
}

int gateway_tb_state_json(char *out, size_t size, const EdgeHomeState *state,
                          bool for_rpc, uint32_t last_id)
{
    if (state == NULL) {
        return snprintf(out, size, "{\"uart_online\":false}");
    }
    char samples[105] = "";
    char light[46] = "";
    char session[65] = "";
    bool th_valid = (state->flags & EDGE_HOME_TH_VALID) != 0U;
    bool light_valid = (state->flags & EDGE_HOME_LIGHT_VALID) != 0U;
    if (th_valid) {
        (void)snprintf(samples, sizeof samples, ",\"temperature_tenths\":%d,\"humidity_tenths\":%u",
                       (int)state->temperature_tenths, (unsigned)state->humidity_tenths);
    }
    if (light_valid) {
        (void)snprintf(light, sizeof light, ",\"light_level\":%u,\"light_raw\":%u",
                       (unsigned)state->light_level, (unsigned)state->light_raw);
    }
    if (for_rpc) {
        (void)snprintf(session, sizeof session, ",\"session\":%" PRIu32 ",\"last_id\":%" PRIu32,
                       state->session, last_id);
    }
    return snprintf(out, size, "{\"uart_online\":true,\"manual\":%s,\"fan_on\":%s,"
                    "\"curtain\":%u,\"curtain_target\":%u,\"fan_on_tenths\":%u,\"close_level\":%u,"
                    "\"temperature_valid\":%s,\"light_valid\":%s,\"dht_flags\":%u,\"source\":%u%s%s%s}",
                    json_bool((state->flags & EDGE_HOME_MANUAL) != 0U),
                    json_bool((state->flags & EDGE_HOME_FAN_ON) != 0U),
                    (unsigned)state->curtain, (unsigned)state->curtain_target,
                    (unsigned)state->fan_on_tenths, (unsigned)state->close_level,
                    json_bool(th_valid), json_bool(light_valid), (unsigned)state->dht_flags,
                    (unsigned)state->source, samples, light, session);
}

bool gateway_tb_significant_change(const EdgeHomeState *a, const EdgeHomeState *b)
{
    return a->flags != b->flags || a->curtain != b->curtain || a->curtain_target != b->curtain_target ||
           a->fan_on_tenths != b->fan_on_tenths || a->close_level != b->close_level ||
           a->dht_flags != b->dht_flags || a->source != b->source;
}
