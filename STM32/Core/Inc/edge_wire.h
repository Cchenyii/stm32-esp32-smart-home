#ifndef EDGE_WIRE_H
#define EDGE_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EDGE_BAUD 115200U
#define EDGE_MAX_PAYLOAD 64U
#define EDGE_MAX_FRAME (10U + EDGE_MAX_PAYLOAD)
#define EDGE_INTERBYTE_MS 100U
#define EDGE_PERIOD_MIN 2000U
#define EDGE_PERIOD_MAX 60000U
#define EDGE_PERIOD_DEFAULT 3000U

enum EdgeType {
    EDGE_SENSOR = 1, EDGE_STATUS, EDGE_HEARTBEAT, EDGE_COMMAND,
    EDGE_RESPONSE, EDGE_ERROR, EDGE_HOME_SYNC, EDGE_HOME_STATE,
    EDGE_HOME_COMMAND, EDGE_HOME_ACK
};

enum EdgeCommand { EDGE_SET_PERIOD = 1, EDGE_GET_STATUS };
enum EdgeResult {
    EDGE_OK = 0, EDGE_INVALID, EDGE_UNSUPPORTED, EDGE_BUSY,
    EDGE_MANUAL_REQUIRED, EDGE_LOCAL_PRIORITY, EDGE_STALE, EDGE_DEFERRED
};
enum EdgeFlags {
    EDGE_TH_VALID = 1U, EDGE_DISTANCE_VALID = 2U,
    EDGE_DHT_TIMEOUT = 4U, EDGE_DHT_BAD = 8U, EDGE_ECHO_TIMEOUT = 16U,
    EDGE_ECHO_RANGE = 32U, EDGE_DHT_DISABLED = 64U, EDGE_ECHO_DISABLED = 128U
};

enum {
    EDGE_SENSOR_LEN = 16, EDGE_STATUS_LEN = 36, EDGE_HEARTBEAT_LEN = 4,
    EDGE_COMMAND_LEN = 9, EDGE_RESPONSE_LEN = 10, EDGE_ERROR_LEN = 6
};

enum EdgeHomeOperation {
    EDGE_HOME_SET_MODE = 1, EDGE_HOME_SET_FAN, EDGE_HOME_SET_CURTAIN,
    EDGE_HOME_SET_FAN_THRESHOLD, EDGE_HOME_SET_LIGHT_THRESHOLD
};

enum EdgeHomeFlags {
    EDGE_HOME_TH_VALID = 1, EDGE_HOME_LIGHT_VALID = 2,
    EDGE_HOME_MANUAL = 4, EDGE_HOME_FAN_ON = 8
};

enum {
    EDGE_HOME_SYNC_LEN = 4, EDGE_HOME_STATE_LEN = 26,
    EDGE_HOME_COMMAND_LEN = 11, EDGE_HOME_ACK_LEN = 10
};

typedef struct {
    uint32_t session, id;
    uint16_t value;
    uint8_t operation;
} EdgeHomeCommand;

typedef struct {
    uint32_t session, id;
    uint8_t operation, result;
} EdgeHomeAck;

typedef struct {
    uint32_t session, uptime_ms;
    int16_t temperature_tenths;
    uint16_t humidity_tenths, light_raw, light_level, fan_on_tenths, close_level, dht_flags;
    uint8_t flags, curtain, curtain_target, source;
} EdgeHomeState;

typedef struct {
    uint8_t type;
    uint16_t sequence;
    uint16_t length;
    const uint8_t *payload;
} EdgeFrame;

typedef struct {
    uint8_t buffer[EDGE_MAX_FRAME];
    size_t used;
    uint32_t last_byte_ms;
    uint32_t bad_header;
    uint32_t bad_crc;
    uint32_t timeout;
} EdgeParser;

typedef void (*EdgeFrameHandler)(const EdgeFrame *frame, void *context);

uint16_t edge_u16(const uint8_t *bytes);
uint32_t edge_u32(const uint8_t *bytes);
void edge_put16(uint8_t *bytes, uint16_t value);
void edge_put32(uint8_t *bytes, uint32_t value);
uint16_t edge_crc16(const uint8_t *data, size_t length);
size_t edge_frame_encode(uint8_t output[EDGE_MAX_FRAME], uint8_t type,
                         uint16_t sequence, const uint8_t *payload, uint16_t length);
void edge_parser_expire(EdgeParser *parser, uint32_t now_ms);
void edge_parser_feed(EdgeParser *parser, const uint8_t *data, size_t length,
                      uint32_t now_ms, EdgeFrameHandler handler, void *context);
void edge_home_encode_command(uint8_t payload[EDGE_HOME_COMMAND_LEN], EdgeHomeCommand command);
EdgeHomeCommand edge_home_decode_command(const uint8_t payload[EDGE_HOME_COMMAND_LEN]);
void edge_home_encode_ack(uint8_t payload[EDGE_HOME_ACK_LEN], EdgeHomeAck ack);
EdgeHomeAck edge_home_decode_ack(const uint8_t payload[EDGE_HOME_ACK_LEN]);
void edge_home_encode_state(uint8_t payload[EDGE_HOME_STATE_LEN], EdgeHomeState state);
EdgeHomeState edge_home_decode_state(const uint8_t payload[EDGE_HOME_STATE_LEN]);

#endif
