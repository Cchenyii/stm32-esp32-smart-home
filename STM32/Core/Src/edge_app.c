#include "edge_app.h"
#include "edge_board.h"
#include "edge_settings.h"
#include "edge_wire.h"
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"
#include "task.h"

enum Worker { SENSOR, PROCESS, DISPLAY, UART, WORKER_COUNT };

enum {
    LIGHT_PERIOD_MS = 100, LIGHT_STALE_MS = 350, CONTROL_PERIOD_MS = 10,
    HOME_REPORT_MS = 500, HOME_COMMAND_MAX_AGE_MS = 450, SETTINGS_QUIET_MS = 2000
};

typedef struct {
    uint32_t timestamp_ms;
    uint16_t raw;
    bool valid;
} LightSample;

typedef struct {
    EdgeSample sample;
    EdgeHomeView home;
    EdgeOledPower oled_power;
} DisplayState;

typedef struct {
    EdgeThresholds thresholds;
    uint32_t changed_ms;
} ThresholdChange;

typedef struct {
    EdgeHomeCommand command; /* Operation zero is a session sync. */
    uint32_t arrived_ms;
} HomeRequest;

/* Retained for a debugger watch, including when no board pins are configured. */
volatile struct {
    uint32_t tasks_alive[WORKER_COUNT];
    uint32_t samples;
    uint32_t sample_drops;
    uint32_t display_errors;
    uint32_t uart_tx_errors;
    uint32_t minimum_stack_words;
    uint32_t free_heap_bytes;
    uint32_t reset_flags;
    uint32_t watchdog;
    uint32_t fatal_code;
} edge_diagnostics;

volatile EdgeSample edge_last_sample;
volatile LightSample edge_last_light;
volatile EdgeHomeView edge_home_view;
volatile uint32_t edge_settings_saves, edge_settings_errors;

static TaskHandle_t workers[WORKER_COUNT];
static QueueHandle_t raw_queue, latest_display, latest_tx, latest_light;
static QueueHandle_t home_requests, home_replies, home_state;
static QueueHandle_t threshold_changes;
static SemaphoreHandle_t settings_lock;
static EdgeSettings thresholds_store;
static EdgeThresholds startup_thresholds;
static bool home_enabled;
static volatile bool uart_alive;
static uint32_t period_ms = EDGE_PERIOD_DEFAULT;
static EdgeParser parser;
static uint16_t sequence;
static uint32_t last_gateway_ms;
static bool gateway_seen;

#ifdef EDGE_ENABLE_FAULT_INJECTION
/* Debugger-only test build. Zero leaves all workers active. */
volatile unsigned edge_suspend_worker;
#endif

static void progress(enum Worker worker)
{
    edge_diagnostics.tasks_alive[worker] = HAL_GetTick();
#ifdef EDGE_ENABLE_FAULT_INJECTION
    if (edge_suspend_worker == (unsigned)worker + 1U) {
        vTaskSuspend(NULL);
    }
#endif
}

static void count_drop(void)
{
    taskENTER_CRITICAL();
    ++edge_diagnostics.sample_drops;
    taskEXIT_CRITICAL();
}

static uint32_t get_period(void)
{
    configASSERT(xSemaphoreTake(settings_lock, pdMS_TO_TICKS(100U)) == pdTRUE);
    uint32_t value = period_ms;
    xSemaphoreGive(settings_lock);
    return value;
}

static void set_period(uint32_t value)
{
    configASSERT(xSemaphoreTake(settings_lock, pdMS_TO_TICKS(100U)) == pdTRUE);
    period_ms = value;
    xSemaphoreGive(settings_lock);
    xTaskNotifyGive(workers[SENSOR]);
}

static void sensor_task(void *context)
{
    (void)context;
    uint32_t boot = HAL_GetTick();
    uint32_t last = boot;
    uint32_t last_light = boot - LIGHT_PERIOD_MS;
    bool first = true;
    for (;;) {
        progress(SENSOR);
        uint32_t now = HAL_GetTick();
        if (home_enabled && now - last_light >= LIGHT_PERIOD_MS) {
            LightSample light = {0};
            last_light = now;
            light.valid = edge_home_light_read(&light.raw);
            light.timestamp_ms = HAL_GetTick();
            edge_last_light = light;
            xQueueOverwrite(latest_light, &light);
        }
        if (now - last >= (first ? EDGE_PERIOD_MIN : get_period())) {
            EdgeSample sample;
            last = now;
            first = false;
            edge_board_read(&sample);
            if (xQueueSend(raw_queue, &sample, 0) != pdTRUE) {
                count_drop();
            }
        }
        ThresholdChange change;
        /* The sensor task owns Flash programming, outside the DHT22 timing window. */
        if (home_enabled && xQueuePeek(threshold_changes, &change, 0) == pdTRUE &&
            HAL_GetTick() - change.changed_ms >= SETTINGS_QUIET_MS &&
            xQueueReceive(threshold_changes, &change, 0) == pdTRUE) {
            bool changed = !thresholds_store.valid ||
                           thresholds_store.current.fan_on_tenths != change.thresholds.fan_on_tenths ||
                           thresholds_store.current.close_level != change.thresholds.close_level;
            if (changed) {
                if (edge_settings_save(&thresholds_store, change.thresholds)) {
                    ++edge_settings_saves;
                } else {
                    ++edge_settings_errors;
                }
            }
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100U));
    }
}

static void control_tick(EdgeHome *home, DisplayState *state, EdgeOledIdle *idle)
{
    LightSample light = {0};
    (void)xQueuePeek(latest_light, &light, 0);
    uint32_t now = HAL_GetTick();
    EdgeHomeReading reading = {
        .temperature_tenths = state->sample.temperature_tenths,
        .temperature_valid = (state->sample.flags & EDGE_TH_VALID) != 0U &&
                             now - state->sample.uptime_ms <= get_period() + 1000U,
        .light_level = edge_pins.home.light_decreases_with_brightness ?
                       EDGE_LIGHT_MAX - light.raw : light.raw,
        .light_valid = light.valid && now - light.timestamp_ms <= LIGHT_STALE_MS
    };
    uint8_t buttons = edge_oled_idle_step(idle, edge_home_buttons(), now);
    state->oled_power = idle->power;
    edge_home_step(home, reading, buttons, now);
    HomeRequest request = {0};
    uint8_t result = EDGE_STALE;
    if (home_requests != NULL && xQueueReceive(home_requests, &request, 0) == pdTRUE) {
        uint32_t received_now = HAL_GetTick();
        if (received_now - request.arrived_ms <= HOME_COMMAND_MAX_AGE_MS) {
            if (request.command.operation == 0U) {
                edge_home_sync(home, request.command.session);
            } else {
                result = edge_home_remote(home, reading, request.command, received_now);
            }
        }
    }
    if (state->home.fan_on_tenths != home->view.fan_on_tenths ||
        state->home.close_level != home->view.close_level) {
        ThresholdChange change = {
            .thresholds = {(uint16_t)home->view.fan_on_tenths, home->view.close_level},
            .changed_ms = HAL_GetTick()
        };
        xQueueOverwrite(threshold_changes, &change);
    }
    edge_home_outputs(&home->view);
    state->home = home->view;
    edge_home_view = home->view;
    if (home_state != NULL) {
        EdgeHomeState report = {
            .session = home->session, .uptime_ms = now,
            .temperature_tenths = state->sample.temperature_tenths,
            .humidity_tenths = state->sample.humidity_tenths,
            .light_raw = light.raw, .light_level = reading.light_level,
            .fan_on_tenths = (uint16_t)home->view.fan_on_tenths, .close_level = home->view.close_level,
            .dht_flags = state->sample.flags & (EDGE_TH_VALID | EDGE_DHT_TIMEOUT | EDGE_DHT_BAD | EDGE_DHT_DISABLED),
            .flags = (reading.temperature_valid ? EDGE_HOME_TH_VALID : 0) |
                     (reading.light_valid ? EDGE_HOME_LIGHT_VALID : 0) |
                     (home->view.manual ? EDGE_HOME_MANUAL : 0) |
                     (home->view.fan_on ? EDGE_HOME_FAN_ON : 0),
            .curtain = (uint8_t)home->view.curtain, .curtain_target = (uint8_t)home->curtain_target,
            .source = (uint8_t)home->view.source
        };
        xQueueOverwrite(home_state, &report);
        if (request.command.operation != 0U) {
            EdgeHomeAck ack = {
                .session = request.command.session, .id = request.command.id,
                .operation = request.command.operation, .result = result
            };
            if (xQueueSend(home_replies, &ack, 0) != pdTRUE) {
                count_drop();
            }
        }
    }
}

static void process_task(void *context)
{
    (void)context;
    DisplayState state = {0};
    EdgeHome home;
    EdgeOledIdle idle = {.last_input_ms = HAL_GetTick()};
    if (home_enabled) {
        edge_home_init(&home, edge_home_buttons(), idle.last_input_ms);
        if (startup_thresholds.fan_on_tenths != 0U) {
            home.view.fan_on_tenths = (int16_t)startup_thresholds.fan_on_tenths;
            home.view.close_level = startup_thresholds.close_level;
        }
        state.home = home.view;
    }
    const TickType_t wait = pdMS_TO_TICKS(home_enabled ? CONTROL_PERIOD_MS : 150U);
    for (;;) {
        bool received = xQueueReceive(raw_queue, &state.sample, wait) == pdTRUE;
        if (received) {
            edge_last_sample = state.sample;
            ++edge_diagnostics.samples;
            if (!home_enabled && edge_uart_ready() && xQueueSend(latest_tx, &state.sample, 0) != pdTRUE) {
                EdgeSample old;
                if (xQueueReceive(latest_tx, &old, 0) == pdTRUE) {
                    count_drop();
                }
                xQueueOverwrite(latest_tx, &state.sample);
            }
        }
        if (home_enabled) {
            control_tick(&home, &state, &idle);
        }
        if (received || home_enabled) {
            xQueueOverwrite(latest_display, &state);
        }
        progress(PROCESS);
    }
}

static void display_task(void *context)
{
    (void)context;
    DisplayState state = {0};
    TickType_t next = xTaskGetTickCount();
    for (;;) {
        bool available = xQueuePeek(latest_display, &state, 0) == pdTRUE;
        if ((available || !home_enabled) &&
            !edge_display_update(&state.sample, uart_alive, home_enabled ? &state.home : NULL,
                                 state.oled_power)) {
            ++edge_diagnostics.display_errors;
        }
        progress(DISPLAY);
        vTaskDelayUntil(&next, pdMS_TO_TICKS(home_enabled ? 250U : 500U));
    }
}

static bool send_frame_with_seq(uint8_t type, uint16_t seq, const uint8_t *payload, uint16_t length)
{
    uint8_t bytes[EDGE_MAX_FRAME];
    size_t size = edge_frame_encode(bytes, type, seq, payload, length);
    if (!edge_uart_send(bytes, (uint16_t)size)) {
        ++edge_diagnostics.uart_tx_errors;
        return false;
    }
    return true;
}

static bool send_frame(uint8_t type, const uint8_t *payload, uint16_t length)
{
    return send_frame_with_seq(type, sequence++, payload, length);
}

static void send_sample(const EdgeSample *sample)
{
    uint8_t data[EDGE_SENSOR_LEN];
    edge_put16(data, (uint16_t)sample->temperature_tenths);
    edge_put16(data + 2, sample->humidity_tenths);
    edge_put16(data + 4, sample->distance_mm);
    edge_put16(data + 6, sample->flags);
    edge_put32(data + 8, sample->uptime_ms);
    edge_put32(data + 12, edge_diagnostics.sample_drops);
    if (!send_frame(EDGE_SENSOR, data, sizeof data)) {
        count_drop();
    }
}

static void send_status(void)
{
    uint8_t data[EDGE_STATUS_LEN];
    edge_put32(data, HAL_GetTick());
    edge_put32(data + 4, get_period());
    edge_put32(data + 8, parser.bad_crc);
    edge_put32(data + 12, parser.bad_header + parser.timeout);
    edge_put32(data + 16, edge_uart_faults() + edge_diagnostics.uart_tx_errors);
    edge_put32(data + 20, edge_diagnostics.display_errors);
    edge_put32(data + 24, edge_diagnostics.sample_drops);
    edge_put32(data + 28, edge_diagnostics.reset_flags);
    edge_put16(data + 32, (uint16_t)edge_diagnostics.minimum_stack_words);
    data[34] = (uint8_t)edge_board_state;
    data[35] = (uint8_t)edge_diagnostics.watchdog;
    (void)send_frame(EDGE_STATUS, data, sizeof data);
}

static void on_frame(const EdgeFrame *frame, void *context)
{
    (void)context;
    if (home_enabled) {
        HomeRequest request = {.arrived_ms = HAL_GetTick()};
        if (frame->type == EDGE_HEARTBEAT && frame->length == EDGE_HEARTBEAT_LEN) {
            last_gateway_ms = request.arrived_ms;
            gateway_seen = true;
            return;
        } else if (frame->type == EDGE_HOME_SYNC && frame->length == EDGE_HOME_SYNC_LEN) {
            request.command.session = edge_u32(frame->payload);
            if (request.command.session == 0U) {
                return;
            }
        } else if (frame->type == EDGE_HOME_COMMAND && frame->length == EDGE_HOME_COMMAND_LEN) {
            request.command = edge_home_decode_command(frame->payload);
            if (request.command.operation == 0U || frame->sequence != (uint16_t)request.command.id) {
                return;
            }
        } else {
            return;
        }
        last_gateway_ms = request.arrived_ms;
        gateway_seen = true;
        if (xQueueSend(home_requests, &request, 0) != pdTRUE) {
            count_drop();
        }
        return;
    }
    if ((frame->type == EDGE_HEARTBEAT && frame->length == EDGE_HEARTBEAT_LEN) ||
        (frame->type == EDGE_COMMAND && frame->length == EDGE_COMMAND_LEN)) {
        last_gateway_ms = HAL_GetTick();
        gateway_seen = true;
    }
    if (frame->type != EDGE_COMMAND || frame->length != EDGE_COMMAND_LEN) {
        return;
    }
    const uint8_t *request = frame->payload;
    uint8_t response[EDGE_RESPONSE_LEN];
    unsigned operation = request[4];
    uint32_t value = edge_u32(request + 5);
    uint8_t result = EDGE_OK;
    if (operation == EDGE_SET_PERIOD) {
        if (value < EDGE_PERIOD_MIN || value > EDGE_PERIOD_MAX) {
            result = EDGE_INVALID;
        } else {
            set_period(value);
        }
    } else if (operation != EDGE_GET_STATUS) {
        result = EDGE_UNSUPPORTED;
    }
    edge_put32(response, edge_u32(request));
    response[4] = (uint8_t)operation;
    response[5] = result;
    edge_put32(response + 6, get_period());
    (void)send_frame(EDGE_RESPONSE, response, sizeof response);
    if (operation == EDGE_GET_STATUS) {
        send_status();
    }
}

static void send_home_updates(uint32_t now, uint32_t *last_status)
{
    EdgeHomeAck ack;
    if (xQueueReceive(home_replies, &ack, 0) == pdTRUE) {
        uint8_t payload[EDGE_HOME_ACK_LEN];
        edge_home_encode_ack(payload, ack);
        (void)send_frame_with_seq(EDGE_HOME_ACK, (uint16_t)ack.id, payload, sizeof payload);
        /* Send the applied snapshot alongside the ACK when one is available. */
        *last_status = now - HOME_REPORT_MS;
    }
    EdgeHomeState report;
    if (now - *last_status >= HOME_REPORT_MS && xQueuePeek(home_state, &report, 0) == pdTRUE) {
        uint8_t payload[EDGE_HOME_STATE_LEN];
        edge_home_encode_state(payload, report);
        (void)send_frame(EDGE_HOME_STATE, payload, sizeof payload);
        *last_status = now;
    }
}

static void uart_task(void *context)
{
    (void)context;
    uint8_t bytes[128];
    uint32_t last_status = HAL_GetTick();
    uint32_t last_heartbeat = last_status;
    uint32_t last_faults = 0;
    uint16_t last_sensor_error = 0;
    for (;;) {
        if (edge_uart_ready()) {
            uint32_t now = HAL_GetTick();
            size_t count = edge_uart_receive(bytes, sizeof bytes);
            uint32_t faults = edge_uart_faults();
            if (faults != last_faults) {
                parser.used = 0;
                last_faults = faults;
            } else {
                edge_parser_feed(&parser, bytes, count, now, on_frame, NULL);
            }
            now = HAL_GetTick();
            uart_alive = gateway_seen && now - last_gateway_ms < 8000U;
            if (home_enabled) {
                send_home_updates(now, &last_status);
                progress(UART);
                vTaskDelay(pdMS_TO_TICKS(8U));
                continue;
            }
            EdgeSample sample;
            if (xQueueReceive(latest_tx, &sample, 0) == pdTRUE) {
                send_sample(&sample);
                const uint16_t error = sample.flags & (EDGE_DHT_TIMEOUT | EDGE_DHT_BAD |
                                        EDGE_ECHO_TIMEOUT | EDGE_ECHO_RANGE);
                if (error != last_sensor_error) {
                    uint8_t report[EDGE_ERROR_LEN];
                    edge_put16(report, error);
                    edge_put32(report + 2, sample.uptime_ms);
                    if (send_frame(EDGE_ERROR, report, sizeof report)) {
                        last_sensor_error = error;
                    }
                }
            }
            if (now - last_heartbeat >= 2000U) {
                uint8_t heartbeat[EDGE_HEARTBEAT_LEN];
                edge_put32(heartbeat, now);
                (void)send_frame(EDGE_HEARTBEAT, heartbeat, sizeof heartbeat);
                last_heartbeat = now;
            }
            if (now - last_status >= 5000U) {
                send_status();
                last_status = now;
            }
        }
        progress(UART);
        vTaskDelay(pdMS_TO_TICKS(8U));
    }
}

static void monitor_task(void *context)
{
    (void)context;
    IWDG_HandleTypeDef watchdog = {
        .Instance = IWDG, .Init = {.Prescaler = IWDG_PRESCALER_64, .Reload = 2499U}
    };
    bool started = false;
    __HAL_DBGMCU_FREEZE_IWDG();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(500U));
        bool healthy = true;
        UBaseType_t minimum = UINT32_MAX;
        for (unsigned i = 0; i < WORKER_COUNT; ++i) {
            uint32_t mark = edge_diagnostics.tasks_alive[i];
            if (mark == 0U || HAL_GetTick() - mark >= 2000U) {
                healthy = false;
            }
            UBaseType_t high_water = uxTaskGetStackHighWaterMark(workers[i]);
            if (high_water < minimum) {
                minimum = high_water;
            }
        }
        edge_diagnostics.minimum_stack_words = minimum;
        edge_diagnostics.free_heap_bytes = xPortGetFreeHeapSize();
        if (healthy) {
            if (!started) {
                started = HAL_IWDG_Init(&watchdog) == HAL_OK;
            }
            if (started) {
                (void)HAL_IWDG_Refresh(&watchdog);
            }
        }
        edge_diagnostics.watchdog = started ? (healthy ? 1U : 2U) : 0U;
    }
}

static void boot_task(void *context)
{
    (void)context;
    edge_board_init();
    home_enabled = edge_board_state == 1U && edge_pins.home.light.port != NULL;
    settings_lock = xSemaphoreCreateMutex();
    raw_queue = xQueueCreate(4, sizeof(EdgeSample));
    latest_display = xQueueCreate(1, sizeof(DisplayState));
    latest_tx = xQueueCreate(1, sizeof(EdgeSample));
    if (home_enabled) {
        latest_light = xQueueCreate(1, sizeof(LightSample));
        threshold_changes = xQueueCreate(1, sizeof(ThresholdChange));
        if (!edge_settings_open(&thresholds_store, edge_settings_flash())) {
            ++edge_settings_errors;
        }
        if (thresholds_store.valid) {
            startup_thresholds = thresholds_store.current;
        }
        if (edge_uart_ready()) {
            home_requests = xQueueCreate(4, sizeof(HomeRequest));
            home_replies = xQueueCreate(4, sizeof(EdgeHomeAck));
            home_state = xQueueCreate(1, sizeof(EdgeHomeState));
            if (home_requests == NULL || home_replies == NULL || home_state == NULL) {
                edge_fatal(2U);
            }
        }
    }
    if (settings_lock == NULL || raw_queue == NULL || latest_display == NULL || latest_tx == NULL ||
        (home_enabled && (latest_light == NULL || threshold_changes == NULL)) ||
        xTaskCreate(sensor_task, "Sensor", 384, NULL, 3, &workers[SENSOR]) != pdPASS ||
        xTaskCreate(process_task, "Process", 256, NULL, 2, &workers[PROCESS]) != pdPASS ||
        xTaskCreate(display_task, "Display", 640, NULL, 1, &workers[DISPLAY]) != pdPASS ||
        xTaskCreate(uart_task, "UART", 640, NULL, 4, &workers[UART]) != pdPASS ||
        xTaskCreate(monitor_task, "Monitor", 320, NULL, 2, NULL) != pdPASS) {
        edge_fatal(2U);
    }
    vTaskDelete(NULL);
}

void edge_app_start(void)
{
    edge_diagnostics.reset_flags = RCC->CSR & 0xFE000000UL;
    __HAL_RCC_CLEAR_RESET_FLAGS();
    if (xTaskCreate(boot_task, "Boot", 512, NULL, 5, NULL) != pdPASS) {
        edge_fatal(3U);
    }
    vTaskStartScheduler();
    edge_fatal(4U);
}

void edge_fatal(unsigned code)
{
    edge_diagnostics.fatal_code = code;
    __disable_irq();
    edge_home_outputs_off();
    for (;;) {}
}

void vApplicationMallocFailedHook(void) { edge_fatal(5U); }

void vApplicationStackOverflowHook(TaskHandle_t task, char *name)
{
    (void)task;
    (void)name;
    edge_fatal(6U);
}
