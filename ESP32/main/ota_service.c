#include "ota_service.h"
#include "cJSON.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ota_service";
/* ESP-IDF places this directly after esp_app_desc_t in the application image. */
const char edge_ota_descriptor[16] __attribute__((section(".rodata_custom_desc"))) = "EDGE-GW-OTA-V1";
enum { IDLE, ARMED, RECEIVING, FAILED, REBOOTING };
enum {
    AUTH_SECONDS = 300,
    HEALTH_GRACE_MS = 10000,
    HEALTH_DEADLINE_MS = 60000,
    MINIMUM_FREE_HEAP = 24576,
};
typedef struct {
    unsigned phase;
    bool healthy, network_ready;
    int64_t arm_until, reboot_at;
    int64_t uart_alive;
    size_t received, total;
    char key[65], error[40], candidate[32];
} ota_control_t;
static ota_control_t control;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static httpd_handle_t server;
extern const unsigned char page_start[] asm("_binary_ota_page_html_start");
extern const unsigned char page_end[] asm("_binary_ota_page_html_end");
static int64_t now_ms(void) { return esp_timer_get_time() / 1000; }
static ota_control_t snapshot(void) {
    portENTER_CRITICAL(&lock); ota_control_t value = control; portEXIT_CRITICAL(&lock); return value;
}
void ota_service_uart_alive(void) {
    const int64_t tick = now_ms();
    portENTER_CRITICAL(&lock);
    control.uart_alive = tick;
    portEXIT_CRITICAL(&lock);
}
static const char *image_state(void) {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK) return "unselected";
    switch (state) {
        case ESP_OTA_IMG_VALID: return "valid";
        case ESP_OTA_IMG_PENDING_VERIFY: return "pending_verify";
        case ESP_OTA_IMG_NEW: return "new";
        case ESP_OTA_IMG_INVALID: return "invalid";
        case ESP_OTA_IMG_ABORTED: return "aborted";
        default: return "undefined";
    }
}
static bool status_json(cJSON *json) {
    ota_control_t value = snapshot();
    const int64_t remaining = value.arm_until - now_ms();
    const char *phases[] = {"idle", "armed", "receiving", "failed", "rebooting"};
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t bad = {0};
    if (invalid) (void)esp_ota_get_partition_description(invalid, &bad);
    bad.version[sizeof(bad.version) - 1] = 0;
    unsigned char mac[6] = {0}; char station_mac[18], ip[16] = "";
    (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(station_mac, sizeof(station_mac), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_netif_t *station = value.network_ready ? esp_netif_get_handle_from_ifkey("WIFI_STA_DEF") : NULL;
    esp_netif_ip_info_t info;
    if (station && esp_netif_get_ip_info(station, &info) == ESP_OK && info.ip.addr)
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    return json && cJSON_AddStringToObject(json, "version", esp_app_get_description()->version) &&
        cJSON_AddStringToObject(json, "project", esp_app_get_description()->project_name) &&
        cJSON_AddStringToObject(json, "station_mac", station_mac) && cJSON_AddStringToObject(json, "ipv4", ip) &&
        cJSON_AddNumberToObject(json, "slot_bytes", running ? running->size : 0) &&
        cJSON_AddStringToObject(json, "partition", running ? running->label : "unknown") &&
        cJSON_AddStringToObject(json, "image_state", image_state()) &&
        cJSON_AddBoolToObject(json, "healthy", value.healthy) &&
        cJSON_AddStringToObject(json, "phase", phases[value.phase]) &&
        cJSON_AddNumberToObject(json, "progress_percent", value.total ? value.received * 100U / value.total : 0) &&
        cJSON_AddNumberToObject(json, "armed_remaining_s", remaining > 0 ? (remaining + 999) / 1000 : 0) &&
        cJSON_AddStringToObject(json, "candidate_version", value.candidate) &&
        cJSON_AddStringToObject(json, "last_error", value.error) &&
        cJSON_AddStringToObject(json, "last_invalid_version", bad.version);
}
static esp_err_t arm(char key[65]) {
    ota_control_t value = snapshot();
    if (!value.healthy || value.phase == RECEIVING || value.phase == REBOOTING || strcmp(image_state(), "valid"))
        return ESP_ERR_INVALID_STATE;
    unsigned char random[32]; esp_fill_random(random, sizeof(random));
    char fresh[65];
    for (unsigned i = 0; i < sizeof(random); ++i) snprintf(fresh + i * 2, 3, "%02x", random[i]);
    portENTER_CRITICAL(&lock);
    /* Recheck after generating randomness: the HTTP task may have started. */
    if (control.phase == RECEIVING || control.phase == REBOOTING) {
        portEXIT_CRITICAL(&lock); return ESP_ERR_INVALID_STATE;
    }
    control.phase = ARMED; control.arm_until = now_ms() + AUTH_SECONDS * 1000;
    memcpy(control.key, fresh, sizeof(fresh)); control.error[0] = 0;
    control.received = control.total = 0; control.candidate[0] = 0;
    portEXIT_CRITICAL(&lock);
    memcpy(key, fresh, sizeof(fresh)); memset(random, 0, sizeof(random)); memset(fresh, 0, sizeof(fresh));
    ESP_LOGI(TAG, "USB authorized upgrade window opened for 300 seconds");
    return ESP_OK;
}
static esp_err_t reply(httpd_req_t *request, const char *status, cJSON *json) {
    char *body = json ? cJSON_PrintUnformatted(json) : NULL;
    if (!body) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "response_memory");
    httpd_resp_set_status(request, status);
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_send(request, body, HTTPD_RESP_USE_STRLEN);
    cJSON_free(body); return result;
}
static esp_err_t root(httpd_req_t *request) {
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    httpd_resp_set_hdr(request, "Content-Security-Policy", "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; connect-src 'self'");
    return httpd_resp_send(request, (const char *)page_start, page_end - page_start - 1);
}
static esp_err_t status_get(httpd_req_t *request) {
    cJSON *json = cJSON_CreateObject();
    if (!status_json(json)) { cJSON_Delete(json); return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "response_memory"); }
    esp_err_t result = reply(request, "200 OK", json); cJSON_Delete(json); return result;
}
static esp_err_t reject(httpd_req_t *request, const char *http_status, const char *reason, bool consumed) {
    if (consumed) {
        portENTER_CRITICAL(&lock); control.phase = FAILED;
        snprintf(control.error, sizeof(control.error), "%s", reason); portEXIT_CRITICAL(&lock);
    }
    ESP_LOGW(TAG, "Upload rejected: %s", reason);
    cJSON *json = cJSON_CreateObject();
    if (json) { cJSON_AddBoolToObject(json, "success", false); cJSON_AddStringToObject(json, "status", reason); }
    esp_err_t result = reply(request, http_status, json); cJSON_Delete(json); return result;
}
static bool hex_digest(const char *text, unsigned char digest[32]) {
    if (strlen(text) != 64) return false;
    for (unsigned i = 0; i < 32; ++i) {
        unsigned value = 0;
        for (unsigned j = 0; j < 2; ++j) {
            char c = text[i * 2 + j]; unsigned n;
            if (c >= '0' && c <= '9') n = (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') n = (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') n = (unsigned)(c - 'A' + 10);
            else return false;
            value = value * 16U + n;
        }
        digest[i] = (unsigned char)value;
    }
    return true;
}
static bool equal_bytes(const unsigned char *a, const unsigned char *b, size_t length) {
    unsigned char difference = 0;
    for (size_t i = 0; i < length; ++i) difference |= a[i] ^ b[i];
    return difference == 0;
}
static int receive(httpd_req_t *request, unsigned char *buffer, size_t length, int64_t deadline) {
    for (unsigned retry = 0; retry < 3 && now_ms() < deadline; ++retry) {
        int n = httpd_req_recv(request, (char *)buffer, length);
        if (n != HTTPD_SOCK_ERR_TIMEOUT) return n;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return -1;
}
static esp_err_t upload(httpd_req_t *request) {
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    const size_t descriptor_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    const size_t marker_offset = descriptor_offset + sizeof(esp_app_desc_t);
    const size_t prefix_size = marker_offset + sizeof(edge_ota_descriptor);
    if (!target || request->content_len < prefix_size || request->content_len > target->size)
        return reject(request, "413 Payload Too Large", "image_size", false);
    char key[65], expected_hex[65]; unsigned char expected[32];
    if (httpd_req_get_hdr_value_len(request, "X-OTA-Key") != 64 ||
        httpd_req_get_hdr_value_str(request, "X-OTA-Key", key, sizeof(key)) != ESP_OK)
        return reject(request, "403 Forbidden", "authorization", false);
    if (httpd_req_get_hdr_value_len(request, "X-Firmware-SHA256") != 64 ||
        httpd_req_get_hdr_value_str(request, "X-Firmware-SHA256", expected_hex, sizeof(expected_hex)) != ESP_OK ||
        !hex_digest(expected_hex, expected)) return reject(request, "400 Bad Request", "sha256_header", false);
    bool authorized;
    portENTER_CRITICAL(&lock);
    authorized = control.healthy && control.phase == ARMED && now_ms() < control.arm_until &&
        equal_bytes((const unsigned char *)key, (const unsigned char *)control.key, 64);
    if (authorized) {
        control.phase = RECEIVING; control.arm_until = 0; memset(control.key, 0, sizeof(control.key));
        control.total = request->content_len; control.received = 0; control.error[0] = 0;
    }
    portEXIT_CRITICAL(&lock); memset(key, 0, sizeof(key));
    if (!authorized) return reject(request, "403 Forbidden", "authorization", false);
    unsigned char buffer[4096]; size_t received = 0;
    const int64_t deadline = now_ms() + 120000;
    while (received < prefix_size) {
        int n = receive(request, buffer + received, prefix_size - received, deadline);
        if (n <= 0) return reject(request, "408 Request Timeout", "receive_timeout", true);
        received += (size_t)n;
    }
    esp_image_header_t header;
    esp_image_segment_header_t first_segment;
    esp_app_desc_t desc;
    memcpy(&header, buffer, sizeof(header));
    memcpy(&first_segment, buffer + sizeof(header), sizeof(first_segment));
    memcpy(&desc, buffer + descriptor_offset, sizeof(desc));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32 ||
        first_segment.data_len < sizeof(desc) + sizeof(edge_ota_descriptor) ||
        desc.magic_word != ESP_APP_DESC_MAGIC_WORD || !memchr(desc.version, 0, sizeof(desc.version)) ||
        !desc.version[0] || memcmp(desc.project_name, esp_app_get_description()->project_name, sizeof(desc.project_name)))
        return reject(request, "400 Bad Request", "image_identity", true);
    if (memcmp(buffer + marker_offset, edge_ota_descriptor, sizeof(edge_ota_descriptor))) {
        return reject(request, "400 Bad Request", "image_without_ota_support", true);
    }
    if (!strcmp(desc.version, esp_app_get_description()->version))
        return reject(request, "409 Conflict", "same_version", true);
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition(); esp_app_desc_t bad;
    if (invalid && esp_ota_get_partition_description(invalid, &bad) == ESP_OK &&
        !memcmp(bad.version, desc.version, sizeof(desc.version)))
        return reject(request, "409 Conflict", "previously_invalid_version", true);
    portENTER_CRITICAL(&lock); memcpy(control.candidate, desc.version, sizeof(desc.version)); portEXIT_CRITICAL(&lock);
    esp_ota_handle_t handle = 0;
    esp_err_t error = esp_ota_begin(target, request->content_len, &handle);
    if (error != ESP_OK) return reject(request, "500 Internal Server Error", "ota_begin", true);
    psa_hash_operation_t hash = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&hash, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        esp_ota_abort(handle); return reject(request, "500 Internal Server Error", "hash_setup", true);
    }
    const char *failure = NULL;
    size_t written = 0; unsigned last_progress = 101;
    for (;;) {
        const size_t n = received - written;
        if (psa_hash_update(&hash, buffer, n) != PSA_SUCCESS) { failure = "hash_update"; break; }
        if (esp_ota_write(handle, buffer, n) != ESP_OK) { failure = "ota_write"; break; }
        written = received;
        portENTER_CRITICAL(&lock); control.received = written; portEXIT_CRITICAL(&lock);
        unsigned progress = (unsigned)(written * 100U / request->content_len);
        if (progress / 10U != last_progress / 10U) {
            ESP_LOGI(TAG, "Receiving %u%% -> %s version=%s", progress, target->label, desc.version); last_progress = progress;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
        if (written == request->content_len) break;
        size_t remaining = request->content_len - written;
        int count = receive(request, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer), deadline);
        if (count <= 0) { failure = "receive_timeout"; break; }
        received += (size_t)count;
    }
    unsigned char actual[32]; size_t hash_size = 0;
    if (!failure && (psa_hash_finish(&hash, actual, sizeof(actual), &hash_size) != PSA_SUCCESS ||
                     hash_size != sizeof(actual) || !equal_bytes(actual, expected, sizeof(actual)))) failure = "sha256_mismatch";
    psa_hash_abort(&hash);
    if (failure) {
        esp_ota_abort(handle); return reject(request, "400 Bad Request", failure, true);
    }
    error = esp_ota_end(handle);
    if (error != ESP_OK) return reject(request, "400 Bad Request", "image_validation", true);
    error = esp_ota_set_boot_partition(target);
    if (error != ESP_OK) return reject(request, "500 Internal Server Error", "boot_selection", true);
    portENTER_CRITICAL(&lock); control.phase = REBOOTING; control.reboot_at = now_ms() + 1500; portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "Image verified; next=%s version=%s bytes=%u; reboot scheduled", target->label, desc.version, (unsigned)written);
    cJSON *json = cJSON_CreateObject();
    if (json) {
        cJSON_AddBoolToObject(json, "success", true); cJSON_AddStringToObject(json, "status", "rebooting");
        cJSON_AddStringToObject(json, "version", desc.version); cJSON_AddStringToObject(json, "partition", target->label);
    }
    esp_err_t result = reply(request, "200 OK", json); cJSON_Delete(json); return result;
}
static void console_command(const char *line) {
    if (!strcmp(line, "OTAARM")) {
        char key[65];
        esp_err_t result = arm(key);
        if (result == ESP_OK) {
            printf("OTA_ARM {\"key\":\"%s\",\"ttl_s\":%d}\n", key, AUTH_SECONDS);
        } else {
            printf("OTA_ARM {\"error\":\"%s\"}\n", esp_err_to_name(result));
        }
        memset(key, 0, sizeof(key));
        fflush(stdout);
    } else if (!strcmp(line, "OTAGET")) {
        cJSON *json = cJSON_CreateObject();
        char *body = status_json(json) ? cJSON_PrintUnformatted(json) : NULL;
        if (body) {
            printf("OTA_STATUS %s\n", body);
            fflush(stdout);
        }
        cJSON_free(body);
        cJSON_Delete(json);
    }
}

static void console_poll(void) {
    static char line[16];
    static size_t used;
    static bool overflow;
    uint8_t buffer[128];
    int bytes = uart_read_bytes(UART_NUM_0, buffer, sizeof(buffer), 0);
    for (int i = 0; i < bytes; ++i) {
        uint8_t byte = buffer[i];
        if (byte == '\r' || byte == '\n') {
            if (!overflow && used > 0) {
                line[used] = '\0';
                console_command(line);
            }
            used = 0;
            overflow = false;
        } else if (used == sizeof(line) - 1 || byte < ' ' || byte > '~') {
            overflow = true;
        } else if (!overflow) {
            line[used++] = (char)byte;
        }
    }
}

static void rollback_if_pending(const char *reason) {
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &state) != ESP_OK ||
        state != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    ESP_LOGE(TAG, "%s", reason);
    if (esp_ota_check_rollback_is_possible()) {
        esp_err_t error = esp_ota_mark_app_invalid_rollback_and_reboot();
        ESP_LOGE(TAG, "Rollback failed: %s", esp_err_to_name(error));
        esp_restart();
    }
    ESP_LOGE(TAG, "No bootable fallback. Restore a verified baseline over USB.");
}

static void health_task(void *argument) {
    (void)argument;
#ifndef CONFIG_EDGE_OTA_DIAGNOSTIC_ROLLBACK
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    esp_err_t lookup = esp_ota_get_state_partition(running, &state);
    const bool pending = lookup == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
#endif
    bool checking = true;
    for (;;) {
        console_poll();
        const int64_t tick = now_ms();
        ota_control_t value = snapshot();
        if (value.reboot_at && tick >= value.reboot_at) esp_restart();
        if (value.phase == ARMED && tick >= value.arm_until) {
            portENTER_CRITICAL(&lock);
            if (control.phase == ARMED && tick >= control.arm_until) {
                control.phase = IDLE; memset(control.key, 0, sizeof(control.key));
            }
            portEXIT_CRITICAL(&lock);
        }
        if (checking && tick >= HEALTH_GRACE_MS) {
#ifdef CONFIG_EDGE_OTA_DIAGNOSTIC_ROLLBACK
            checking = false;
            rollback_if_pending("Diagnostic image forced a startup health failure");
#else
            bool healthy = value.network_ready && value.uart_alive > 0 &&
                tick - value.uart_alive <= 2000 && esp_get_free_heap_size() >= MINIMUM_FREE_HEAP;
            if (healthy) {
                checking = false;
                if (lookup == ESP_ERR_NOT_FOUND) {
                    /* A USB baseline with erased otadata must become a VALID fallback. */
                    esp_err_t error = esp_ota_set_boot_partition(running);
                    if (error == ESP_OK) esp_restart();
                    ESP_LOGE(TAG, "Cannot initialize boot selection: %s", esp_err_to_name(error));
                } else if (pending && esp_ota_mark_app_valid_cancel_rollback() != ESP_OK) {
                    rollback_if_pending("Could not confirm the new image");
                } else if (pending || (lookup == ESP_OK && state == ESP_OTA_IMG_VALID)) {
                    portENTER_CRITICAL(&lock);
                    control.healthy = true;
                    portEXIT_CRITICAL(&lock);
                    ESP_LOGI(TAG, "Startup health verified for %s on %s",
                             esp_app_get_description()->version, running->label);
                }
            } else if (tick >= HEALTH_DEADLINE_MS) {
                checking = false;
                ESP_LOGE(TAG, "UART task, local network initialization or heap check failed");
                rollback_if_pending("Startup health deadline expired");
            }
#endif
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t ota_service_network_ready(void) {
    if (!uart_is_driver_installed(UART_NUM_0)) {
        return ESP_ERR_INVALID_STATE;
    }
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80; config.stack_size = 10240; config.max_open_sockets = 4;
    config.max_uri_handlers = 3; config.lru_purge_enable = false;
    config.recv_wait_timeout = 5; config.send_wait_timeout = 5;
    esp_err_t error = httpd_start(&server, &config);
    if (error != ESP_OK) return error;
    const httpd_uri_t routes[] = {
        {.uri="/", .method=HTTP_GET, .handler=root},
        {.uri="/api/ota", .method=HTTP_GET, .handler=status_get},
        {.uri="/api/update", .method=HTTP_POST, .handler=upload},
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        error = httpd_register_uri_handler(server, &routes[i]);
        if (error != ESP_OK) break;
    }
    if (error != ESP_OK) {
        httpd_stop(server);
        server = NULL;
        return error;
    }
    portENTER_CRITICAL(&lock);
    control.network_ready = true;
    portEXIT_CRITICAL(&lock);
    ESP_LOGI(TAG, "LAN OTA page uses port 80; USB authorization required");
    return ESP_OK;
}

static esp_err_t start_service(void) {
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    if (running == NULL || other == NULL || running == other ||
        running->subtype < ESP_PARTITION_SUBTYPE_APP_OTA_MIN ||
        running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MAX ||
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL) == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (psa_crypto_init() != PSA_SUCCESS) return ESP_FAIL;
    esp_err_t error = uart_driver_install(UART_NUM_0, 512, 0, 0, NULL, 0);
    if (error != ESP_OK) return error;
    if (xTaskCreate(health_task, "ota_health", 4096, NULL, 2, NULL) != pdPASS) {
        uart_driver_delete(UART_NUM_0);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ota_service_start(void) {
    esp_err_t error = start_service();
    if (error != ESP_OK) {
        rollback_if_pending("OTA service initialization failed");
    }
    return error;
}
