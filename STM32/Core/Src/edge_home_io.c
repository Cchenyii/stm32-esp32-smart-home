#include "edge_board.h"

enum {
    LIGHT_SAMPLES = 8,
    ADC_TIMEOUT_US = 500,
    SERVO_PERIOD_US = 20000,
    SERVO_OPEN_US = 1100,
    SERVO_CLOSED_US = 1900
};

static bool outputs_ready;
static EdgeJoystickPosition joystick_position;

static void set_fan(bool on)
{
    const EdgePin fan = edge_pins.home.fan;
    GPIO_PinState level = (on != edge_pins.home.fan_active_low) ? GPIO_PIN_SET : GPIO_PIN_RESET;
    HAL_GPIO_WritePin(fan.port, fan.pin, level);
}

void edge_home_io_init(void)
{
    const EdgeHomePins *pins = &edge_pins.home;
    set_fan(false);
    edge_pin_init(pins->fan, GPIO_MODE_OUTPUT_PP, GPIO_NOPULL);

    for (unsigned i = 0; i < EDGE_KEY_COUNT; ++i) {
        if (pins->keys[i].port != NULL) {
            edge_pin_init(pins->keys[i], GPIO_MODE_INPUT,
                          pins->keys_active_high ? GPIO_PULLDOWN : GPIO_PULLUP);
        }
    }

    edge_pin_init(pins->light, GPIO_MODE_ANALOG, GPIO_NOPULL);
    __HAL_RCC_ADC1_CLK_ENABLE();
    /* PA1 and PA5 map to ADC1_IN1 and ADC1_IN5, respectively. */
    unsigned light_channel = (unsigned)__builtin_ctz((unsigned)pins->light.pin);
    /* PCLK2 / 4, 12-bit, one software-triggered conversion. */
    MODIFY_REG(ADC->CCR, ADC_CCR_ADCPRE, ADC_CCR_ADCPRE_0);
    ADC1->CR1 = 0;
    ADC1->SQR1 = 0;
    ADC1->SQR3 = light_channel;
    ADC1->SMPR2 = 4U << (3U * light_channel); /* 84 ADC clocks. */
    ADC1->SR = 0;
    ADC1->CR2 = ADC_CR2_ADON;
    if (pins->joystick_y.port != NULL) {
        edge_pin_init(pins->joystick_y, GPIO_MODE_ANALOG, GPIO_NOPULL);
        __HAL_RCC_ADC2_CLK_ENABLE();
        /* PC2 = ADC2_IN12. The process and sensor tasks use separate converters. */
        ADC2->CR1 = 0;
        ADC2->SQR1 = 0;
        ADC2->SQR3 = 12U;
        ADC2->SMPR1 = 4U << ADC_SMPR1_SMP12_Pos;
        ADC2->SR = 0;
        ADC2->CR2 = ADC_CR2_ADON;
        joystick_position = EDGE_JOYSTICK_WAIT_CENTER;
    }

    __HAL_RCC_TIM4_CLK_ENABLE();
    uint32_t timer_clock = HAL_RCC_GetPCLK1Freq();
    if ((RCC->CFGR & RCC_CFGR_PPRE1_2) != 0U) {
        timer_clock *= 2U;
    }
    /* CH1 PWM mode 1. The initial compare of zero holds the output low. */
    TIM4->PSC = timer_clock / 1000000U - 1U;
    TIM4->ARR = SERVO_PERIOD_US - 1U;
    TIM4->CCR1 = 0U;
    TIM4->CCMR1 = TIM_CCMR1_OC1M_1 | TIM_CCMR1_OC1M_2 | TIM_CCMR1_OC1PE;
    TIM4->CR1 = TIM_CR1_ARPE;
    TIM4->EGR = TIM_EGR_UG;
    TIM4->CCER = TIM_CCER_CC1E;
    TIM4->CR1 |= TIM_CR1_CEN;
    edge_pin_init(pins->servo, GPIO_MODE_AF_PP, GPIO_PULLDOWN);
    outputs_ready = true;
}

static bool read_adc(ADC_TypeDef *adc, uint16_t *raw)
{
    const uint32_t timeout_cycles = ADC_TIMEOUT_US * (SystemCoreClock / 1000000U);
    adc->CR2 |= ADC_CR2_SWSTART;
    uint32_t start = DWT->CYCCNT;
    while ((adc->SR & ADC_SR_EOC) == 0U) {
        if (DWT->CYCCNT - start >= timeout_cycles) {
            /* Discard the incomplete conversion before restarting the ADC. */
            adc->CR2 = 0;
            adc->SR = 0;
            adc->CR2 = ADC_CR2_ADON;
            return false;
        }
    }
    *raw = (uint16_t)adc->DR;
    return true;
}

bool edge_home_light_read(uint16_t *raw)
{
    uint32_t sum = 0;
    for (unsigned i = 0; i < LIGHT_SAMPLES; ++i) {
        uint16_t sample;
        if (!read_adc(ADC1, &sample)) {
            return false;
        }
        sum += sample;
    }
    *raw = (uint16_t)((sum + LIGHT_SAMPLES / 2U) / LIGHT_SAMPLES);
    return true;
}

uint8_t edge_home_buttons(void)
{
    const EdgeHomePins *pins = &edge_pins.home;
    const GPIO_PinState pressed = pins->keys_active_high ? GPIO_PIN_SET : GPIO_PIN_RESET;
    uint8_t buttons = 0;
    for (unsigned i = 0; i < EDGE_KEY_COUNT; ++i) {
        EdgePin key = pins->keys[i];
        if (key.port != NULL && HAL_GPIO_ReadPin(key.port, key.pin) == pressed) {
            buttons |= (uint8_t)(1U << i);
        }
    }
    if (pins->joystick_y.port != NULL) {
        uint16_t y;
        if (read_adc(ADC2, &y)) {
            buttons |= edge_home_joystick_keys(&joystick_position, y);
        } else {
            joystick_position = EDGE_JOYSTICK_WAIT_CENTER;
        }
    }
    return buttons;
}

void edge_home_outputs(const EdgeHomeView *view)
{
    set_fan(view->fan_on);
    uint32_t pulse = 0U;
    if (view->curtain == EDGE_CURTAIN_OPEN) {
        pulse = SERVO_OPEN_US;
    } else if (view->curtain == EDGE_CURTAIN_CLOSED) {
        pulse = SERVO_CLOSED_US;
    }
    TIM4->CCR1 = pulse;
}

void edge_home_outputs_off(void)
{
    if (outputs_ready) {
        set_fan(false);
        TIM4->CCR1 = 0;
        TIM4->EGR = TIM_EGR_UG;
    }
}
