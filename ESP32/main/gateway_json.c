#include "gateway.h"
#include "cJSON.h"
#include "sdkconfig.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#define PREFIX "{\"v\":1,\"device_id\":\"" CONFIG_EDGE_DEVICE_ID "\","

static bool uint32_field(const cJSON *item, uint32_t *value)
{
    if (!cJSON_IsNumber(item) || !(item->valuedouble >= 0 && item->valuedouble <= UINT32_MAX)) {
        return false;
    }
    uint32_t integer = (uint32_t)item->valuedouble;
    if (item->valuedouble != (double)integer) {
        return false;
    }
    *value = integer;
    return true;
}

static bool flat_object(const char *line, size_t bytes)
{
    unsigned objects = 0;
    bool in_string = false;
    for (size_t i = 0; i < bytes; ++i) {
        char c = line[i];
        if (c == '\0') {
            return false;
        }
        if (in_string && c == '\\') {
            if (++i >= bytes || line[i] == '\0') {
                return false;
            }
        } else if (c == '"') {
            in_string = !in_string;
        } else if (!in_string && (c == '[' || (c == '{' && ++objects > 1U))) {
            return false;
        }
    }
    return objects == 1U;
}

bool gateway_decode(const char *line, size_t bytes, GatewayCommand *command, bool *is_pong)
{
    *is_pong = false;
    if (!flat_object(line, bytes)) {
        return false;
    }
    cJSON *object = cJSON_ParseWithLengthOpts(line, bytes + 1U, NULL, true);
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(object, "type");
    uint32_t version;
    bool valid = false;
    if (!cJSON_IsObject(object) || !cJSON_IsString(type) ||
        !uint32_field(cJSON_GetObjectItemCaseSensitive(object, "v"), &version) || version != 1U) {
        goto done;
    }
    if (strcmp(type->valuestring, "pong") == 0) {
        *is_pong = true;
        valid = true;
        goto done;
    }
    if (strcmp(type->valuestring, "command") != 0 ||
        !uint32_field(cJSON_GetObjectItemCaseSensitive(object, "rid"), &command->id) ||
        command->id == 0U) {
        goto done;
    }
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(object, "op");
    if (cJSON_IsString(op) && strcmp(op->valuestring, "get_status") == 0) {
        command->operation = EDGE_GET_STATUS;
        command->value = 0;
        valid = true;
    } else if (cJSON_IsString(op) && strcmp(op->valuestring, "set_period") == 0) {
        command->operation = EDGE_SET_PERIOD;
        valid = uint32_field(cJSON_GetObjectItemCaseSensitive(object, "period_ms"), &command->value);
    }
done:
    cJSON_Delete(object);
    return valid;
}

static bool written(int count)
{
    return count > 0 && (size_t)count < GATEWAY_JSON_MAX;
}

bool gateway_response(char out[GATEWAY_JSON_MAX], GatewayCommand command, unsigned result, uint32_t value)
{
    return written(snprintf(out, GATEWAY_JSON_MAX,
        PREFIX "\"type\":\"response\",\"rid\":%" PRIu32 ",\"op\":\"%s\","
               "\"result\":%u,\"period_ms\":%" PRIu32 "}\n",
        command.id, command.operation == EDGE_SET_PERIOD ? "set_period" : "get_status", result, value));
}

bool gateway_serialize(char out[GATEWAY_JSON_MAX], const GatewayMessage *message)
{
    const uint8_t *p = message->bytes;
    switch (message->type) {
    case EDGE_SENSOR: {
        unsigned flags = edge_u16(p + 6);
        char temperature[16] = "null", humidity[16] = "null", distance[16] = "null";
        if ((flags & EDGE_TH_VALID) != 0U) {
            snprintf(temperature, sizeof temperature, "%.1f", (int16_t)edge_u16(p) / 10.0);
            snprintf(humidity, sizeof humidity, "%.1f", edge_u16(p + 2) / 10.0);
        }
        if ((flags & EDGE_DISTANCE_VALID) != 0U) {
            snprintf(distance, sizeof distance, "%.1f", edge_u16(p + 4) / 10.0);
        }
        return written(snprintf(out, GATEWAY_JSON_MAX,
            PREFIX "\"type\":\"sensor\",\"seq\":%u,\"temperature\":%s,\"humidity\":%s,"
                   "\"distance_cm\":%s,\"flags\":%u,\"uptime_ms\":%" PRIu32 ",\"drops\":%" PRIu32 "}\n",
            (unsigned)message->seq, temperature, humidity, distance, flags,
            edge_u32(p + 8), edge_u32(p + 12)));
    }
    case EDGE_STATUS:
        return written(snprintf(out, GATEWAY_JSON_MAX,
            PREFIX "\"type\":\"status\",\"seq\":%u,\"uptime_ms\":%" PRIu32 ",\"period_ms\":%" PRIu32 ","
                   "\"bad_crc\":%" PRIu32 ",\"bad_frames\":%" PRIu32 ",\"uart_errors\":%" PRIu32 ","
                   "\"oled_errors\":%" PRIu32 ",\"drops\":%" PRIu32 ",\"reset_flags\":%" PRIu32 ","
                   "\"stack_words\":%u,\"board_state\":%u,\"watchdog\":%u}\n",
            (unsigned)message->seq, edge_u32(p), edge_u32(p+4), edge_u32(p+8), edge_u32(p+12),
            edge_u32(p+16), edge_u32(p+20), edge_u32(p+24), edge_u32(p+28),
            (unsigned)edge_u16(p+32), (unsigned)p[34], (unsigned)p[35]));
    case EDGE_ERROR:
        return written(snprintf(out, GATEWAY_JSON_MAX,
            PREFIX "\"type\":\"error\",\"seq\":%u,\"code\":%u,\"uptime_ms\":%" PRIu32 "}\n",
            (unsigned)message->seq, (unsigned)edge_u16(p), edge_u32(p+2)));
    default:
        return false;
    }
}
