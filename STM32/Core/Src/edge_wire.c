#include "edge_wire.h"

#include <string.h>

enum { SOF_0 = 0xAA, SOF_1 = 0x55, VERSION = 1, HEADER_LEN = 8 };

uint16_t edge_u16(const uint8_t *bytes)
{
    return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

uint32_t edge_u32(const uint8_t *bytes)
{
    return (uint32_t)edge_u16(bytes) | ((uint32_t)edge_u16(bytes + 2) << 16);
}

void edge_put16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

void edge_put32(uint8_t *bytes, uint32_t value)
{
    edge_put16(bytes, (uint16_t)value);
    edge_put16(bytes + 2, (uint16_t)(value >> 16));
}

void edge_home_encode_command(uint8_t data[EDGE_HOME_COMMAND_LEN], EdgeHomeCommand command)
{
    edge_put32(data, command.session);
    edge_put32(data + 4, command.id);
    data[8] = command.operation;
    edge_put16(data + 9, command.value);
}

EdgeHomeCommand edge_home_decode_command(const uint8_t data[EDGE_HOME_COMMAND_LEN])
{
    return (EdgeHomeCommand){
        .session = edge_u32(data), .id = edge_u32(data + 4),
        .operation = data[8], .value = edge_u16(data + 9)
    };
}

void edge_home_encode_ack(uint8_t data[EDGE_HOME_ACK_LEN], EdgeHomeAck ack)
{
    edge_put32(data, ack.session);
    edge_put32(data + 4, ack.id);
    data[8] = ack.operation;
    data[9] = ack.result;
}

EdgeHomeAck edge_home_decode_ack(const uint8_t data[EDGE_HOME_ACK_LEN])
{
    return (EdgeHomeAck){
        .session = edge_u32(data), .id = edge_u32(data + 4),
        .operation = data[8], .result = data[9]
    };
}

void edge_home_encode_state(uint8_t data[EDGE_HOME_STATE_LEN], EdgeHomeState state)
{
    edge_put32(data, state.session);
    edge_put32(data + 4, state.uptime_ms);
    edge_put16(data + 8, (uint16_t)state.temperature_tenths);
    edge_put16(data + 10, state.humidity_tenths);
    edge_put16(data + 12, state.light_raw);
    edge_put16(data + 14, state.light_level);
    edge_put16(data + 16, state.fan_on_tenths);
    edge_put16(data + 18, state.close_level);
    edge_put16(data + 20, state.dht_flags);
    data[22] = state.flags;
    data[23] = state.curtain;
    data[24] = state.curtain_target;
    data[25] = state.source;
}

EdgeHomeState edge_home_decode_state(const uint8_t data[EDGE_HOME_STATE_LEN])
{
    uint16_t temperature = edge_u16(data + 8);
    return (EdgeHomeState){
        .session = edge_u32(data), .uptime_ms = edge_u32(data + 4),
        .temperature_tenths = (int16_t)(temperature < 32768U ? temperature : (int32_t)temperature - 65536),
        .humidity_tenths = edge_u16(data + 10), .light_raw = edge_u16(data + 12),
        .light_level = edge_u16(data + 14), .fan_on_tenths = edge_u16(data + 16),
        .close_level = edge_u16(data + 18), .dht_flags = edge_u16(data + 20),
        .flags = data[22], .curtain = data[23], .curtain_target = data[24], .source = data[25]
    };
}

uint16_t edge_crc16(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFU;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (uint16_t)((crc << 1) ^ ((crc & 0x8000U) ? 0x1021U : 0U));
        }
    }
    return crc;
}

size_t edge_frame_encode(uint8_t output[EDGE_MAX_FRAME], uint8_t type,
                         uint16_t sequence, const uint8_t *payload, uint16_t length)
{
    if (length > EDGE_MAX_PAYLOAD || (length != 0U && payload == NULL)) {
        return 0;
    }
    output[0] = SOF_0;
    output[1] = SOF_1;
    output[2] = VERSION;
    output[3] = type;
    edge_put16(output + 4, sequence);
    edge_put16(output + 6, length);
    if (length != 0U) {
        memcpy(output + HEADER_LEN, payload, length);
    }
    edge_put16(output + HEADER_LEN + length, edge_crc16(output + 2, 6U + length));
    return HEADER_LEN + length + 2U;
}

static void discard(EdgeParser *parser, size_t count)
{
    parser->used -= count;
    memmove(parser->buffer, parser->buffer + count, parser->used);
}

static void parse(EdgeParser *parser, EdgeFrameHandler handler, void *context)
{
    uint8_t *bytes = parser->buffer;
    while (parser->used != 0U) {
        if (bytes[0] != SOF_0 || (parser->used >= 2U && bytes[1] != SOF_1)) {
            discard(parser, 1);
            continue;
        }
        if (parser->used < HEADER_LEN) {
            return;
        }
        uint16_t length = edge_u16(bytes + 6);
        if (bytes[2] != VERSION || length > EDGE_MAX_PAYLOAD) {
            ++parser->bad_header;
            discard(parser, 1);
            continue;
        }
        size_t size = HEADER_LEN + length + 2U;
        if (parser->used < size) {
            return;
        }
        if (edge_crc16(bytes + 2, 6U + length) != edge_u16(bytes + HEADER_LEN + length)) {
            ++parser->bad_crc;
            discard(parser, 1);
            continue;
        }
        const EdgeFrame frame = {bytes[3], edge_u16(bytes + 4), length, bytes + HEADER_LEN};
        handler(&frame, context);
        discard(parser, size);
    }
}

void edge_parser_expire(EdgeParser *parser, uint32_t now_ms)
{
    if (parser->used != 0U && now_ms - parser->last_byte_ms >= EDGE_INTERBYTE_MS) {
        parser->used = 0;
        ++parser->timeout;
    }
}

void edge_parser_feed(EdgeParser *parser, const uint8_t *data, size_t length,
                      uint32_t now_ms, EdgeFrameHandler handler, void *context)
{
    edge_parser_expire(parser, now_ms);
    while (length-- != 0U) {
        parser->buffer[parser->used++] = *data++;
        parser->last_byte_ms = now_ms;
        parse(parser, handler, context);
    }
}
