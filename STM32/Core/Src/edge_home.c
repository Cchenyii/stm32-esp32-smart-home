#include "edge_home.h"

enum {
    KEY_DEBOUNCE_MS = 30,
    MODE_HOLD_MS = 800,
    FAN_HYSTERESIS = 20,      /* Tenths of a degree Celsius. */
    LIGHT_HYSTERESIS = 500,   /* ADC counts on the 0–4095 scale. */
    CURTAIN_INTERVAL_MS = 1200,
    LOCAL_PRIORITY_MS = 1200,
    MIN_FAN_ON = 100,
    MAX_FAN_ON = 600,
    MIN_CLOSE_LEVEL = 600,
    MAX_CLOSE_LEVEL = 4000,
    JOYSTICK_TRIP_LOW = 1100,
    JOYSTICK_CENTER_LOW = 1500,
    JOYSTICK_CENTER_HIGH = 2600,
    JOYSTICK_TRIP_HIGH = 3000,
    EVENT_MODE_HOLD = 1U << EDGE_KEY_COUNT
};

uint8_t edge_home_joystick_keys(EdgeJoystickPosition *position, uint16_t y)
{
    if (y >= JOYSTICK_CENTER_LOW && y <= JOYSTICK_CENTER_HIGH) {
        *position = EDGE_JOYSTICK_CENTER;
        return 0;
    }
    if (*position == EDGE_JOYSTICK_CENTER) {
        if (y <= JOYSTICK_TRIP_LOW) {
            *position = EDGE_JOYSTICK_UP;
        } else if (y >= JOYSTICK_TRIP_HIGH) {
            *position = EDGE_JOYSTICK_DOWN;
        }
    }
    if (*position == EDGE_JOYSTICK_UP && y < JOYSTICK_CENTER_LOW) {
        return 1U << EDGE_KEY_UP;
    }
    if (*position == EDGE_JOYSTICK_DOWN && y > JOYSTICK_CENTER_HIGH) {
        return 1U << EDGE_KEY_DOWN;
    }
    if (*position != EDGE_JOYSTICK_CENTER) {
        /* Re-arm at center after startup or an unsampled pass through center. */
        *position = EDGE_JOYSTICK_WAIT_CENTER;
    }
    return 0;
}

void edge_home_init(EdgeHome *home, uint8_t buttons, uint32_t now_ms)
{
    *home = (EdgeHome){.view = {.fan_on_tenths = 300, .close_level = 3000}};
    for (unsigned i = 0; i < EDGE_KEY_COUNT; ++i) {
        bool pressed = (buttons & (1U << i)) != 0U;
        home->keys[i] = (EdgeKeyState){
            .candidate = pressed, .pressed = pressed, .long_handled = pressed,
            .changed_ms = now_ms, .pressed_ms = now_ms
        };
    }
}

static unsigned key_events(EdgeHome *home, uint8_t buttons, uint32_t now_ms)
{
    unsigned events = 0;
    for (unsigned i = 0; i < EDGE_KEY_COUNT; ++i) {
        EdgeKeyState *key = &home->keys[i];
        bool raw = (buttons & (1U << i)) != 0U;
        if (raw != key->candidate) {
            key->candidate = raw;
            key->changed_ms = now_ms;
        }
        if (raw != key->pressed && now_ms - key->changed_ms >= KEY_DEBOUNCE_MS) {
            key->pressed = raw;
            if (raw) {
                key->pressed_ms = now_ms;
                key->long_handled = false;
                if (i != EDGE_KEY_MODE) {
                    events |= 1U << i;
                }
            } else if (i == EDGE_KEY_MODE && !key->long_handled) {
                /* The hold deadline can fall between the last poll and release. */
                events |= key->changed_ms - key->pressed_ms >= MODE_HOLD_MS ?
                          EVENT_MODE_HOLD : 1U << EDGE_KEY_MODE;
            }
        }
        if (i == EDGE_KEY_MODE && raw && key->pressed && !key->long_handled &&
            now_ms - key->pressed_ms >= MODE_HOLD_MS) {
            key->long_handled = true;
            events |= EVENT_MODE_HOLD;
        }
    }
    return events;
}

static void set_mode(EdgeHome *home, bool manual, EdgeHomeSource source)
{
    EdgeHomeView *view = &home->view;
    if (view->manual != manual) {
        view->manual = manual;
        home->curtain_target = view->curtain;
    }
    view->source = source;
}

static void open_menu_item(EdgeHomeView *view)
{
    switch (view->menu_item) {
    case EDGE_HOME_MENU_MODE:
        view->mode_choice_manual = view->manual;
        view->page = EDGE_HOME_MODE_SELECT;
        break;
    case EDGE_HOME_MENU_TEMP:
        view->page = EDGE_HOME_SET_TEMP;
        break;
    case EDGE_HOME_MENU_LIGHT:
        view->page = EDGE_HOME_SET_LIGHT;
        break;
    case EDGE_HOME_MENU_BACK:
        view->page = EDGE_HOME_MAIN;
        break;
    case EDGE_HOME_MENU_COUNT:
        break;
    }
}

static void apply_buttons(EdgeHome *home, unsigned events)
{
    EdgeHomeView *view = &home->view;
    if ((events & EVENT_MODE_HOLD) != 0U) {
        view->page = EDGE_HOME_MAIN;
        return;
    }
    if ((events & (1U << EDGE_KEY_MODE)) != 0U) {
        switch (view->page) {
        case EDGE_HOME_MAIN:
            view->menu_item = EDGE_HOME_MENU_MODE;
            view->page = EDGE_HOME_MENU;
            break;
        case EDGE_HOME_MENU:
            open_menu_item(view);
            break;
        case EDGE_HOME_MODE_SELECT:
            if (view->mode_choice_manual != view->manual) {
                set_mode(home, view->mode_choice_manual, EDGE_HOME_LOCAL_SOURCE);
            }
            view->page = EDGE_HOME_MENU;
            break;
        case EDGE_HOME_SET_TEMP:
        case EDGE_HOME_SET_LIGHT:
            view->page = EDGE_HOME_MENU;
            break;
        }
        return;
    }

    bool up = (events & (1U << EDGE_KEY_UP)) != 0U;
    bool down = (events & (1U << EDGE_KEY_DOWN)) != 0U;
    if (up == down) {
        return;
    }
    switch (view->page) {
    case EDGE_HOME_MENU:
        view->menu_item = (view->menu_item + (up ? EDGE_HOME_MENU_COUNT - 1 : 1)) %
                          EDGE_HOME_MENU_COUNT;
        break;
    case EDGE_HOME_MODE_SELECT:
        view->mode_choice_manual = down;
        break;
    case EDGE_HOME_SET_TEMP: {
        int value = view->fan_on_tenths + (up ? 10 : -10);
        if (value >= MIN_FAN_ON && value <= MAX_FAN_ON) {
            view->fan_on_tenths = (int16_t)value;
            view->source = EDGE_HOME_LOCAL_SOURCE;
        }
        break;
    }
    case EDGE_HOME_SET_LIGHT: {
        int value = view->close_level + (up ? 100 : -100);
        if (value >= MIN_CLOSE_LEVEL && value <= MAX_CLOSE_LEVEL) {
            view->close_level = (uint16_t)value;
            view->source = EDGE_HOME_LOCAL_SOURCE;
        }
        break;
    }
    case EDGE_HOME_MAIN:
        if (!view->manual) {
            break;
        }
        view->source = EDGE_HOME_LOCAL_SOURCE;
        if (up) {
            view->fan_on = !view->fan_on;
        } else {
            home->curtain_target = home->curtain_target == EDGE_CURTAIN_OPEN ?
                                   EDGE_CURTAIN_CLOSED : EDGE_CURTAIN_OPEN;
        }
        break;
    }
}

int16_t edge_home_fan_off_threshold(const EdgeHomeView *view)
{
    return (int16_t)(view->fan_on_tenths - FAN_HYSTERESIS);
}

uint16_t edge_home_open_threshold(const EdgeHomeView *view)
{
    return (uint16_t)(view->close_level - LIGHT_HYSTERESIS);
}

static void apply_automatic_control(EdgeHome *home, EdgeHomeReading reading)
{
    EdgeHomeView *view = &home->view;
    if (!reading.temperature_valid || reading.temperature_tenths <= edge_home_fan_off_threshold(view)) {
        view->fan_on = false;
    } else if (reading.temperature_tenths >= view->fan_on_tenths) {
        view->fan_on = true;
    }

    home->curtain_target = view->curtain;
    if (reading.light_valid) {
        if (reading.light_level >= view->close_level) {
            home->curtain_target = EDGE_CURTAIN_CLOSED;
        } else if (reading.light_level <= edge_home_open_threshold(view)) {
            home->curtain_target = EDGE_CURTAIN_OPEN;
        }
    }
}

static void update_outputs(EdgeHome *home, EdgeHomeReading reading, uint32_t now_ms)
{
    EdgeHomeView *view = &home->view;
    if (!view->manual) {
        apply_automatic_control(home, reading);
    }
    if (view->curtain != home->curtain_target &&
        (view->curtain == EDGE_CURTAIN_IDLE || now_ms - home->curtain_changed_ms >= CURTAIN_INTERVAL_MS)) {
        view->curtain = home->curtain_target;
        home->curtain_changed_ms = now_ms;
    }
}

void edge_home_step(EdgeHome *home, EdgeHomeReading reading, uint8_t buttons, uint32_t now_ms)
{
    EdgeHomeView *view = &home->view;
    view->temperature_valid = reading.temperature_valid;
    view->light_valid = reading.light_valid;
    view->light_level = reading.light_level;
    unsigned events = key_events(home, buttons, now_ms);
    if (buttons != 0U || events != 0U) {
        home->local_ms = now_ms;
        home->local_used = true;
    } else if (now_ms - home->local_ms >= LOCAL_PRIORITY_MS) {
        home->local_used = false;
    }
    apply_buttons(home, events);
    update_outputs(home, reading, now_ms);
}

void edge_home_sync(EdgeHome *home, uint32_t session)
{
    if (session != 0U && session != home->session) {
        home->session = session;
        home->last_command.id = 0;
    }
}

static uint8_t apply_remote(EdgeHome *home, EdgeHomeCommand cmd)
{
    EdgeHomeView *view = &home->view;
    if (home->local_used || view->page != EDGE_HOME_MAIN) {
        return EDGE_LOCAL_PRIORITY;
    }
    switch (cmd.operation) {
    case EDGE_HOME_SET_MODE:
        if (cmd.value > 1U) { return EDGE_INVALID; }
        set_mode(home, cmd.value != 0U, EDGE_HOME_REMOTE_SOURCE);
        return EDGE_OK;
    case EDGE_HOME_SET_FAN:
        if (cmd.value > 1U) { return EDGE_INVALID; }
        if (!view->manual) { return EDGE_MANUAL_REQUIRED; }
        view->fan_on = cmd.value != 0U;
        break;
    case EDGE_HOME_SET_CURTAIN:
        if (cmd.value != EDGE_CURTAIN_OPEN && cmd.value != EDGE_CURTAIN_CLOSED) { return EDGE_INVALID; }
        if (!view->manual) { return EDGE_MANUAL_REQUIRED; }
        home->curtain_target = (EdgeCurtain)cmd.value;
        break;
    case EDGE_HOME_SET_FAN_THRESHOLD:
        if (cmd.value < MIN_FAN_ON || cmd.value > MAX_FAN_ON || cmd.value % 10U != 0U) {
            return EDGE_INVALID;
        }
        view->fan_on_tenths = (int16_t)cmd.value;
        break;
    case EDGE_HOME_SET_LIGHT_THRESHOLD:
        if (cmd.value < MIN_CLOSE_LEVEL || cmd.value > MAX_CLOSE_LEVEL || cmd.value % 100U != 0U) {
            return EDGE_INVALID;
        }
        view->close_level = cmd.value;
        break;
    default:
        return EDGE_UNSUPPORTED;
    }
    view->source = EDGE_HOME_REMOTE_SOURCE;
    return EDGE_OK;
}

uint8_t edge_home_remote(EdgeHome *home, EdgeHomeReading reading, EdgeHomeCommand cmd,
                         uint32_t now_ms)
{
    if (cmd.session == 0U || cmd.session != home->session || cmd.id == 0U) {
        return EDGE_STALE;
    }
    EdgeHomeCommand last = home->last_command;
    if (cmd.id == last.id) {
        return cmd.operation == last.operation && cmd.value == last.value ?
               home->last_result : EDGE_INVALID;
    }
    if (cmd.id < last.id) {
        return EDGE_STALE;
    }

    uint8_t result = apply_remote(home, cmd);
    if (result == EDGE_OK) {
        update_outputs(home, reading, now_ms);
        if (cmd.operation == EDGE_HOME_SET_CURTAIN && home->view.curtain != home->curtain_target) {
            result = EDGE_DEFERRED;
        }
    }
    home->last_command = cmd;
    home->last_result = result;
    return result;
}
