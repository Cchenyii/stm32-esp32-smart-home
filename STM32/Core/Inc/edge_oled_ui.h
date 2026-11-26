#ifndef EDGE_OLED_UI_H
#define EDGE_OLED_UI_H

#include "edge_home.h"

enum { EDGE_OLED_WIDTH = 128, EDGE_OLED_HEIGHT = 64, EDGE_OLED_PAGES = 8 };
typedef uint8_t EdgeOledFrame[EDGE_OLED_PAGES][EDGE_OLED_WIDTH];

typedef enum {
    EDGE_OLED_BRIGHT, EDGE_OLED_DIM, EDGE_OLED_OFF, EDGE_OLED_UNKNOWN
} EdgeOledPower;

typedef struct {
    uint32_t last_input_ms;
    EdgeOledPower power;
    bool wake_hold;
} EdgeOledIdle;

typedef struct {
    int16_t temperature_tenths;
    uint16_t humidity_tenths;
    uint32_t sample_age_s;
    bool link_configured;
    bool link_alive;
} EdgeOledReadings;

void edge_oled_text(EdgeOledFrame frame, unsigned x, unsigned y, unsigned scale, const char *text);
void edge_oled_home(EdgeOledFrame frame, const EdgeHomeView *home, EdgeOledReadings readings);
uint8_t edge_oled_idle_step(EdgeOledIdle *idle, uint8_t buttons, uint32_t now_ms);

#endif
