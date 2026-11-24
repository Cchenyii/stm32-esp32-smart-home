#include "gateway.h"
#include "sdkconfig.h"

#ifdef CONFIG_EDGE_HOME_MQTT

#include "gateway_home_codec.h"
#ifdef CONFIG_EDGE_HOME_THINGSBOARD
#include "esp_crt_bundle.h"
#endif
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

enum {
    SYNC_MS = 1000, UART_STALE_MS = 2000, RETRY_MS = 350, MAX_ATTEMPTS = 3,
    TB_REPORT_MS = 30000, TB_MIN_REPORT_MS = 1000,
    MAX_QUEUED_REQUEST_MS = 3000, HOME_UNAVAILABLE = 8, HOME_TIMEOUT = 9
};

typedef struct {
    EdgeHomeCommand command;
    uint32_t rpc_id;
    uint32_t received_ms;
    unsigned generation;
    TbRpcMethod method;
} MqttRequest;

typedef struct {
    MqttRequest request;
    uint8_t result;
    bool confirmed;
} Receipt;

typedef struct {
    uint32_t session, sync_ms, state_ms, sent_ms;
    bool ready;
    unsigned attempts;
    MqttRequest pending;
    Receipt last;
#ifdef CONFIG_EDGE_HOME_THINGSBOARD
    EdgeHomeState snapshot, reported;
    uint32_t reported_ms;
    bool telemetry_sent;
#endif
} HomeSession;

static struct {
    Gateway *gateway;
    esp_mqtt_client_handle_t client;
    QueueHandle_t requests;
    atomic_bool subscribed;
    atomic_uint generation;
    char command[68], state[68], ack[68], availability[68], client_id[40];
} bridge;

static bool parse_request(esp_mqtt_event_handle_t event, MqttRequest *request)
{
    if (event->retain || event->current_data_offset != 0 || event->topic_len <= 0 ||
        event->data_len < 0 || event->data_len != event->total_data_len ||
        event->data_len > HOME_MQTT_COMMAND_MAX) {
        return false;
    }
    request->generation = atomic_load(&bridge.generation);
    request->received_ms = gateway_ms();
#ifdef CONFIG_EDGE_HOME_THINGSBOARD
    if (!gateway_tb_request_id(event->topic, (size_t)event->topic_len, &request->rpc_id)) {
        return false;
    }
    request->method = gateway_tb_decode_rpc(event->data, (size_t)event->data_len, &request->command);
    return true;
#else
    request->method = TB_RPC_SET;
    return (size_t)event->topic_len == strlen(bridge.command) &&
           memcmp(event->topic, bridge.command, (size_t)event->topic_len) == 0 &&
           gateway_home_decode_command(event->data, (size_t)event->data_len, &request->command);
#endif
}

static void mqtt_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    esp_mqtt_event_handle_t event = data;
    switch (id) {
    case MQTT_EVENT_CONNECTED:
        atomic_fetch_add(&bridge.generation, 1U);
        if (esp_mqtt_client_subscribe(event->client, bridge.command, 1) < 0) {
            ESP_LOGW("home-mqtt", "Command subscription failed");
            (void)esp_mqtt_client_disconnect(event->client);
        }
        break;
    case MQTT_EVENT_SUBSCRIBED:
        if (event->error_handle != NULL && event->error_handle->error_type == MQTT_ERROR_TYPE_NONE) {
            atomic_store(&bridge.subscribed, true);
            ESP_LOGI("home-mqtt", "Connected to broker; control subscription active");
        } else {
            ESP_LOGW("home-mqtt", "Broker denied the command subscription");
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        atomic_store(&bridge.gateway->home_session, 0U);
        atomic_store(&bridge.subscribed, false);
        atomic_fetch_add(&bridge.generation, 1U);
        break;
    case MQTT_EVENT_DATA: {
        MqttRequest request = {0};
        if (parse_request(event, &request) && xQueueSend(bridge.requests, &request, 0) != pdTRUE) {
            ESP_LOGW("home-mqtt", "Command queue full");
        }
        break;
    }
    default:
        break;
    }
}

#ifdef CONFIG_EDGE_HOME_THINGSBOARD
static bool publish_tb_state(const EdgeHomeState *state)
{
    char json[TB_STATE_MAX];
    int length = gateway_tb_state_json(json, sizeof json, state, false, 0);
    return length > 0 && (size_t)length < sizeof json &&
           esp_mqtt_client_publish(bridge.client, TB_TELEMETRY_TOPIC, json, length, 0, 0) >= 0;
}

static void update_tb_telemetry(HomeSession *session, uint32_t now)
{
    uint32_t elapsed = now - session->reported_ms;
    if ((!session->telemetry_sent || elapsed >= TB_REPORT_MS ||
         (elapsed >= TB_MIN_REPORT_MS &&
          gateway_tb_significant_change(&session->snapshot, &session->reported))) &&
        publish_tb_state(&session->snapshot)) {
        session->reported = session->snapshot;
        session->reported_ms = now;
        session->telemetry_sent = true;
    }
}

static void reply_tb(MqttRequest request, const char *json, int length)
{
    if (!atomic_load(&bridge.subscribed) || request.generation != atomic_load(&bridge.generation)) {
        return;
    }
    char topic[sizeof TB_RPC_RESPONSE_PREFIX + 10];
    (void)snprintf(topic, sizeof topic, TB_RPC_RESPONSE_PREFIX "%" PRIu32, request.rpc_id);
    /* Connection-specific RPC replies must not remain in the MQTT outbox. */
    (void)esp_mqtt_client_publish(bridge.client, topic, json, length, 0, 0);
}
#else

static void publish_state(EdgeHomeState data, uint32_t last_id)
{
    char json[512];
    int length = snprintf(json, sizeof json,
                          "{\"session\":%" PRIu32 ",\"last_id\":%" PRIu32 ",\"uptime_ms\":%" PRIu32
                          ",\"temperature_tenths\":%d,\"humidity_tenths\":%u,\"light_raw\":%u,"
                          "\"light_level\":%u,\"fan_on_tenths\":%u,\"close_level\":%u,"
                          "\"dht_flags\":%u,\"flags\":%u,\"curtain\":%u,\"curtain_target\":%u,"
                          "\"source\":%u,\"uart_errors\":%u,\"uart_crc_errors\":%u}",
                          data.session, last_id, data.uptime_ms, (int)data.temperature_tenths,
                          (unsigned)data.humidity_tenths, (unsigned)data.light_raw, (unsigned)data.light_level,
                          (unsigned)data.fan_on_tenths, (unsigned)data.close_level, (unsigned)data.dht_flags,
                          (unsigned)data.flags, (unsigned)data.curtain, (unsigned)data.curtain_target,
                          (unsigned)data.source, atomic_load(&bridge.gateway->uart_errors),
                          atomic_load(&bridge.gateway->uart_crc_errors));
    if (length > 0 && (size_t)length < sizeof json) {
        (void)esp_mqtt_client_publish(bridge.client, bridge.state, json, length, 0, 0);
    }
}
#endif

static void availability(bool online)
{
#ifdef CONFIG_EDGE_HOME_THINGSBOARD
    if (!online) {
        (void)publish_tb_state(NULL);
    }
#else
    (void)esp_mqtt_client_publish(bridge.client, bridge.availability, online ? "online" : "offline",
                                  0, 0, 1);
#endif
}

static void publish_receipt(Receipt receipt)
{
    EdgeHomeCommand command = receipt.request.command;
    char json[144];
    int length = snprintf(json, sizeof json,
                          "{\"session\":%" PRIu32 ",\"id\":%" PRIu32
                          ",\"op\":%u,\"result\":%u,\"confirmed\":%s}",
                          command.session, command.id, (unsigned)command.operation,
                          (unsigned)receipt.result, receipt.confirmed ? "true" : "false");
    if (length > 0 && (size_t)length < sizeof json) {
#ifdef CONFIG_EDGE_HOME_THINGSBOARD
        reply_tb(receipt.request, json, length);
#else
        (void)esp_mqtt_client_enqueue(bridge.client, bridge.ack, json, length, 1, 0, true);
#endif
    }
}

static void finish(HomeSession *session, uint8_t result, bool confirmed)
{
    session->last = (Receipt){.request = session->pending, .result = result, .confirmed = confirmed};
    session->pending.command.id = 0;
    publish_receipt(session->last);
}

static void start_session(HomeSession *session, uint32_t now)
{
    uint32_t previous = session->session;
    uint32_t nonce;
    do {
        nonce = esp_random();
    } while (nonce == 0U || nonce == previous);
    *session = (HomeSession){.session = nonce, .sync_ms = now - SYNC_MS};
    atomic_store(&bridge.gateway->home_session, nonce);
    availability(false);
}

static void accept_request(HomeSession *session, MqttRequest request, uint32_t now)
{
    if (request.generation != atomic_load(&bridge.generation) || !atomic_load(&bridge.subscribed)) {
        return;
    }
    bool fresh = session->ready && now - session->state_ms < UART_STALE_MS &&
                 session->session == atomic_load(&bridge.gateway->home_session) && gateway_wifi_ready();
    Receipt rejection = {.request = request, .result = HOME_UNAVAILABLE};
#ifdef CONFIG_EDGE_HOME_THINGSBOARD
    if (request.method == TB_RPC_GET) {
        char json[TB_STATE_MAX];
        int length = gateway_tb_state_json(json, sizeof json, fresh ? &session->snapshot : NULL,
                                           true, session->last.request.command.id);
        if (length > 0 && (size_t)length < sizeof json) {
            reply_tb(request, json, length);
        }
        return;
    }
    if (request.method != TB_RPC_SET) {
        rejection.result = request.method == TB_RPC_UNSUPPORTED ? EDGE_UNSUPPORTED : EDGE_INVALID;
        publish_receipt(rejection);
        return;
    }
#endif
    EdgeHomeCommand command = request.command;
    if (command.session != session->session || now - request.received_ms >= MAX_QUEUED_REQUEST_MS) {
        rejection.result = EDGE_STALE;
    } else if (fresh) {
        const EdgeHomeCommand last = session->last.request.command;
        if (command.id == last.id && command.operation == last.operation && command.value == last.value) {
            Receipt replay = session->last;
            replay.request = request;
            publish_receipt(replay);
            return;
        }
        if (command.id <= last.id) {
            rejection.result = EDGE_STALE;
        } else if (session->pending.command.id != 0U) {
            const EdgeHomeCommand pending = session->pending.command;
            if (command.id == pending.id && command.operation == pending.operation &&
                command.value == pending.value && request.rpc_id == session->pending.rpc_id) {
                return; /* MQTT duplicate while the original request is in flight. */
            }
            rejection.result = EDGE_BUSY;
        } else {
            session->pending = request;
            session->attempts = 0U;
            return;
        }
    }
    publish_receipt(rejection);
}

static void drive_uart(HomeSession *session, uint32_t now)
{
    const EdgeHomeCommand command = session->pending.command;
    if (command.id != 0U) {
        if (now - session->state_ms >= UART_STALE_MS) {
            finish(session, HOME_UNAVAILABLE, false);
            return;
        }
        if (session->attempts != 0U && now - session->sent_ms < RETRY_MS) {
            return;
        }
        if (session->attempts >= MAX_ATTEMPTS) {
            finish(session, HOME_TIMEOUT, false);
            return;
        }
        GatewayMessage frame = {
            .type = EDGE_HOME_COMMAND, .seq = (uint16_t)command.id, .length = EDGE_HOME_COMMAND_LEN
        };
        edge_home_encode_command(frame.bytes, command);
        (void)xQueueSend(bridge.gateway->home_tx, &frame, 0);
        session->sent_ms = now;
        ++session->attempts;
    } else if (now - session->sync_ms >= SYNC_MS) {
        GatewayMessage frame = {
            .type = session->ready ? EDGE_HEARTBEAT : EDGE_HOME_SYNC,
            .length = EDGE_HOME_SYNC_LEN
        };
        edge_put32(frame.bytes, session->session);
        (void)xQueueSend(bridge.gateway->home_tx, &frame, 0);
        session->sync_ms = now;
    }
}

static void mqtt_worker(void *context)
{
    (void)context;
    HomeSession session = {0};
    unsigned generation = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(25));
        if (!atomic_load(&bridge.subscribed) || !gateway_wifi_ready()) {
            atomic_store(&bridge.gateway->home_session, 0U);
            session.session = 0U;
            continue;
        }

        uint32_t now = gateway_ms();
        unsigned current = atomic_load(&bridge.generation);
        if (session.session == 0U || generation != current ||
            session.session != atomic_load(&bridge.gateway->home_session)) {
            start_session(&session, now);
            generation = current;
        }

        GatewayMessage message;
        bool received = xQueueReceive(bridge.gateway->home_state, &message, 0) == pdTRUE;
        EdgeHomeState state = {0};
        if (received) {
            state = edge_home_decode_state(message.bytes);
        }
        if (session.ready && ((received && state.session != session.session) ||
                              now - session.state_ms >= UART_STALE_MS)) {
            if (session.pending.command.id != 0U) {
                finish(&session, HOME_UNAVAILABLE, false);
            }
            start_session(&session, now);
        }
        if (xQueueReceive(bridge.gateway->home_ack, &message, 0) == pdTRUE &&
            session.pending.command.id != 0U) {
            EdgeHomeAck ack = edge_home_decode_ack(message.bytes);
            if (message.seq == (uint16_t)ack.id && ack.session == session.session &&
                ack.id == session.pending.command.id && ack.operation == session.pending.command.operation &&
                ack.result <= EDGE_DEFERRED) {
                finish(&session, ack.result, true);
            }
        }
        if (received && state.session == session.session) {
            session.state_ms = now;
#ifdef CONFIG_EDGE_HOME_THINGSBOARD
            session.snapshot = state;
            update_tb_telemetry(&session, now);
#else
            publish_state(state, session.last.request.command.id);
#endif
            if (!session.ready) {
                session.ready = true;
                ESP_LOGI("home-mqtt", "STM32 confirmed the current UART session");
                availability(true);
            }
        }
        MqttRequest request;
        if (xQueueReceive(bridge.requests, &request, 0) == pdTRUE) {
            accept_request(&session, request, gateway_ms());
        }
        drive_uart(&session, gateway_ms());
    }
}

bool gateway_mqtt_start(Gateway *gateway)
{
    bridge.gateway = gateway;
    (void)snprintf(bridge.client_id, sizeof bridge.client_id, "home-%s", CONFIG_EDGE_DEVICE_ID);
#ifdef CONFIG_EDGE_HOME_THINGSBOARD
    memcpy(bridge.command, TB_RPC_REQUEST_PREFIX "+", sizeof TB_RPC_REQUEST_PREFIX "+");
    esp_mqtt_client_config_t config = {
        .broker.address = {.hostname = CONFIG_EDGE_TB_HOST, .transport = MQTT_TRANSPORT_OVER_SSL, .port = 8883},
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        .credentials = {.client_id = bridge.client_id, .username = CONFIG_EDGE_TB_DEVICE_TOKEN},
        .session = {.keepalive = 15, .protocol_ver = MQTT_PROTOCOL_V_3_1_1},
        .network = {.timeout_ms = 4000, .reconnect_timeout_ms = 3000},
        .outbox.limit = 4096
    };
#else
    (void)snprintf(bridge.command, sizeof bridge.command, "smarthome/%s/command", CONFIG_EDGE_DEVICE_ID);
    (void)snprintf(bridge.state, sizeof bridge.state, "smarthome/%s/state", CONFIG_EDGE_DEVICE_ID);
    (void)snprintf(bridge.ack, sizeof bridge.ack, "smarthome/%s/ack", CONFIG_EDGE_DEVICE_ID);
    (void)snprintf(bridge.availability, sizeof bridge.availability, "smarthome/%s/availability", CONFIG_EDGE_DEVICE_ID);
    esp_mqtt_client_config_t config = {
        .broker.address = {.hostname = CONFIG_EDGE_SERVER_IPV4,
                           .transport = MQTT_TRANSPORT_OVER_TCP, .port = CONFIG_EDGE_MQTT_PORT},
        .credentials = {.client_id = bridge.client_id, .username = CONFIG_EDGE_MQTT_USER,
                        .authentication.password = CONFIG_EDGE_MQTT_PASSWORD},
        .session = {.keepalive = 10, .last_will = {.topic = bridge.availability, .msg = "offline",
                                                  .qos = 1, .retain = true}},
        .network = {.timeout_ms = 1200, .reconnect_timeout_ms = 1500},
        .outbox.limit = 8192
    };
#endif
    bridge.requests = xQueueCreate(4, sizeof(MqttRequest));
    bridge.client = esp_mqtt_client_init(&config);
    if (bridge.requests != NULL && bridge.client != NULL &&
        esp_mqtt_client_register_event(bridge.client, ESP_EVENT_ANY_ID, mqtt_event, NULL) == ESP_OK &&
        esp_mqtt_client_start(bridge.client) == ESP_OK) {
        if (xTaskCreate(mqtt_worker, "home-mqtt", 6144, NULL, 5, NULL) == pdPASS) {
            return true;
        }
        (void)esp_mqtt_client_stop(bridge.client);
    }
    if (bridge.client != NULL) {
        (void)esp_mqtt_client_destroy(bridge.client);
    }
    if (bridge.requests != NULL) {
        vQueueDelete(bridge.requests);
    }
    ESP_LOGE("home-mqtt", "MQTT startup failed");
    return false;
}

#endif
