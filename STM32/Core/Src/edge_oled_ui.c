#include "edge_oled_ui.h"
#include <stdio.h>
#include <string.h>

uint8_t edge_oled_idle_step(EdgeOledIdle *idle, uint8_t buttons, uint32_t now_ms)
{
    if (buttons != 0U) {
        if (idle->power == EDGE_OLED_OFF) {
            idle->wake_hold = true;
        }
        idle->last_input_ms = now_ms;
        idle->power = EDGE_OLED_BRIGHT;
        return idle->wake_hold ? 0U : buttons;
    }
    idle->wake_hold = false;
    uint32_t elapsed = now_ms - idle->last_input_ms;
    idle->power = elapsed >= 60000U ? EDGE_OLED_OFF :
                  elapsed >= 30000U ? EDGE_OLED_DIM : EDGE_OLED_BRIGHT;
    return 0U;
}

static const uint8_t digits[10][5] = {
    {0x3e,0x51,0x49,0x45,0x3e}, {0,0x42,0x7f,0x40,0}, {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4b,0x31}, {0x18,0x14,0x12,0x7f,0x10}, {0x27,0x45,0x45,0x45,0x39},
    {0x3c,0x4a,0x49,0x49,0x30}, {1,0x71,9,5,3}, {0x36,0x49,0x49,0x49,0x36},
    {6,0x49,0x49,0x29,0x1e}
};

static const uint8_t letters[26][5] = {
    ['A'-'A'] = {0x7e,9,9,9,0x7e}, ['B'-'A'] = {0x7f,0x49,0x49,0x49,0x36},
    ['C'-'A'] = {0x3e,0x41,0x41,0x41,0x22},
    ['D'-'A'] = {0x7f,0x41,0x41,0x22,0x1c}, ['E'-'A'] = {0x7f,0x49,0x49,0x49,0x41},
    ['F'-'A'] = {0x7f,9,9,9,1}, ['G'-'A'] = {0x3e,0x41,0x49,0x49,0x7a},
    ['H'-'A'] = {0x7f,8,8,8,0x7f}, ['I'-'A'] = {0,0x41,0x7f,0x41,0},
    ['K'-'A'] = {0x7f,8,0x14,0x22,0x41}, ['L'-'A'] = {0x7f,0x40,0x40,0x40,0x40},
    ['M'-'A'] = {0x7f,2,0x0c,2,0x7f}, ['N'-'A'] = {0x7f,4,8,0x10,0x7f},
    ['O'-'A'] = {0x3e,0x41,0x41,0x41,0x3e}, ['P'-'A'] = {0x7f,9,9,9,6},
    ['R'-'A'] = {0x7f,9,0x19,0x29,0x46}, ['S'-'A'] = {0x46,0x49,0x49,0x49,0x31},
    ['T'-'A'] = {1,1,0x7f,1,1}, ['U'-'A'] = {0x3f,0x40,0x40,0x40,0x3f},
    ['W'-'A'] = {0x3f,0x40,0x38,0x40,0x3f}, ['X'-'A'] = {0x63,0x14,8,0x14,0x63}
};

static const uint8_t *glyph(char character)
{
    static const uint8_t dash[5] = {8,8,8,8,8};
    static const uint8_t decimal[5] = {0,0x60,0x60,0,0};
    static const uint8_t percent[5] = {0x23,0x13,8,0x64,0x62};
    static const uint8_t slash[5] = {0x20,0x10,8,4,2};
    static const uint8_t chevron[5] = {0,0x22,0x14,8,0};
    if (character >= '0' && character <= '9') {
        return digits[character - '0'];
    }
    if (character >= 'A' && character <= 'Z') {
        return letters[character - 'A'];
    }
    switch (character) {
    case '-': return dash;
    case '.': return decimal;
    case '%': return percent;
    case '/': return slash;
    case '>': return chevron;
    default: return NULL;
    }
}

static void fill(EdgeOledFrame frame, unsigned x, unsigned y, unsigned width, unsigned height)
{
    for (unsigned row = y; row < y + height && row < EDGE_OLED_HEIGHT; ++row) {
        for (unsigned col = x; col < x + width && col < EDGE_OLED_WIDTH; ++col) {
            frame[row / 8U][col] |= (uint8_t)(1U << (row % 8U));
        }
    }
}

void edge_oled_text(EdgeOledFrame frame, unsigned x, unsigned y, unsigned scale, const char *text)
{
    for (; *text != '\0' && x < EDGE_OLED_WIDTH; ++text, x += 6U * scale) {
        const uint8_t *bits = glyph(*text);
        if (bits == NULL) {
            continue;
        }
        for (unsigned col = 0; col < 5; ++col) {
            for (unsigned row = 0; row < 7; ++row) {
                if ((bits[col] & (1U << row)) != 0U) {
                    fill(frame, x + col * scale, y + row * scale, scale, scale);
                }
            }
        }
    }
}

static void outline(EdgeOledFrame frame, unsigned x, unsigned y, unsigned width, unsigned height)
{
    fill(frame, x + 1U, y, width - 2U, 1);
    fill(frame, x + 1U, y + height - 1U, width - 2U, 1);
    fill(frame, x, y + 1U, 1, height - 2U);
    fill(frame, x + width - 1U, y + 1U, 1, height - 2U);
}

static void badge(EdgeOledFrame frame, unsigned x, unsigned y, unsigned width, const char *text)
{
    outline(frame, x, y, width, 11);
    unsigned text_width = (unsigned)strlen(text) * 6U - 1U;
    edge_oled_text(frame, x + (width - text_width) / 2U, y + 2U, 1, text);
}

static void header(EdgeOledFrame frame, const EdgeHomeView *home, EdgeOledReadings readings,
                   const char *title)
{
    edge_oled_text(frame, 4, 2, 1, title);
    badge(frame, 66, 0, 42, home->manual ? "MANUAL" : "AUTO");
    if (readings.link_configured) {
        outline(frame, 114, 1, 10, 9);
        if (readings.link_alive) {
            fill(frame, 117, 4, 4, 3);
        }
    }
    fill(frame, 3, 12, 122, 1);
}

static void overview(EdgeOledFrame frame, const EdgeHomeView *home, EdgeOledReadings readings)
{
    edge_oled_text(frame, 4, 17, 1, "TEMP");
    char temperature[14] = "--";
    char humidity[14] = "--";
    if (home->temperature_valid) {
        int value = readings.temperature_tenths;
        unsigned magnitude = (unsigned)(value < 0 ? -value : value);
        (void)snprintf(temperature, sizeof temperature, "%s%u.%u", value < 0 ? "-" : "",
                       magnitude / 10U, magnitude % 10U);
        (void)snprintf(humidity, sizeof humidity, "%u.%u%%",
                       readings.humidity_tenths / 10U, readings.humidity_tenths % 10U);
        char age[5];
        unsigned seconds = readings.sample_age_s > 99U ? 99U : (unsigned)readings.sample_age_s;
        (void)snprintf(age, sizeof age, "%uS", seconds);
        edge_oled_text(frame, 69U - (unsigned)strlen(age) * 6U, 17, 1, age);
    }
    edge_oled_text(frame, 4, 29, 2, temperature);
    edge_oled_text(frame, 65, 29, 1, "C");
    fill(frame, 76, 16, 1, 30);

    edge_oled_text(frame, 82, 16, 1, "HUM");
    edge_oled_text(frame, 82, 25, 1, humidity);
    edge_oled_text(frame, 82, 35, 1, "LGT");
    outline(frame, 82, 44, 42, 5);
    if (home->light_valid) {
        unsigned percent = ((unsigned)home->light_level * 100U + 2047U) / 4095U;
        char light[7];
        (void)snprintf(light, sizeof light, "%u%%", percent);
        edge_oled_text(frame, 126U - (unsigned)strlen(light) * 6U, 35, 1, light);
        fill(frame, 83, 45, (percent * 40U + 50U) / 100U, 3);
    } else {
        edge_oled_text(frame, 111, 35, 1, "--");
    }

    badge(frame, 3, 52, 49, home->fan_on ? "FAN ON" : "FAN OFF");
    const char *curtain = "CUR IDLE";
    if (home->curtain == EDGE_CURTAIN_OPEN) {
        curtain = "CUR OPEN";
    } else if (home->curtain == EDGE_CURTAIN_CLOSED) {
        curtain = "CUR CLOSED";
    }
    badge(frame, 56, 52, 69, curtain);
}

static void select_row(EdgeOledFrame frame, unsigned y, unsigned height,
                       const char *label, bool selected)
{
    unsigned text_y = y + (height - 7U) / 2U;
    if (selected) {
        outline(frame, 3, y, 122, height);
        edge_oled_text(frame, 7, text_y, 1, ">");
    }
    edge_oled_text(frame, 17, text_y, 1, label);
}

static void menu(EdgeOledFrame frame, const EdgeHomeView *home)
{
    static const char *const labels[EDGE_HOME_MENU_COUNT] = {
        [EDGE_HOME_MENU_MODE] = "RUN MODE",
        [EDGE_HOME_MENU_TEMP] = "FAN TEMP",
        [EDGE_HOME_MENU_LIGHT] = "LIGHT LIMIT",
        [EDGE_HOME_MENU_BACK] = "BACK HOME"
    };
    for (unsigned item = 0; item < EDGE_HOME_MENU_COUNT; ++item) {
        select_row(frame, 14U + 10U * item, 10, labels[item], home->menu_item == item);
    }
    fill(frame, 3, 55, 122, 1);
    edge_oled_text(frame, 4, 57, 1, "UP/DN SELECT");
    edge_oled_text(frame, 83, 57, 1, "MODE OK");
}

static void mode_selection(EdgeOledFrame frame, const EdgeHomeView *home)
{
    select_row(frame, 16, 14, "AUTO", !home->mode_choice_manual);
    select_row(frame, 32, 14, "MANUAL", home->mode_choice_manual);
    fill(frame, 3, 49, 122, 1);
    edge_oled_text(frame, 4, 54, 1, "UP/DN SELECT");
    edge_oled_text(frame, 83, 54, 1, "MODE OK");
}

static void thresholds(EdgeOledFrame frame, const EdgeHomeView *home, bool temperature)
{
    char upper[12];
    char lower[12];
    if (temperature) {
        (void)snprintf(upper, sizeof upper, "%dC", home->fan_on_tenths / 10);
        (void)snprintf(lower, sizeof lower, "%dC", edge_home_fan_off_threshold(home) / 10);
    } else {
        (void)snprintf(upper, sizeof upper, "%u", (unsigned)home->close_level);
        (void)snprintf(lower, sizeof lower, "%u", (unsigned)edge_home_open_threshold(home));
    }
    edge_oled_text(frame, 4, 17, 1, temperature ? "FAN ON" : "CLOSE");
    edge_oled_text(frame, 70, 17, 1, temperature ? "FAN OFF" : "OPEN");
    edge_oled_text(frame, 4, 29, 2, upper);
    edge_oled_text(frame, 70, 29, 2, lower);
    fill(frame, 63, 16, 1, 30);
    fill(frame, 3, 49, 122, 1);
    edge_oled_text(frame, 4, 54, 1, "UP/DN EDIT");
    edge_oled_text(frame, 72, 54, 1, "MODE BACK");
}

void edge_oled_home(EdgeOledFrame frame, const EdgeHomeView *home, EdgeOledReadings readings)
{
    memset(frame, 0, sizeof(EdgeOledFrame));
    switch (home->page) {
    case EDGE_HOME_MENU:
        header(frame, home, readings, "MENU");
        menu(frame, home);
        break;
    case EDGE_HOME_MODE_SELECT:
        header(frame, home, readings, "SET MODE");
        mode_selection(frame, home);
        break;
    case EDGE_HOME_SET_TEMP:
    case EDGE_HOME_SET_LIGHT: {
        bool temperature = home->page == EDGE_HOME_SET_TEMP;
        header(frame, home, readings, temperature ? "SET TEMP" : "SET LIGHT");
        thresholds(frame, home, temperature);
        break;
    }
    case EDGE_HOME_MAIN:
    default:
        header(frame, home, readings, "HOME");
        overview(frame, home, readings);
        break;
    }
}
