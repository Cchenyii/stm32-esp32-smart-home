#include "edge_home.h"
#include <assert.h>

#define BUTTON(key) ((uint8_t)(1U << (key)))

static void press(EdgeHome *home, EdgeHomeReading reading, EdgeHomeKey key,
                  uint32_t start, uint32_t held_ms)
{
    edge_home_step(home, reading, BUTTON(key), start);
    edge_home_step(home, reading, BUTTON(key), start + 30U);
    edge_home_step(home, reading, BUTTON(key), start + held_ms - 1U);
    edge_home_step(home, reading, 0, start + held_ms);
    edge_home_step(home, reading, 0, start + held_ms + 30U);
}

static void auto_thresholds(void)
{
    EdgeHome home;
    EdgeHomeReading reading = {0};
    edge_home_init(&home, 0, 0);
    edge_home_step(&home, reading, 0, 10);
    assert(!home.view.fan_on && home.view.curtain == EDGE_CURTAIN_IDLE);

    reading = (EdgeHomeReading){300, 3000, true, true};
    edge_home_step(&home, reading, 0, 20);
    assert(home.view.fan_on && home.view.curtain == EDGE_CURTAIN_CLOSED);

    reading = (EdgeHomeReading){290, 2750, true, true};
    edge_home_step(&home, reading, 0, 100);
    assert(home.view.fan_on && home.view.curtain == EDGE_CURTAIN_CLOSED);

    reading = (EdgeHomeReading){280, 2500, true, true};
    edge_home_step(&home, reading, 0, 1219);
    assert(!home.view.fan_on && home.view.curtain == EDGE_CURTAIN_CLOSED);
    edge_home_step(&home, reading, 0, 1220);
    assert(home.view.curtain == EDGE_CURTAIN_OPEN);

    edge_home_step(&home, (EdgeHomeReading){310, 3500, true, false}, 0, 2400);
    edge_home_step(&home, (EdgeHomeReading){310, 3500, false, false}, 0, 2430);
    assert(!home.view.fan_on && home.view.curtain == EDGE_CURTAIN_OPEN);
}

static void manual_and_settings(void)
{
    EdgeHome home;
    EdgeHomeReading reading = {320, 3800, true, true};
    edge_home_init(&home, BUTTON(EDGE_KEY_MODE), 0);
    edge_home_step(&home, reading, BUTTON(EDGE_KEY_MODE), 950);
    edge_home_step(&home, reading, 0, 960);
    edge_home_step(&home, reading, 0, 990);
    assert(!home.view.manual && home.view.page == EDGE_HOME_MAIN);

    edge_home_step(&home, reading, BUTTON(EDGE_KEY_MODE), 1000);
    edge_home_step(&home, reading, 0, 1020);
    edge_home_step(&home, reading, 0, 1050);
    assert(home.view.page == EDGE_HOME_MAIN);

    press(&home, reading, EDGE_KEY_MODE, 1100, 80);
    assert(home.view.page == EDGE_HOME_MENU && home.view.menu_item == EDGE_HOME_MENU_MODE);
    assert(!home.view.manual && home.view.source == EDGE_HOME_DEFAULT_SOURCE);
    press(&home, reading, EDGE_KEY_MODE, 1250, 80);
    assert(home.view.page == EDGE_HOME_MODE_SELECT && !home.view.mode_choice_manual);
    press(&home, reading, EDGE_KEY_DOWN, 1400, 80);
    assert(home.view.mode_choice_manual && !home.view.manual);
    press(&home, reading, EDGE_KEY_MODE, 1550, 900);
    assert(home.view.page == EDGE_HOME_MAIN && !home.view.manual);
    assert(home.view.source == EDGE_HOME_DEFAULT_SOURCE);

    press(&home, reading, EDGE_KEY_MODE, 2500, 80);
    press(&home, reading, EDGE_KEY_MODE, 2650, 80);
    assert(!home.view.mode_choice_manual);
    press(&home, reading, EDGE_KEY_DOWN, 2800, 80);
    press(&home, reading, EDGE_KEY_MODE, 2950, 80);
    assert(home.view.page == EDGE_HOME_MENU && home.view.manual && home.view.fan_on);
    assert(home.view.curtain == EDGE_CURTAIN_CLOSED && home.view.source == EDGE_HOME_LOCAL_SOURCE);
    press(&home, reading, EDGE_KEY_UP, 3100, 80);
    assert(home.view.menu_item == EDGE_HOME_MENU_BACK);
    press(&home, reading, EDGE_KEY_MODE, 3250, 80);
    assert(home.view.page == EDGE_HOME_MAIN && home.view.manual);

    reading.temperature_valid = false;
    reading.light_valid = false;
    press(&home, reading, EDGE_KEY_UP, 3400, 800);
    assert(!home.view.fan_on);
    press(&home, reading, EDGE_KEY_DOWN, 4250, 80);
    assert(home.view.curtain == EDGE_CURTAIN_OPEN);

    press(&home, reading, EDGE_KEY_MODE, 4400, 80);
    press(&home, reading, EDGE_KEY_UP, 4550, 80);
    press(&home, reading, EDGE_KEY_DOWN, 4700, 80);
    assert(home.view.menu_item == EDGE_HOME_MENU_MODE);
    press(&home, reading, EDGE_KEY_DOWN, 4850, 80);
    press(&home, reading, EDGE_KEY_MODE, 5000, 80);
    assert(home.view.page == EDGE_HOME_SET_TEMP && !home.view.fan_on);
    press(&home, reading, EDGE_KEY_UP, 5150, 80);
    assert(home.view.fan_on_tenths == 310 && edge_home_fan_off_threshold(&home.view) == 290);
    press(&home, reading, EDGE_KEY_MODE, 5300, 80);
    assert(home.view.page == EDGE_HOME_MENU && home.view.menu_item == EDGE_HOME_MENU_TEMP);
    press(&home, reading, EDGE_KEY_DOWN, 5450, 80);
    press(&home, reading, EDGE_KEY_MODE, 5600, 80);
    assert(home.view.page == EDGE_HOME_SET_LIGHT);
    press(&home, reading, EDGE_KEY_DOWN, 5750, 80);
    assert(home.view.close_level == 2900 && edge_home_open_threshold(&home.view) == 2400);
    press(&home, reading, EDGE_KEY_MODE, 5900, 80);
    assert(home.view.page == EDGE_HOME_MENU && home.view.menu_item == EDGE_HOME_MENU_LIGHT);
    press(&home, reading, EDGE_KEY_MODE, 6050, 900);
    assert(home.view.page == EDGE_HOME_MAIN && home.view.manual);

    reading = (EdgeHomeReading){315, 3000, true, true};
    press(&home, reading, EDGE_KEY_MODE, 7000, 80);
    press(&home, reading, EDGE_KEY_MODE, 7150, 80);
    assert(home.view.mode_choice_manual);
    press(&home, reading, EDGE_KEY_UP, 7300, 80);
    assert(home.view.manual && !home.view.fan_on && home.view.curtain == EDGE_CURTAIN_OPEN);
    press(&home, reading, EDGE_KEY_MODE, 7450, 80);
    assert(!home.view.manual && home.view.fan_on && home.view.curtain == EDGE_CURTAIN_CLOSED);
}

static void timer_wrap(void)
{
    EdgeHome home;
    EdgeHomeReading reading = {0};
    uint32_t start = UINT32_MAX - 50U;
    edge_home_init(&home, 0, start);
    press(&home, reading, EDGE_KEY_MODE, start + 10U, 90U);
    assert(home.view.page == EDGE_HOME_MENU && !home.view.manual);
}

static void hold_deadline_between_polls(void)
{
    EdgeHome home;
    EdgeHomeReading reading = {0};
    edge_home_init(&home, 0, 0);
    press(&home, reading, EDGE_KEY_MODE, 100, 80);
    press(&home, reading, EDGE_KEY_MODE, 250, 830);
    assert(home.view.page == EDGE_HOME_MAIN && !home.view.manual);
}

static void remote_control_and_replay(void)
{
    EdgeHome home;
    EdgeHomeReading reading = {300, 3300, true, true};
    EdgeHomeCommand command = {.session = 456, .id = 1, .operation = EDGE_HOME_SET_FAN, .value = 0};
    edge_home_init(&home, 0, 0);
    edge_home_step(&home, reading, 0, 100);
    assert(edge_home_remote(&home, reading, command, 100) == EDGE_STALE);
    edge_home_sync(&home, command.session);
    assert(edge_home_remote(&home, reading, command, 110) == EDGE_MANUAL_REQUIRED);
    assert(home.view.fan_on);

    command = (EdgeHomeCommand){.session = 456, .id = 2, .operation = EDGE_HOME_SET_MODE, .value = 1};
    assert(edge_home_remote(&home, reading, command, 120) == EDGE_OK);
    assert(home.view.manual && home.view.fan_on && home.view.source == EDGE_HOME_REMOTE_SOURCE);
    command = (EdgeHomeCommand){.session = 456, .id = 3, .operation = EDGE_HOME_SET_FAN, .value = 0};
    assert(edge_home_remote(&home, reading, command, 130) == EDGE_OK);
    assert(!home.view.fan_on);

    press(&home, reading, EDGE_KEY_UP, 140, 80);
    assert(home.view.fan_on && home.view.source == EDGE_HOME_LOCAL_SOURCE);
    assert(edge_home_remote(&home, reading, command, 250) == EDGE_OK);
    assert(home.view.fan_on); /* Cached ACK does not apply the old target again. */
    command.id = 4;
    assert(edge_home_remote(&home, reading, command, 250) == EDGE_LOCAL_PRIORITY);
    edge_home_step(&home, reading, 0, 1450);
    assert(edge_home_remote(&home, reading, command, 1450) == EDGE_LOCAL_PRIORITY);
    command.id = 5;
    assert(edge_home_remote(&home, reading, command, 1450) == EDGE_OK && !home.view.fan_on);
    command.value = 1;
    assert(edge_home_remote(&home, reading, command, 1450) == EDGE_INVALID);
    command.id = 2;
    assert(edge_home_remote(&home, reading, command, 1450) == EDGE_STALE);

    command = (EdgeHomeCommand){.session = 456, .id = 6, .operation = EDGE_HOME_SET_CURTAIN, .value = 1};
    assert(edge_home_remote(&home, reading, command, 1460) == EDGE_OK);
    assert(home.view.curtain == EDGE_CURTAIN_OPEN);
    command.id = 7;
    command.value = 2;
    assert(edge_home_remote(&home, reading, command, 1500) == EDGE_DEFERRED);
    assert(home.view.curtain == EDGE_CURTAIN_OPEN && home.curtain_target == EDGE_CURTAIN_CLOSED);
    edge_home_step(&home, reading, 0, 2660);
    assert(home.view.curtain == EDGE_CURTAIN_CLOSED);

    edge_home_sync(&home, 457);
    assert(edge_home_remote(&home, reading, command, 2700) == EDGE_STALE);
    command = (EdgeHomeCommand){.session = 457, .id = 1, .operation = EDGE_HOME_SET_FAN_THRESHOLD, .value = 310};
    assert(edge_home_remote(&home, reading, command, 2700) == EDGE_OK);
    assert(home.view.fan_on_tenths == 310 && edge_home_fan_off_threshold(&home.view) == 290);
    edge_home_sync(&home, 457);
    command.value = 320;
    assert(edge_home_remote(&home, reading, command, 2700) == EDGE_INVALID);
}

static void remote_priority_while_in_menu(void)
{
    EdgeHome home;
    EdgeHomeReading reading = {250, 3500, true, true};
    EdgeHomeCommand command = {.session = 123, .id = 1, .operation = EDGE_HOME_SET_MODE, .value = 1};
    edge_home_init(&home, 0, 0);
    edge_home_sync(&home, command.session);
    press(&home, reading, EDGE_KEY_MODE, 100, 80);
    assert(home.view.page == EDGE_HOME_MENU && !home.view.fan_on);

    reading.temperature_tenths = 310;
    edge_home_step(&home, reading, 0, 1500);
    assert(!home.local_used && home.view.fan_on);
    assert(edge_home_remote(&home, reading, command, 1500) == EDGE_LOCAL_PRIORITY);
    press(&home, reading, EDGE_KEY_MODE, 1600, 900);
    command.id++;
    assert(edge_home_remote(&home, reading, command, 2530) == EDGE_LOCAL_PRIORITY);
    edge_home_step(&home, reading, 0, 3700);
    command.id++;
    assert(edge_home_remote(&home, reading, command, 3700) == EDGE_OK && home.view.manual);
}

static void joystick_navigation(void)
{
    EdgeHome home;
    EdgeHomeReading reading = {0};
    EdgeJoystickPosition position = EDGE_JOYSTICK_WAIT_CENTER;
    edge_home_init(&home, 0, 0);
    assert(edge_home_joystick_keys(&position, 3200) == 0); /* Held at power-on. */
    assert(edge_home_joystick_keys(&position, 2100) == 0);
    press(&home, reading, EDGE_KEY_MODE, 100, 80);
    assert(home.view.menu_item == EDGE_HOME_MENU_MODE);

    assert(edge_home_joystick_keys(&position, 2940) == 0);
    uint8_t down = edge_home_joystick_keys(&position, 3200);
    assert(down == BUTTON(EDGE_KEY_DOWN));
    edge_home_step(&home, reading, down, 230);
    edge_home_step(&home, reading, edge_home_joystick_keys(&position, 2800), 260);
    assert(home.view.menu_item == EDGE_HOME_MENU_TEMP);
    edge_home_step(&home, reading, edge_home_joystick_keys(&position, 4000), 400);
    assert(home.view.menu_item == EDGE_HOME_MENU_TEMP);

    assert(edge_home_joystick_keys(&position, 600) == 0); /* Center was skipped. */
    edge_home_step(&home, reading, edge_home_joystick_keys(&position, 2050), 460);
    edge_home_step(&home, reading, edge_home_joystick_keys(&position, 2150), 490);
    uint8_t up = edge_home_joystick_keys(&position, 900);
    assert(up == BUTTON(EDGE_KEY_UP));
    edge_home_step(&home, reading, up, 510);
    edge_home_step(&home, reading, edge_home_joystick_keys(&position, 1250), 540);
    assert(home.view.menu_item == EDGE_HOME_MENU_MODE);
}

int main(void)
{
    auto_thresholds();
    manual_and_settings();
    timer_wrap();
    hold_deadline_between_polls();
    remote_control_and_replay();
    remote_priority_while_in_menu();
    joystick_navigation();
    return 0;
}
