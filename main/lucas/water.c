#include "water.h"
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
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
    uint16_t duration = *(uint16_t*)octx;

    enable_relay();

    LOGI("relay task sleeping for %hu seconds", duration);
    vTaskDelay(pdMS_TO_TICKS(duration * 1000));
    LOGI("sleep over");

    disable_relay();

    vTaskDelete(NULL);
}

static uint16_t g_max_pulse_count = 0;
static uint16_t g_pulse_count = 0;

static void consume_data_stream(void** stream, void* out, size_t len) {
    memcpy(out, *stream, len);
    *stream += len;
}

void lucas_water_interpret_cmd(void* data) {
    lucas_water_event_type_t type;
    consume_data_stream(&data, &type, sizeof(lucas_water_event_type_t));

    LOGI("interpreting cmd - type = %d", type);
    switch (type) {
    case LUCAS_WATER_CMD_ENABLE_RELAY_TIMED: {
        static uint16_t relay_task_duration = 0;
        consume_data_stream(&data, &relay_task_duration, sizeof(relay_task_duration));
        xTaskCreate(relay_task, "relay", 2048, &relay_task_duration, 15, NULL);
    } break;
    case LUCAS_WATER_CMD_ENABLE_RELAY_PULSES:
        portDISABLE_INTERRUPTS();
        enable_relay();
        g_pulse_count = 0;
        consume_data_stream(&data, &g_max_pulse_count, sizeof(g_max_pulse_count));
        portENABLE_INTERRUPTS();
        break;
    }
}

static void pulse_counter_isr() {
    lucas_event_t event = {
        .type = LUCAS_EVENT_WATER,
        .water = { .type = LUCAS_WATER_EVENT_PULSE }
    };

    if (g_max_pulse_count) {
        g_pulse_count++;
        if (g_pulse_count == g_max_pulse_count) {
            g_pulse_count = g_max_pulse_count = 0;
            // override the type
            event.water.type = LUCAS_WATER_EVENT_MAX_PULSES_REACHED;
        }
    }

    if (lucas_event_send_from_isr(&event))
        portYIELD_FROM_ISR();
}

void lucas_water_handle_event_send(lucas_water_event_type_t event_discriminator, lucas_fifo_t* buffer) {
    if (event_discriminator == LUCAS_WATER_EVENT_MAX_PULSES_REACHED)
        disable_relay();

    lucas_fifo_push(buffer, &event_discriminator, sizeof(lucas_water_event_type_t));
    lucas_fifo_push(buffer, "\n", 1);
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
