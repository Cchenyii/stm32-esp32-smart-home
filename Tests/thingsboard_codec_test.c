#include "gateway_home_codec.h"
#include "cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

#define BYTES(s) s, (sizeof(s) - 1U)

static void topic_ids(void)
{
    uint32_t id;
    assert(gateway_tb_request_id(BYTES(TB_RPC_REQUEST_PREFIX "0"), &id) && id == 0);
    assert(gateway_tb_request_id(BYTES(TB_RPC_REQUEST_PREFIX "4294967295"), &id) && id == UINT32_MAX);
    assert(!gateway_tb_request_id(BYTES(TB_RPC_REQUEST_PREFIX "4294967296"), &id));
    assert(!gateway_tb_request_id(BYTES(TB_RPC_REQUEST_PREFIX "007"), &id));
    assert(!gateway_tb_request_id(BYTES(TB_RPC_REQUEST_PREFIX "4/extra"), &id));
    assert(!gateway_tb_request_id(BYTES(TB_RPC_RESPONSE_PREFIX "4"), &id));
    assert(!gateway_tb_request_id(BYTES(TB_RPC_REQUEST_PREFIX), &id));
}

static void requests(void)
{
    EdgeHomeCommand command = {0};
    assert(gateway_tb_decode_rpc(BYTES("{\"method\":\"getHome\",\"params\":{}}"), &command) == TB_RPC_GET);
    assert(gateway_tb_decode_rpc(BYTES("{\"method\":\"getHome\",\"params\":0}"), &command) == TB_RPC_INVALID);
    assert(gateway_tb_decode_rpc(BYTES("{\"method\":\"off\"}"), &command) == TB_RPC_UNSUPPORTED);
    assert(gateway_tb_decode_rpc(BYTES("{\"method\":\"getHome\"}\0{}"), &command) == TB_RPC_INVALID);
    assert(gateway_tb_decode_rpc(BYTES("{\"method\":\"getHome\",\"method\":\"setHome\"}"), &command) == TB_RPC_INVALID);

    const char valid[] = "{\"method\":\"setHome\",\"params\":{\"session\":2147483648,\"id\":65537,\"op\":2,\"value\":1}}";
    assert(gateway_tb_decode_rpc(BYTES(valid), &command) == TB_RPC_SET);
    assert(command.session == 2147483648U && command.id == 65537U && command.operation == 2 && command.value == 1);
    assert(gateway_tb_decode_rpc(BYTES("{\"method\":\"setHome\",\"params\":{\"session\":0,\"id\":8,\"op\":2,\"value\":1}}"),
                                 &command) == TB_RPC_INVALID);
    assert(gateway_tb_decode_rpc(BYTES("{\"method\":\"setHome\",\"params\":{\"session\":123,\"id\":8,\"op\":2,\"value\":true}}"),
                                 &command) == TB_RPC_INVALID);
    assert(gateway_tb_decode_rpc(BYTES("{\"method\":\"setHome\",\"params\":{\"session\":123,\"id\":8,\"op\":2,\"value\":0.5}}"),
                                 &command) == TB_RPC_INVALID);
    assert(gateway_home_decode_command(BYTES("{\"session\":123,\"id\":8,\"op\":2,\"value\":1}"), &command));
}

static void snapshots(void)
{
    EdgeHomeState state = {
        .session = 42, .flags = EDGE_HOME_MANUAL | EDGE_HOME_TH_VALID | EDGE_HOME_FAN_ON,
        .temperature_tenths = -1, .humidity_tenths = 553, .light_level = 1247,
        .curtain = 1, .curtain_target = 2, .fan_on_tenths = 300, .close_level = 3000
    };
    char buffer[TB_STATE_MAX];
    assert(gateway_tb_state_json(buffer, sizeof buffer, &state, false, 9) > 0);
    cJSON *object = cJSON_Parse(buffer);
    assert(object != NULL);
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, "manual")));
    assert(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(object, "fan_on")));
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(object, "light_valid")));
    assert(cJSON_GetObjectItemCaseSensitive(object, "light_level") == NULL);
    assert(cJSON_GetObjectItemCaseSensitive(object, "session") == NULL);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(object, "temperature_tenths")) == -1);
    cJSON_Delete(object);

    assert(gateway_tb_state_json(buffer, sizeof buffer, &state, true, 9) > 0);
    object = cJSON_Parse(buffer);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(object, "session")) == 42);
    assert(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(object, "last_id")) == 9);
    cJSON_Delete(object);

    assert(gateway_tb_state_json(buffer, sizeof buffer, NULL, true, 9) > 0);
    assert(strcmp(buffer, "{\"uart_online\":false}") == 0);

    EdgeHomeState next = state;
    next.temperature_tenths = 22;
    assert(!gateway_tb_significant_change(&state, &next));
    next.curtain = 2;
    assert(gateway_tb_significant_change(&state, &next));
}

int main(void)
{
    topic_ids();
    requests();
    snapshots();
    puts("ThingsBoard MQTT codec: topic bounds, request validation, telemetry, live snapshot passed");
}
