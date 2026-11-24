#ifndef EDGE_BOARD_HOME_H
#define EDGE_BOARD_HOME_H

#ifdef EDGE_HOME_UART_RX_PD6
#define EDGE_HOME_UART_RX {GPIOD, GPIO_PIN_6, GPIO_AF7_USART2}
#else
#define EDGE_HOME_UART_RX {GPIOA, GPIO_PIN_3, GPIO_AF7_USART2}
#endif

#ifdef EDGE_HOME_JOYSTICK
#define EDGE_HOME_NAV_INPUTS \
    .keys = {[EDGE_KEY_MODE] = {GPIOC, GPIO_PIN_1, 0}}, \
    .joystick_y = {GPIOC, GPIO_PIN_2, 0},
#else
#define EDGE_HOME_NAV_INPUTS \
    .keys = { \
        [EDGE_KEY_MODE] = {GPIOC, GPIO_PIN_1, 0}, \
        [EDGE_KEY_UP] = {GPIOC, GPIO_PIN_2, 0}, \
        [EDGE_KEY_DOWN] = {GPIOC, GPIO_PIN_3, 0}, \
    },
#endif

/* STM32F407VET6 smart-home wiring. */
#define EDGE_BOARD_CONFIG { \
    .dht = {GPIOC, GPIO_PIN_0, 0}, \
    .scl = {GPIOB, GPIO_PIN_8, GPIO_AF4_I2C1}, \
    .sda = {GPIOB, GPIO_PIN_9, GPIO_AF4_I2C1}, \
    .tx = {GPIOA, GPIO_PIN_2, GPIO_AF7_USART2}, \
    .rx = EDGE_HOME_UART_RX, \
    .home = { \
        .light = {GPIOA, GPIO_PIN_5, 0}, /* PA1 shares the board's user LED. */ \
        .fan = {GPIOC, GPIO_PIN_4, 0}, \
        .fan_active_low = true, /* JY-25-003 */ \
        .servo = {GPIOB, GPIO_PIN_6, GPIO_AF2_TIM4}, \
        EDGE_HOME_NAV_INPUTS \
    }, \
}

#endif
