#include "water.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <driver/gpio.h>
#include <lucas/util/err.h>
#include <lucas/util/log.h>
#include <lucas/event.h>

#define RELAY_PIN GPIO_NUM_17
#define PULSE_COUNTER_PIN GPIO_NUM_16

static esp_err_t control_relay(bool state) {
    return gpio_set_level(RELAY_PIN, !state);
}

static esp_err_t disable_relay() {
    return control_relay(false);
}

static esp_err_t enable_relay() {
    return control_relay(true);
}

typedef struct {
    uint32_t duration;
} relay_ctx_t;

static void relay_task(void* octx) {
    relay_ctx_t* ctx = octx;

    enable_relay();

    LOGI("relay task sleeping for %lu seconds", ctx->duration);

    vTaskDelay(pdMS_TO_TICKS(ctx->duration * 1000));
    LOGI("sleep over");

    disable_relay();

    vTaskDelete(NULL);
}

void lucas_water_interpret_cmd(void* data) {
    lucas_water_cmd_type_t type = *(uint8_t*)data;
    // only 1 byte for the discrimator even thhough the enum is 4 bytes
    data += sizeof(uint8_t);

    LOGI("interpreting cmd - type = %d", type);
    switch (type) {
    case LUCAS_WATER_CMD_ENABLE_RELAY: {
        uint32_t duration = *(uint32_t*)data;
        LOGI("duration = %lu", duration);

        static relay_ctx_t relay_ctx = {};
        relay_ctx.duration = duration;
        xTaskCreate(relay_task, "relay", 2048, &relay_ctx, 5, NULL);
    } break;
    }
}

typedef struct {
    QueueHandle_t queue;
} pulse_counter_ctx_t;

static void pulse_counter_isr(void* octx) {
    pulse_counter_ctx_t* ctx = octx;

    lucas_water_event_t event = LUCAS_WATER_EVENT_PULSE;
    BaseType_t higher_prio_task_awoken = false;
    xQueueSendFromISR(ctx->queue, &event, &higher_prio_task_awoken);

    if (higher_prio_task_awoken)
        portYIELD_FROM_ISR();
}

typedef struct {
    QueueHandle_t queue;
} event_loop_ctx_t;

static void event_loop(void* octx) {
    event_loop_ctx_t* ctx = octx;

    lucas_water_event_t event;
    while (true) {
        xQueueReceive(ctx->queue, &event, portMAX_DELAY);

        // one byte for the event discriminator and one for the newline character
        const uint8_t byte_event = (uint8_t)event;
        const size_t event_size = sizeof(uint8_t) + sizeof(char);
        struct lucas_event_data_t send = {
            .data = malloc(event_size),
            .len = event_size,
        };

        memcpy(send.data, &byte_event, sizeof(byte_event));
        send.data[event_size - 1] = '\n';

        lucas_event_t send_event = {
            .type = LUCAS_EVENT_SPP_SEND,
            .send = send,
        };
        lucas_event_send(&send_event);
    }
}

esp_err_t lucas_water_init() {
    static pulse_counter_ctx_t pulse_counter_ctx = {
        .queue = NULL,
    };

    static event_loop_ctx_t event_loop_ctx = {
        .queue = NULL,
    };

    pulse_counter_ctx.queue = event_loop_ctx.queue = xQueueCreate(100, sizeof(lucas_water_event_t));
    if (pulse_counter_ctx.queue == NULL)
        return ESP_ERR_NO_MEM;

    gpio_config_t relay_cfg = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << RELAY_PIN,
    };

    TRY(gpio_config(&relay_cfg));
    TRY(disable_relay());

    gpio_config_t pulse_counter_cfg = {
        .mode = GPIO_MODE_INPUT,
        .intr_type = GPIO_INTR_POSEDGE,
        .pin_bit_mask = 1ULL << PULSE_COUNTER_PIN,
    };
    TRY(gpio_config(&pulse_counter_cfg));
    TRY(gpio_install_isr_service(ESP_INTR_FLAG_EDGE));
    TRY(gpio_isr_handler_add(PULSE_COUNTER_PIN, pulse_counter_isr, &pulse_counter_ctx));

    RTOS_TRY(xTaskCreate(event_loop, "EVENT", 4096, &event_loop_ctx, 20, NULL));

    return ESP_OK;
}
