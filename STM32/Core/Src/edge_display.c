#include "edge_board.h"
#include "edge_oled_ui.h"
#include "edge_wire.h"
#include <stdio.h>
#include <string.h>

static I2C_HandleTypeDef i2c;
static bool initialized;
static uint8_t page_valid;
static EdgeOledFrame frame, page_cache;
static uint32_t last_full_refresh_ms;
static unsigned failed_updates;
volatile uint8_t edge_oled_address;
volatile EdgeOledPower edge_oled_power = EDGE_OLED_UNKNOWN;
volatile uint32_t edge_oled_pages_written;

void edge_display_init(void)
{
    __HAL_RCC_I2C1_CLK_ENABLE();
    edge_pin_init(edge_pins.scl, GPIO_MODE_AF_OD, GPIO_NOPULL);
    edge_pin_init(edge_pins.sda, GPIO_MODE_AF_OD, GPIO_NOPULL);
    i2c.Instance = I2C1;
    i2c.Init = (I2C_InitTypeDef){
        .ClockSpeed = 100000U, .DutyCycle = I2C_DUTYCYCLE_2,
        .AddressingMode = I2C_ADDRESSINGMODE_7BIT
    };
    (void)HAL_I2C_Init(&i2c);
}

static bool transmit(uint8_t *data, uint16_t length)
{
    return HAL_I2C_Master_Transmit(&i2c, (uint16_t)(edge_oled_address << 1),
                                   data, length, 25U) == HAL_OK;
}

static bool display_failed(void)
{
    initialized = false;
    edge_oled_address = 0;
    edge_oled_power = EDGE_OLED_UNKNOWN;
    if (++failed_updates >= 4U) {
        /* The F407 can hold SCL low until I2C1 is reset. HAL init pulses SWRST. */
        (void)HAL_I2C_Init(&i2c);
        failed_updates = 0;
    }
    return false;
}

static bool find_display(void)
{
    const uint8_t first = edge_pins.oled_address ? edge_pins.oled_address : 0x3CU;
    const uint8_t last = edge_pins.oled_address ? edge_pins.oled_address : 0x3DU;
    edge_oled_address = 0;
    for (uint8_t address = first; address <= last; ++address) {
        if (HAL_I2C_IsDeviceReady(&i2c, (uint16_t)(address << 1), 2, 10U) == HAL_OK) {
            edge_oled_address = address;
            return true;
        }
    }
    return false;
}

static bool write_page(unsigned number)
{
    if ((page_valid & (1U << number)) != 0U &&
        memcmp(page_cache[number], frame[number], EDGE_OLED_WIDTH) == 0) {
        return true;
    }
    uint8_t select[] = {0, (uint8_t)(0xB0U + number), 0x00, 0x10};
    uint8_t data[1U + EDGE_OLED_WIDTH] = {0x40};
    memcpy(data + 1U, frame[number], EDGE_OLED_WIDTH);
    if (!transmit(select, sizeof select) || !transmit(data, sizeof data)) {
        return false;
    }
    memcpy(page_cache[number], frame[number], EDGE_OLED_WIDTH);
    page_valid |= (uint8_t)(1U << number);
    ++edge_oled_pages_written;
    return true;
}

static bool configure_display(bool white_background)
{
    if (!find_display()) {
        return false;
    }
    uint8_t setup[] = {
        0, 0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0, 0x40, 0x8D, 0x14,
        0x20, 0x02, 0xA1, 0xC8, 0xDA, 0x12, 0x81, white_background ? 0x40 : 0x7F,
        0xD9, 0xF1, 0xDB, 0x40, 0xA4, white_background ? 0xA7 : 0xA6
    };
    return transmit(setup, sizeof setup);
}

static void render_monitor(const EdgeSample *sample, bool uart_alive)
{
    char temperature[22] = "T NA";
    char humidity[22] = "H NA";
    char distance[22] = "D NA";
    if ((sample->flags & EDGE_TH_VALID) != 0U) {
        int value = sample->temperature_tenths;
        unsigned magnitude = (unsigned)(value < 0 ? -value : value);
        (void)snprintf(temperature, sizeof temperature, "T %s%u.%u C", value < 0 ? "-" : "",
                       magnitude / 10U, magnitude % 10U);
        (void)snprintf(humidity, sizeof humidity, "H %u.%u %%", sample->humidity_tenths / 10U,
                       sample->humidity_tenths % 10U);
    }
    if ((sample->flags & EDGE_DISTANCE_VALID) != 0U) {
        (void)snprintf(distance, sizeof distance, "D %u MM", (unsigned)sample->distance_mm);
    }
    memset(frame, 0, sizeof frame);
    edge_oled_text(frame, 0, 0, 1, temperature);
    edge_oled_text(frame, 0, 16, 1, humidity);
    edge_oled_text(frame, 0, 32, 1, distance);
    edge_oled_text(frame, 0, 48, 1, uart_alive ? "LINK ON" : "LINK OFF");
}

bool edge_display_update(const EdgeSample *sample, bool uart_alive, const EdgeHomeView *home,
                         EdgeOledPower power)
{
    if (i2c.Instance == NULL) {
        return true;
    }
    uint32_t now = HAL_GetTick();
    if (!initialized) {
        page_valid = 0;
        last_full_refresh_ms = now;
        if (!configure_display(home != NULL)) {
            return display_failed();
        }
        initialized = true;
        edge_oled_power = EDGE_OLED_OFF;
    }
    if (power == EDGE_OLED_OFF) {
        if (edge_oled_power != EDGE_OLED_OFF) {
            uint8_t off[] = {0, 0xAE};
            if (!transmit(off, sizeof off)) {
                return display_failed();
            }
            edge_oled_power = EDGE_OLED_OFF;
        }
        failed_updates = 0;
        return true;
    }
    if (power != edge_oled_power) {
        uint8_t contrast[] = {0, 0x81, power == EDGE_OLED_DIM ? 0x18 : home != NULL ? 0x40 : 0x7F};
        if (!transmit(contrast, sizeof contrast)) {
            return display_failed();
        }
    }
    if (edge_oled_power == EDGE_OLED_OFF) {
        page_valid = 0;
        last_full_refresh_ms = now;
    }
    if (home != NULL) {
        if (now - last_full_refresh_ms >= 2000U) {
            page_valid = 0;
            last_full_refresh_ms = now;
        }
        edge_oled_home(frame, home, (EdgeOledReadings){
            .temperature_tenths = sample->temperature_tenths, .humidity_tenths = sample->humidity_tenths,
            .sample_age_s = (now - sample->uptime_ms) / 1000U,
            .link_configured = edge_pins.tx.port != NULL, .link_alive = uart_alive
        });
    } else {
        /* Keep the original monitor's periodic writes to pages 0, 2, 4 and 6. */
        page_valid &= 0xAAU;
        render_monitor(sample, uart_alive);
    }

    for (unsigned i = 0; i < EDGE_OLED_PAGES; ++i) {
        if (!write_page(i)) {
            return display_failed();
        }
    }
    if (edge_oled_power == EDGE_OLED_OFF) {
        /* Write the current frame before lighting the panel. */
        uint8_t on[] = {0, 0xAF};
        if (!transmit(on, sizeof on)) {
            return display_failed();
        }
    }
    edge_oled_power = power;
    failed_updates = 0;
    return true;
}
