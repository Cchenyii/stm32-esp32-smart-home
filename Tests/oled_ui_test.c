#include "edge_oled_ui.h"
#include <assert.h>
#include <stdio.h>

static bool pixel(EdgeOledFrame frame, unsigned x, unsigned y)
{
    return (frame[y / 8U][x] & (1U << (y % 8U))) != 0U;
}

int main(int argc, char **argv)
{
    struct {
        uint8_t before;
        EdgeOledFrame frame;
        uint8_t after;
    } screen = {.before = 0x35, .after = 0xA9};
    EdgeHome home;
    edge_home_init(&home, 0, 0);
    EdgeHomeView view = home.view;
    view.temperature_valid = true;
    view.light_valid = true;
    view.light_level = EDGE_LIGHT_MAX;
    view.fan_on = true;
    view.curtain = EDGE_CURTAIN_CLOSED;
    EdgeOledReadings readings = {
        .temperature_tenths = -400, .humidity_tenths = 1000, .sample_age_s = UINT32_MAX,
        .link_configured = true, .link_alive = true
    };
    edge_oled_home(screen.frame, &view, readings);
    assert(pixel(screen.frame, 4, 35));  /* Negative sign fits the temperature panel. */
    assert(pixel(screen.frame, 117, 4)); /* Linked icon has a filled center. */
    assert(pixel(screen.frame, 83, 46) && pixel(screen.frame, 122, 46));
    assert(screen.before == 0x35 && screen.after == 0xA9);
    for (unsigned y = 0; y < EDGE_OLED_HEIGHT; ++y) {
        assert(!pixel(screen.frame, 0, y) && !pixel(screen.frame, 127, y));
    }

    view.temperature_valid = false;
    view.light_valid = false;
    readings.link_alive = false;
    edge_oled_home(screen.frame, &view, readings);
    assert(!pixel(screen.frame, 117, 4) && !pixel(screen.frame, 83, 46));

    view.page = EDGE_HOME_SET_TEMP;
    edge_oled_home(screen.frame, &view, readings);
    assert(pixel(screen.frame, 63, 32) && pixel(screen.frame, 4, 49));
    assert(screen.before == 0x35 && screen.after == 0xA9);

    view.page = EDGE_HOME_MENU;
    for (unsigned selected = 0; selected < EDGE_HOME_MENU_COUNT; ++selected) {
        view.menu_item = (EdgeHomeMenuItem)selected;
        edge_oled_home(screen.frame, &view, readings);
        for (unsigned row = 0; row < EDGE_HOME_MENU_COUNT; ++row) {
            assert(pixel(screen.frame, 4, 14 + 10 * row) == (row == selected));
            assert(pixel(screen.frame, 10, 18 + 10 * row) == (row == selected));
        }
        assert(pixel(screen.frame, 4, 55) && pixel(screen.frame, 83, 63));
    }

    view.page = EDGE_HOME_MODE_SELECT;
    edge_oled_home(screen.frame, &view, readings);
    assert(pixel(screen.frame, 4, 16) && !pixel(screen.frame, 4, 32));
    view.mode_choice_manual = true;
    edge_oled_home(screen.frame, &view, readings);
    assert(!pixel(screen.frame, 4, 16) && pixel(screen.frame, 4, 32));
    assert(screen.before == 0x35 && screen.after == 0xA9);

    if (argc == 2) {
        static const EdgeHomePage pages[] = {
            EDGE_HOME_MAIN, EDGE_HOME_MENU, EDGE_HOME_MODE_SELECT,
            EDGE_HOME_SET_TEMP, EDGE_HOME_SET_LIGHT
        };
        FILE *output = fopen(argv[1], "wb");
        if (output == NULL) {
            return 1;
        }
        view.temperature_valid = true;
        view.light_valid = true;
        view.light_level = 2760;
        view.fan_on = false;
        readings.temperature_tenths = 246;
        readings.humidity_tenths = 583;
        readings.sample_age_s = 1;
        readings.link_alive = true;
        view.menu_item = EDGE_HOME_MENU_MODE;
        for (unsigned i = 0; i < sizeof pages / sizeof pages[0]; ++i) {
            view.page = pages[i];
            edge_oled_home(screen.frame, &view, readings);
            if (fwrite(screen.frame, 1, sizeof screen.frame, output) != sizeof screen.frame) {
                fclose(output);
                return 1;
            }
        }
        if (fclose(output) != 0) {
            return 1;
        }
    }
    puts("OLED: boundaries, readings, menu highlights, mode selection and thresholds passed");
}
