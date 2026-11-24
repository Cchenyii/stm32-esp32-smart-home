#ifndef EDGE_BOARD_H
#define EDGE_BOARD_H

#include "stm32f4xx_hal.h"
#include "edge_home.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    GPIO_TypeDef *port;
    uint16_t pin;
    uint8_t alternate;
} EdgePin;

typedef struct {
    EdgePin light, fan, servo, keys[EDGE_KEY_COUNT];
    EdgePin joystick_y;
    bool fan_active_low;
    bool keys_active_high;
    bool light_decreases_with_brightness;
} EdgeHomePins;

typedef struct {
    EdgePin dht, trig, echo, scl, sda, tx, rx;
    uint8_t oled_address; /* SSD1306 address, or 0 to probe 0x3C/0x3D. */
    EdgeHomePins home;
} EdgeBoardConfig;

typedef struct {
    int16_t temperature_tenths;
    uint16_t humidity_tenths;
    uint16_t distance_mm;
    uint16_t flags;
    uint32_t uptime_ms;
} EdgeSample;

extern const EdgeBoardConfig edge_pins;
extern volatile uint32_t edge_board_state;

void edge_board_init(void);
void edge_board_read(EdgeSample *sample);
void edge_pin_init(EdgePin pin, uint32_t mode, uint32_t pull);
bool edge_display_update(const EdgeSample *sample, bool uart_alive, const EdgeHomeView *home);
void edge_display_init(void);
void edge_uart_init(void);
bool edge_uart_ready(void);
size_t edge_uart_receive(uint8_t *data, size_t length);
bool edge_uart_send(const uint8_t *data, uint16_t length);
uint32_t edge_uart_faults(void);
void edge_home_io_init(void);
bool edge_home_light_read(uint16_t *raw);
uint8_t edge_home_buttons(void);
void edge_home_outputs(const EdgeHomeView *view);
void edge_home_outputs_off(void);

#endif
