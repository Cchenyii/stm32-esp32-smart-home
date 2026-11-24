#include "edge_wire.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint16_t sequence[512];
    unsigned count;
} Observed;

static void observe(const EdgeFrame *frame, void *context)
{
    Observed *out = context;
    assert(frame->type == EDGE_COMMAND);
    assert(frame->length == EDGE_COMMAND_LEN);
    assert(edge_u32(frame->payload) == 12345U);
    assert(frame->payload[4] == EDGE_SET_PERIOD);
    assert(edge_u32(frame->payload + 5) == 5000U);
    assert(out->count < sizeof out->sequence / sizeof out->sequence[0]);
    out->sequence[out->count++] = frame->sequence;
}

static size_t build(uint8_t *frame, uint16_t sequence)
{
    uint8_t command[EDGE_COMMAND_LEN];
    edge_put32(command, 12345U);
    command[4] = EDGE_SET_PERIOD;
    edge_put32(command + 5, 5000U);
    return edge_frame_encode(frame, EDGE_COMMAND, sequence, command, sizeof command);
}

static void chunk_boundaries(void)
{
    uint8_t bytes[EDGE_MAX_FRAME * 2U];
    size_t first = build(bytes, 0x34AAU);
    size_t length = first + build(bytes + first, UINT16_MAX);
    assert(bytes[0] == 0xAA && bytes[1] == 0x55 && bytes[2] == 1);
    assert(bytes[4] == 0xAA && bytes[5] == 0x34 && bytes[6] == EDGE_COMMAND_LEN && bytes[7] == 0);
    for (size_t split = 0; split <= length; ++split) {
        EdgeParser parser = {0};
        Observed seen = {0};
        edge_parser_feed(&parser, bytes, split, 10, observe, &seen);
        edge_parser_feed(&parser, bytes + split, length - split, 15, observe, &seen);
        assert(seen.count == 2 && seen.sequence[0] == 0x34AAU && seen.sequence[1] == UINT16_MAX);
        assert(parser.bad_crc == 0 && parser.bad_header == 0 && parser.used == 0);
    }
}

static void bad_frames(void)
{
    uint8_t command[EDGE_MAX_FRAME], outer[EDGE_MAX_FRAME];
    size_t length = build(command, 81U);
    size_t wrapping = edge_frame_encode(outer, EDGE_STATUS, 0, command, (uint16_t)length);
    outer[wrapping - 1] ^= 0x80U;
    const uint8_t no_frame[] = {1, 2, 3, 0xAA, 0xAA, 0x55, 1, 4, 0, 0, 0xFF, 0xFF};
    EdgeParser parser = {0};
    Observed seen = {0};
    edge_parser_feed(&parser, no_frame, sizeof no_frame, 1, observe, &seen);
    assert(parser.bad_header == 1 && seen.count == 0);
    edge_parser_feed(&parser, outer, wrapping, 2, observe, &seen);
    assert(parser.bad_crc == 1 && seen.count == 1 && seen.sequence[0] == 81U);
    edge_parser_feed(&parser, command, 6, UINT32_MAX - 50U, observe, &seen);
    edge_parser_expire(&parser, 60);
    assert(parser.used == 0 && parser.timeout == 1);
    edge_parser_feed(&parser, command, length, 62, observe, &seen);
    assert(seen.count == 2);
}

static void repeated_frames(void)
{
    EdgeParser parser = {0};
    Observed seen = {0};
    uint32_t prng = 71U;
    for (uint16_t i = 0; i < 512; ++i) {
        uint8_t frame[EDGE_MAX_FRAME];
        size_t length = build(frame, i);
        for (size_t done = 0; done < length;) {
            prng = 1664525U * prng + 1013904223U;
            size_t chunk = 1U + prng % 7U;
            chunk = chunk > length - done ? length - done : chunk;
            edge_parser_feed(&parser, frame + done, chunk, i, observe, &seen);
            done += chunk;
        }
    }
    assert(seen.count == 512);
    for (uint16_t i = 0; i < 512; ++i) {
        assert(seen.sequence[i] == i);
    }
}

static void home_payloads(void)
{
    uint8_t command[EDGE_HOME_COMMAND_LEN];
    edge_home_encode_command(command, (EdgeHomeCommand){
        .session = 0x01020304U, .id = 65537U, .value = 3300, .operation = EDGE_HOME_SET_LIGHT_THRESHOLD
    });
    const uint8_t expected[] = {4, 3, 2, 1, 1, 0, 1, 0, 5, 0xe4, 0x0c};
    assert(memcmp(command, expected, sizeof expected) == 0);
    EdgeHomeCommand parsed = edge_home_decode_command(command);
    assert(parsed.session == 0x01020304U && parsed.id == 65537U);
    assert(parsed.value == 3300 && parsed.operation == EDGE_HOME_SET_LIGHT_THRESHOLD);

    uint8_t ack_bytes[EDGE_HOME_ACK_LEN];
    edge_home_encode_ack(ack_bytes, (EdgeHomeAck){
        .session = parsed.session, .id = parsed.id, .operation = parsed.operation, .result = EDGE_DEFERRED
    });
    EdgeHomeAck ack = edge_home_decode_ack(ack_bytes);
    assert(ack.session == parsed.session && ack.id == parsed.id);
    assert(ack.operation == parsed.operation && ack.result == EDGE_DEFERRED);

    uint8_t state[EDGE_HOME_STATE_LEN] = {0};
    edge_home_encode_state(state, (EdgeHomeState){
        .session = parsed.session, .uptime_ms = UINT32_MAX, .temperature_tenths = -123,
        .humidity_tenths = 456, .light_raw = 1500, .light_level = 2595,
        .fan_on_tenths = 310, .close_level = 3200, .dht_flags = EDGE_TH_VALID,
        .flags = EDGE_HOME_TH_VALID | EDGE_HOME_LIGHT_VALID | EDGE_HOME_MANUAL,
        .curtain = 1, .curtain_target = 2, .source = 2
    });
    EdgeHomeState report = edge_home_decode_state(state);
    assert(report.session == parsed.session && report.uptime_ms == UINT32_MAX);
    assert(report.temperature_tenths == -123 && report.humidity_tenths == 456);
    assert(report.light_raw == 1500 && report.light_level == 2595);
    assert(report.fan_on_tenths == 310 && report.close_level == 3200 && report.dht_flags == EDGE_TH_VALID);
    assert(report.flags == 7 && report.curtain == 1 && report.curtain_target == 2 && report.source == 2);
}

int main(void)
{
    assert(edge_crc16((const uint8_t *)"123456789", 9) == 0x29B1U);
    chunk_boundaries();
    bad_frames();
    repeated_frames();
    home_payloads();
    puts("UART codec: split points, noise, CRC, timeout, 512 frames, smart-home wire fixtures passed");
}
