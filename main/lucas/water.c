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

static void relay_task(void* octx) {
    uint32_t duration = *(uint32_t*)octx;

    enable_relay();

    LOGI("relay task sleeping for %lu seconds", duration);
    vTaskDelay(pdMS_TO_TICKS(duration * 1000));
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
        static uint32_t relay_task_duration = 0;
        relay_task_duration = *(uint32_t*)data;
        xTaskCreate(relay_task, "relay", 2048, &relay_task_duration, 5, NULL);
    } break;
    }
}

static void pulse_counter_isr() {
    lucas_event_t event = {
        .type = LUCAS_EVENT_WATER,
        .water = { .type = LUCAS_WATER_EVENT_PULSE }
    };

    if (lucas_event_send_from_isr(&event))
        portYIELD_FROM_ISR();
}

esp_err_t lucas_water_init() {
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
    TRY(gpio_isr_handler_add(PULSE_COUNTER_PIN, pulse_counter_isr, NULL));

    return ESP_OK;
}
