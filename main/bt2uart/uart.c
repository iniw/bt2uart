#include "uart.h"
#include <bt2uart/event.h>
#include <bt2uart/shared.h>
#include <bt2uart/util/err.h>
#include <bt2uart/util/log.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <string.h>

struct uart_event_loop_ctx_t {
    QueueHandle_t event_queue;
    uint8_t* rx_buffer;
};

static void uart_event_loop(void* octx) {
    struct uart_event_loop_ctx_t* ctx = octx;

    uart_event_t event;
    while (true) {
        xQueueReceive(ctx->event_queue, &event, portMAX_DELAY);

        switch (event.type) {
        case UART_DATA:
            LOGI("UART [%zu]", event.size);
            if (uart_read_bytes(LUCAS_UART_PORT, ctx->rx_buffer, event.size, portMAX_DELAY) <= 0)
                break;

            // SAFETY: the main event loop is responsible for freeing this
            uint8_t* data = malloc(event.size);
            memcpy(data, ctx->rx_buffer, event.size);

            bt2uart_event_t bt2uart_event = {
                .type = LUCAS_EVENT_UART_RECV,
                .recv = { data, .len = event.size }
            };
            bt2uart_event_send(&bt2uart_event);
            break;
        case UART_FIFO_OVF:
            LOGE("UART_FIFO_OVF");
            uart_flush_input(LUCAS_UART_PORT);
            xQueueReset(ctx->event_queue);
            break;
        case UART_BUFFER_FULL:
            LOGE("UART_BUFFER_FULL");
            uart_flush_input(LUCAS_UART_PORT);
            xQueueReset(ctx->event_queue);
            break;
        default:
            LOGW("unhandled uart event: %d", event.type);
            break;
        }
    }
}

esp_err_t bt2uart_uart_init() {
    // NOTE: the static lifetime is very important,
    //       this object has to live as long as the task itself, which is forever
    static struct uart_event_loop_ctx_t ctx = { 0 };

    ctx.rx_buffer = malloc(LUCAS_UART_BUFFER_SIZE);
    if (ctx.rx_buffer == NULL)
        return ESP_ERR_NO_MEM;

    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT
    };
    TRY(uart_param_config(LUCAS_UART_PORT, &uart_config));
    TRY(uart_set_pin(LUCAS_UART_PORT, 17, 16, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    TRY(uart_driver_install(LUCAS_UART_PORT, LUCAS_UART_BUFFER_SIZE, LUCAS_UART_BUFFER_SIZE, 20, &ctx.event_queue, 0));

    RTOS_TRY(xTaskCreate(uart_event_loop, "UART", 4096, &ctx, 20, NULL));

    return ESP_OK;
}
