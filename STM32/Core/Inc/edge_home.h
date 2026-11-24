#ifndef EDGE_HOME_H
#define EDGE_HOME_H

#include <stdbool.h>
#include <stdint.h>
#include "edge_wire.h"

enum { EDGE_LIGHT_MAX = 4095 };

typedef enum {
    EDGE_KEY_MODE, EDGE_KEY_UP, EDGE_KEY_DOWN, EDGE_KEY_COUNT
} EdgeHomeKey;

typedef enum {
    EDGE_JOYSTICK_WAIT_CENTER, EDGE_JOYSTICK_CENTER, EDGE_JOYSTICK_UP, EDGE_JOYSTICK_DOWN
} EdgeJoystickPosition;

typedef enum {
    EDGE_HOME_MAIN, EDGE_HOME_SET_TEMP, EDGE_HOME_SET_LIGHT,
    EDGE_HOME_MENU, EDGE_HOME_MODE_SELECT
} EdgeHomePage;

typedef enum {
    EDGE_HOME_MENU_MODE, EDGE_HOME_MENU_TEMP, EDGE_HOME_MENU_LIGHT,
    EDGE_HOME_MENU_BACK, EDGE_HOME_MENU_COUNT
} EdgeHomeMenuItem;

typedef enum {
    EDGE_CURTAIN_IDLE, EDGE_CURTAIN_OPEN, EDGE_CURTAIN_CLOSED
} EdgeCurtain;

typedef enum {
    EDGE_HOME_DEFAULT_SOURCE, EDGE_HOME_LOCAL_SOURCE, EDGE_HOME_REMOTE_SOURCE
} EdgeHomeSource;

typedef struct {
    int16_t temperature_tenths;
    uint16_t light_level;
    bool temperature_valid;
    bool light_valid;
} EdgeHomeReading;

typedef struct {
    EdgeHomePage page;
    EdgeHomeMenuItem menu_item;
    EdgeCurtain curtain;
    EdgeHomeSource source;
    int16_t fan_on_tenths;
    uint16_t close_level;
    uint16_t light_level;
    bool temperature_valid;
    bool light_valid;
    bool manual;
    bool fan_on;
    bool mode_choice_manual;
} EdgeHomeView;

typedef struct {
    bool candidate;
    bool pressed;
    bool long_handled;
    uint32_t changed_ms;
    uint32_t pressed_ms;
} EdgeKeyState;

typedef struct {
    EdgeHomeView view;
    EdgeKeyState keys[EDGE_KEY_COUNT];
    EdgeCurtain curtain_target;
    uint32_t curtain_changed_ms;
    uint32_t local_ms;
    uint32_t session;
    EdgeHomeCommand last_command;
    uint8_t last_result;
    bool local_used;
} EdgeHome;

void edge_home_init(EdgeHome *home, uint8_t buttons, uint32_t now_ms);
uint8_t edge_home_joystick_keys(EdgeJoystickPosition *position, uint16_t y);
void edge_home_step(EdgeHome *home, EdgeHomeReading reading, uint8_t buttons, uint32_t now_ms);
void edge_home_sync(EdgeHome *home, uint32_t session);
uint8_t edge_home_remote(EdgeHome *home, EdgeHomeReading reading, EdgeHomeCommand command,
                         uint32_t now_ms);
int16_t edge_home_fan_off_threshold(const EdgeHomeView *view);
uint16_t edge_home_open_threshold(const EdgeHomeView *view);

#endif
