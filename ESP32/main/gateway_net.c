#include "gateway.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum {
    WIFI_IP = BIT0, WIFI_DISCONNECTED = BIT1,
    GATEWAY_UNAVAILABLE = 4, GATEWAY_TIMEOUT = 5
};

static const char *const tag = "edge-net";
static EventGroupHandle_t wifi_events;
static atomic_uint wifi_generation;
static struct sockaddr_in peer;

typedef struct {
    char line[GATEWAY_JSON_MAX];
    size_t used;
    GatewayCommand pending;
    uint32_t pending_since;
    uint32_t last_pong;
} Session;

bool gateway_wifi_ready(void)
{
    return wifi_events != NULL && (xEventGroupGetBits(wifi_events) & WIFI_IP) != 0U;
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t event, void *data)
{
    Gateway *gateway = arg;
    if (base == IP_EVENT && event == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *info = data;
        if (info->ip_changed) {
            atomic_store(&gateway->home_session, 0U);
            atomic_fetch_add(&wifi_generation, 1);
        }
        xEventGroupSetBits(wifi_events, WIFI_IP);
        ESP_LOGI(tag, "Wi-Fi got " IPSTR, IP2STR(&info->ip_info.ip));
    } else if ((base == WIFI_EVENT && event == WIFI_EVENT_STA_DISCONNECTED) ||
               (base == IP_EVENT && event == IP_EVENT_STA_LOST_IP)) {
        atomic_store(&gateway->home_session, 0U);
        atomic_fetch_add(&wifi_generation, 1);
        xEventGroupClearBits(wifi_events, WIFI_IP);
        xEventGroupSetBits(wifi_events, WIFI_DISCONNECTED);
        ESP_LOGW(tag, "Wi-Fi/IP unavailable");
    }
}

static int wait_io(int socket_fd, bool write, unsigned ms)
{
    fd_set descriptors;
    FD_ZERO(&descriptors);
    FD_SET(socket_fd, &descriptors);
    struct timeval timeout = {.tv_sec = ms / 1000U, .tv_usec = (ms % 1000U) * 1000U};
    return select(socket_fd + 1, write ? NULL : &descriptors,
                  write ? &descriptors : NULL, NULL, &timeout);
}

static int open_connection(void)
{
    int socket_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_fd < 0) {
        return -1;
    }
    if (fcntl(socket_fd, F_SETFL, O_NONBLOCK) < 0) {
        goto failed;
    }
    if (connect(socket_fd, (struct sockaddr *)&peer, sizeof peer) == 0) {
        return socket_fd;
    }
    if (errno != EINPROGRESS || wait_io(socket_fd, true, 3000U) <= 0) {
        goto failed;
    }
    int error = 0;
    socklen_t length = sizeof error;
    if (getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &error, &length) == 0 && error == 0) {
        return socket_fd;
    }
failed:
    close(socket_fd);
    return -1;
}

static bool send_line(int socket_fd, const char *line)
{
    size_t bytes = strlen(line);
    uint32_t start = gateway_ms();
    while (bytes != 0U && gateway_ms() - start < 1000U && gateway_wifi_ready()) {
        int count = send(socket_fd, line, bytes, 0);
        if (count > 0) {
            line += count;
            bytes -= (size_t)count;
        } else if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (wait_io(socket_fd, true, 100U) < 0) {
                return false;
            }
        } else if (count == 0 || errno != EINTR) {
            return false;
        }
    }
    return bytes == 0U;
}

static bool uart_alive(Gateway *gateway)
{
#ifdef CONFIG_EDGE_SIMULATOR
    (void)gateway;
    return true;
#else
    if (!atomic_load(&gateway->stm_seen)) {
        return false;
    }
    uint32_t last = atomic_load(&gateway->last_stm_ms);
    return gateway_ms() - last < 8000U;
#endif
}

static bool send_reply(int socket_fd, GatewayCommand command, unsigned result, uint32_t value)
{
    char line[GATEWAY_JSON_MAX];
    return gateway_response(line, command, result, value) && send_line(socket_fd, line);
}

static bool incoming(int socket_fd, Gateway *gateway, Session *session)
{
    GatewayCommand command = {0};
    bool pong;
    if (!gateway_decode(session->line, session->used, &command, &pong)) {
        ESP_LOGW(tag, "Ignored invalid server message");
        return true;
    }
    if (pong) {
        session->last_pong = gateway_ms();
        return true;
    }
    if (session->pending.id != 0U) {
        return send_reply(socket_fd, command, EDGE_BUSY, 0U);
    }
    if (command.operation == EDGE_SET_PERIOD &&
        (command.value < EDGE_PERIOD_MIN || command.value > EDGE_PERIOD_MAX)) {
        return send_reply(socket_fd, command, EDGE_INVALID, 0U);
    }
    if (!uart_alive(gateway)) {
        return send_reply(socket_fd, command, GATEWAY_UNAVAILABLE, 0U);
    }
    if (xQueueSend(gateway->commands, &command, 0) != pdTRUE) {
        return send_reply(socket_fd, command, EDGE_BUSY, 0U);
    }
    session->pending = command;
    session->pending_since = gateway_ms();
    return true;
}

static bool read_messages(int socket_fd, Gateway *gateway, Session *session)
{
    int ready = wait_io(socket_fd, false, 50U);
    if (ready <= 0) {
        return ready == 0 || errno == EINTR;
    }
    char data[256];
    int length = recv(socket_fd, data, sizeof data, 0);
    if (length <= 0) {
        return length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR);
    }
    for (int i = 0; i < length; ++i) {
        if (data[i] == '\n') {
            session->line[session->used] = '\0';
            if (!incoming(socket_fd, gateway, session)) {
                return false;
            }
            session->used = 0;
        } else if (session->used == sizeof session->line - 1U) {
            ESP_LOGW(tag, "Server line too long");
            return false;
        } else {
            session->line[session->used++] = data[i];
        }
    }
    return true;
}

static bool send_updates(int socket_fd, Gateway *gateway, Session *session)
{
    GatewayMessage message;
    for (unsigned i = 0; i < 8 && xQueueReceive(gateway->replies, &message, 0) == pdTRUE; ++i) {
        if (session->pending.id != 0U && edge_u32(message.bytes) == session->pending.id &&
            message.bytes[4] == session->pending.operation) {
            if (!send_reply(socket_fd, session->pending, message.bytes[5], edge_u32(message.bytes + 6))) {
                return false;
            }
            session->pending.id = 0;
        }
    }
    if (session->pending.id != 0U && gateway_ms() - session->pending_since >= 3000U) {
        if (!send_reply(socket_fd, session->pending, GATEWAY_TIMEOUT, 0U)) {
            return false;
        }
        session->pending.id = 0;
    }
    QueueHandle_t sources[] = {gateway->errors, gateway->status, gateway->sensor};
    char line[GATEWAY_JSON_MAX];
    for (unsigned i = 0; i < sizeof sources / sizeof sources[0]; ++i) {
        if (xQueueReceive(sources[i], &message, 0) == pdTRUE &&
            gateway_serialize(line, &message) && !send_line(socket_fd, line)) {
            return false;
        }
    }
    return true;
}

static void run_session(int socket_fd, Gateway *gateway, unsigned generation)
{
#ifdef CONFIG_EDGE_SIMULATOR
#define SIMULATED "true"
#else
#define SIMULATED "false"
#endif
    static const char hello[] = "{\"v\":1,\"type\":\"hello\",\"device_id\":\""
        CONFIG_EDGE_DEVICE_ID "\",\"simulated\":" SIMULATED "}\n";
    if (!send_line(socket_fd, hello)) {
        return;
    }
    uint32_t now = gateway_ms();
    uint32_t last_ping = now - 3000U;
    Session session = {.last_pong = now};
    while (gateway_wifi_ready() && atomic_load(&wifi_generation) == generation) {
        now = gateway_ms();
        if (now - session.last_pong >= 12000U) {
            ESP_LOGW(tag, "PC heartbeat timeout");
            break;
        }
        if (now - last_ping >= 3000U) {
            wifi_ap_record_t ap;
            int rssi = esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
            char line[280];
            int length = snprintf(line, sizeof line, "{\"v\":1,\"type\":\"ping\",\"device_id\":\""
                CONFIG_EDGE_DEVICE_ID "\",\"uptime_ms\":%" PRIu32 ",\"uart_online\":%s,"
                "\"rssi\":%d,\"bridge_uart_errors\":%u,\"bridge_crc_errors\":%u}\n",
                now, uart_alive(gateway) ? "true" : "false", rssi,
                atomic_load(&gateway->uart_errors), atomic_load(&gateway->uart_crc_errors));
            if (length <= 0 || (size_t)length >= sizeof line || !send_line(socket_fd, line)) {
                break;
            }
            last_ping = now;
        }
        if (!read_messages(socket_fd, gateway, &session) ||
            !send_updates(socket_fd, gateway, &session)) {
            break;
        }
    }
}

static void backoff(unsigned *ms)
{
    vTaskDelay(pdMS_TO_TICKS(*ms));
    *ms = *ms < 4000U ? *ms * 2U : 8000U;
}

#ifdef CONFIG_EDGE_HOME_THINGSBOARD
static bool network_time_ready(void)
{
    static bool sntp_started;
    if (!sntp_started) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(CONFIG_EDGE_TB_NTP_SERVER);
        if (esp_netif_sntp_init(&config) != ESP_OK) {
            ESP_LOGW(tag, "Could not start SNTP for ThingsBoard TLS");
            return false;
        }
        sntp_started = true;
    }
    /* A cold boot starts near 1970. TLS still checks the full certificate validity period. */
    if (time(NULL) < (time_t)1704067200) {
        (void)esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2500U));
    }
    return time(NULL) >= (time_t)1704067200;
}
#endif

static void network_task(void *context)
{
    Gateway *gateway = context;
    unsigned wifi_delay = 500U;
    unsigned tcp_delay = 500U;
#ifdef CONFIG_EDGE_HOME_MQTT
    bool mqtt_started = false;
#endif
    for (;;) {
        if (!gateway_wifi_ready()) {
            xEventGroupClearBits(wifi_events, WIFI_DISCONNECTED);
            if (esp_wifi_connect() == ESP_OK) {
                (void)xEventGroupWaitBits(wifi_events, WIFI_IP | WIFI_DISCONNECTED,
                                          pdFALSE, pdFALSE, pdMS_TO_TICKS(20000U));
            }
            if (!gateway_wifi_ready()) {
                (void)esp_wifi_disconnect();
                backoff(&wifi_delay);
                continue;
            }
            wifi_delay = 500U;
        }
#ifdef CONFIG_EDGE_HOME_MQTT
        if (!mqtt_started) {
#ifdef CONFIG_EDGE_HOME_THINGSBOARD
            if (!network_time_ready()) {
                ESP_LOGW(tag, "Waiting for network time before connecting to ThingsBoard");
                vTaskDelay(pdMS_TO_TICKS(4000U));
                continue;
            }
#endif
            mqtt_started = gateway_mqtt_start(gateway);
        }
        vTaskDelay(pdMS_TO_TICKS(mqtt_started ? 200U : 2000U));
        continue;
#endif
        unsigned generation = atomic_load(&wifi_generation);
        int socket_fd = open_connection();
        if (socket_fd >= 0) {
            ESP_LOGI(tag, "PC connected");
            uint32_t start = gateway_ms();
            run_session(socket_fd, gateway, generation);
            close(socket_fd);
            if (gateway_ms() - start >= 10000U) {
                tcp_delay = 500U;
            }
            ESP_LOGW(tag, "PC disconnected");
        } else {
            ESP_LOGW(tag, "TCP connect failed");
        }
        if (gateway_wifi_ready()) {
            backoff(&tcp_delay);
        } else {
            tcp_delay = 500U;
        }
    }
}

static bool check(esp_err_t error, const char *step)
{
    if (error == ESP_OK) {
        return true;
    }
    ESP_LOGE(tag, "%s: %s", step, esp_err_to_name(error));
    return false;
}

bool gateway_net_start(Gateway *gateway)
{
#if defined(CONFIG_EDGE_HOME_THINGSBOARD)
    if (CONFIG_EDGE_TB_HOST[0] == '\0' || CONFIG_EDGE_TB_DEVICE_TOKEN[0] == '\0' ||
        CONFIG_EDGE_TB_NTP_SERVER[0] == '\0') {
        ESP_LOGE(tag, "Configure the ThingsBoard hostname, device token and NTP server");
        return false;
    }
#elif defined(CONFIG_EDGE_HOME_MQTT)
    if (CONFIG_EDGE_MQTT_USER[0] == '\0' || CONFIG_EDGE_MQTT_PASSWORD[0] == '\0') {
        ESP_LOGE(tag, "Configure the local MQTT username and password");
        return false;
    }
#endif
#ifndef CONFIG_EDGE_HOME_THINGSBOARD
    peer.sin_family = AF_INET;
    peer.sin_port = htons(CONFIG_EDGE_SERVER_PORT);
    if (inet_pton(AF_INET, CONFIG_EDGE_SERVER_IPV4, &peer.sin_addr) != 1) {
        ESP_LOGE(tag, "Invalid PC or MQTT broker IPv4 address");
        return false;
    }
#endif
    const size_t ssid_length = strlen(CONFIG_EDGE_WIFI_SSID);
    const size_t password_length = strlen(CONFIG_EDGE_WIFI_PASSWORD);
    wifi_config_t config = {0};
    if (ssid_length == 0U || ssid_length > sizeof config.sta.ssid ||
        password_length > sizeof config.sta.password) {
        ESP_LOGE(tag, "Invalid SSID or Wi-Fi password length");
        return false;
    }
    memcpy(config.sta.ssid, CONFIG_EDGE_WIFI_SSID, ssid_length);
    memcpy(config.sta.password, CONFIG_EDGE_WIFI_PASSWORD, password_length);
    wifi_events = xEventGroupCreate();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    return wifi_events != NULL &&
           check(nvs_flash_init(), "nvs_flash_init") &&
           check(esp_netif_init(), "esp_netif_init") &&
           check(esp_event_loop_create_default(), "create event loop") &&
           esp_netif_create_default_wifi_sta() != NULL &&
           check(esp_wifi_init(&init), "esp_wifi_init") &&
           check(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, gateway), "Wi-Fi events") &&
           check(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, wifi_event, gateway), "IP events") &&
           check(esp_wifi_set_storage(WIFI_STORAGE_RAM), "Wi-Fi storage") &&
           check(esp_wifi_set_mode(WIFI_MODE_STA), "station mode") &&
           check(esp_wifi_set_config(WIFI_IF_STA, &config), "station configuration") &&
           check(esp_wifi_start(), "esp_wifi_start") &&
           xTaskCreate(network_task, "edge-net", 8192, gateway, 5, NULL) == pdPASS;
}
