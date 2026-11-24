#include "edge_board.h"
#include "edge_wire.h"
#include "FreeRTOS.h"
#include "stream_buffer.h"

#define DMA_BYTES 256U
#define FIFO_BYTES 1024U

static UART_HandleTypeDef uart;
static DMA_HandleTypeDef rx_dma;
static StreamBufferHandle_t fifo;
static uint8_t dma_data[DMA_BYTES];
static uint16_t last_offset;
static volatile uint32_t faults;
static volatile bool restart_rx = true;
static bool ready;

void edge_uart_init(void)
{
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    edge_pin_init(edge_pins.tx, GPIO_MODE_AF_PP, GPIO_NOPULL);
    edge_pin_init(edge_pins.rx, GPIO_MODE_AF_PP, GPIO_PULLUP);
    rx_dma.Instance = DMA1_Stream5;
    rx_dma.Init = (DMA_InitTypeDef){
        .Channel = DMA_CHANNEL_4, .Direction = DMA_PERIPH_TO_MEMORY,
        .PeriphInc = DMA_PINC_DISABLE, .MemInc = DMA_MINC_ENABLE,
        .PeriphDataAlignment = DMA_PDATAALIGN_BYTE, .MemDataAlignment = DMA_MDATAALIGN_BYTE,
        .Mode = DMA_CIRCULAR, .Priority = DMA_PRIORITY_HIGH, .FIFOMode = DMA_FIFOMODE_DISABLE
    };
    if (HAL_DMA_Init(&rx_dma) != HAL_OK) {
        return;
    }
    uart.Instance = USART2;
    uart.Init = (UART_InitTypeDef){
        .BaudRate = EDGE_BAUD, .WordLength = UART_WORDLENGTH_8B,
        .StopBits = UART_STOPBITS_1, .Parity = UART_PARITY_NONE,
        .Mode = UART_MODE_TX_RX, .HwFlowCtl = UART_HWCONTROL_NONE,
        .OverSampling = UART_OVERSAMPLING_16
    };
    __HAL_LINKDMA(&uart, hdmarx, rx_dma);
    if (HAL_UART_Init(&uart) != HAL_OK) {
        return;
    }
    fifo = xStreamBufferCreate(FIFO_BYTES, 1);
    if (fifo == NULL) {
        return;
    }
    /* Same preemption priority serializes the two FIFO producer IRQs. */
    HAL_NVIC_SetPriority(DMA1_Stream5_IRQn, 6U, 0U);
    HAL_NVIC_SetPriority(USART2_IRQn, 6U, 0U);
    HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
    ready = true;
}

bool edge_uart_ready(void) { return ready; }
uint32_t edge_uart_faults(void) { return faults; }

void USART2_IRQHandler(void) { HAL_UART_IRQHandler(&uart); }
void DMA1_Stream5_IRQHandler(void) { HAL_DMA_IRQHandler(&rx_dma); }

static void fifo_push(uint16_t first, uint16_t end, BaseType_t *wake)
{
    size_t count = end - first;
    if (count != 0U &&
        xStreamBufferSendFromISR(fifo, dma_data + first, count, wake) != count) {
        ++faults;
        restart_rx = true;
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *handle, uint16_t size)
{
    (void)size;
    if (handle != &uart || restart_rx) {
        return;
    }
    /* Read DMA's current offset so a late HT interrupt cannot replay data. */
    uint16_t offset = (uint16_t)((DMA_BYTES - __HAL_DMA_GET_COUNTER(&rx_dma)) % DMA_BYTES);
    BaseType_t wake = pdFALSE;
    if (offset < last_offset) {
        fifo_push(last_offset, DMA_BYTES, &wake);
        fifo_push(0, offset, &wake);
    } else {
        fifo_push(last_offset, offset, &wake);
    }
    last_offset = offset;
    portYIELD_FROM_ISR(wake);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *handle)
{
    if (handle == &uart) {
        ++faults;
        restart_rx = true;
    }
}

size_t edge_uart_receive(uint8_t *data, size_t length)
{
    if (!ready) {
        return 0;
    }
    if (restart_rx) {
        static uint32_t next_retry;
        uint32_t now = HAL_GetTick();
        if (next_retry != 0U && (int32_t)(now - next_retry) < 0) {
            return 0;
        }
        HAL_NVIC_DisableIRQ(DMA1_Stream5_IRQn);
        HAL_NVIC_DisableIRQ(USART2_IRQn);
        (void)HAL_UART_AbortReceive(&uart);
        (void)xStreamBufferReset(fifo);
        last_offset = 0;
        HAL_NVIC_ClearPendingIRQ(DMA1_Stream5_IRQn);
        HAL_NVIC_ClearPendingIRQ(USART2_IRQn);
        restart_rx = HAL_UARTEx_ReceiveToIdle_DMA(&uart, dma_data, DMA_BYTES) != HAL_OK;
        if (restart_rx) {
            ++faults;
            next_retry = now + 100U;
        } else {
            next_retry = 0;
        }
        HAL_NVIC_EnableIRQ(DMA1_Stream5_IRQn);
        HAL_NVIC_EnableIRQ(USART2_IRQn);
        return 0;
    }
    return xStreamBufferReceive(fifo, data, length, 0);
}

bool edge_uart_send(const uint8_t *data, uint16_t length)
{
    return ready && HAL_UART_Transmit(&uart, data, length, 20U) == HAL_OK;
}
