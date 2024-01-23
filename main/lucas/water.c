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

#define SEND_EVENT(x)                  \
    {                                  \
        lucas_event_t event = {        \
            .type = LUCAS_EVENT_WATER, \
            .water = { .type = x }     \
        };                             \
        lucas_event_send(&event);      \
    }

#define SEND_EVENT_FROM_ISR(x)             \
    ({                                     \
        lucas_event_t event = {            \
            .type = LUCAS_EVENT_WATER,     \
            .water = { .type = x }         \
        };                                 \
        lucas_event_send_from_isr(&event); \
    })

static esp_err_t control_relay(bool state) {
    return gpio_set_level(RELAY_PIN, !state);
}

static esp_err_t disable_relay() {
    return control_relay(false);
}

static esp_err_t enable_relay() {
    return control_relay(true);
}

static TaskHandle_t s_relay_task_handle = NULL;

static void relay_task(void* octx) {
    uint16_t duration = *(uint16_t*)octx;

    SEND_EVENT(LUCAS_WATER_EVENT_RELAY_TURNED_ON);

    LOGI("relay task sleeping for %hu seconds", duration);
    vTaskDelay(pdMS_TO_TICKS(duration * 1000));
    LOGI("sleep over");

    SEND_EVENT(LUCAS_WATER_EVENT_RELAY_TURNED_OFF);

    s_relay_task_handle = NULL;
    vTaskDelete(NULL);
}

static uint16_t s_max_pulse_count = 0;
static uint16_t s_pulse_count = 0;

static void consume_data_stream(void** stream, void* out, size_t len) {
    memcpy(out, *stream, len);
    *stream += len;
}

void lucas_water_interpret_cmd(void* data) {
    lucas_water_event_type_t type;
    consume_data_stream(&data, &type, sizeof(lucas_water_event_type_t));

    LOGI("interpreting cmd - type = %d", type);
    switch (type) {
    case LUCAS_WATER_CMD_ENABLE_RELAY_WITH_DURATION: {
        static uint16_t relay_task_duration = 0;
        consume_data_stream(&data, &relay_task_duration, sizeof(relay_task_duration));
        xTaskCreate(relay_task, "relay", 2048, &relay_task_duration, 15, &s_relay_task_handle);
    } break;
    case LUCAS_WATER_CMD_ENABLE_RELAY_WITH_MAX_PULSES:
        portDISABLE_INTERRUPTS();
        s_pulse_count = 0;
        consume_data_stream(&data, &s_max_pulse_count, sizeof(s_max_pulse_count));
        portENABLE_INTERRUPTS();

        SEND_EVENT(LUCAS_WATER_EVENT_RELAY_TURNED_ON);
        break;
    case LUCAS_WATER_CMD_CANCEL_COMMAND:
        disable_relay();

        portDISABLE_INTERRUPTS();
        s_pulse_count = 0;
        s_max_pulse_count = 0;
        portENABLE_INTERRUPTS();

        if (s_relay_task_handle) {
            vTaskDelete(s_relay_task_handle);
            s_relay_task_handle = NULL;
        }
        break;
    }
}

static void pulse_counter_isr() {
    if (s_max_pulse_count) {
        s_pulse_count++;
        if (s_pulse_count == s_max_pulse_count) {
            s_pulse_count = s_max_pulse_count = 0;
            if (SEND_EVENT_FROM_ISR(LUCAS_WATER_EVENT_MAX_PULSES_REACHED))
                portYIELD_FROM_ISR();

            return;
        }
    }

    if (SEND_EVENT_FROM_ISR(LUCAS_WATER_EVENT_PULSE))
        portYIELD_FROM_ISR();
}

void lucas_water_handle_event_send(lucas_water_event_type_t event_discriminator, lucas_fifo_t* buffer) {
    switch (event_discriminator) {
    case LUCAS_WATER_EVENT_RELAY_TURNED_ON:
        enable_relay();
        break;
    case LUCAS_WATER_EVENT_RELAY_TURNED_OFF:
        disable_relay();
        break;
    case LUCAS_WATER_EVENT_MAX_PULSES_REACHED:
        disable_relay();

        // this event becomes two different events, pulse and relay turned off
        event_discriminator = LUCAS_WATER_EVENT_PULSE;
        lucas_fifo_push(buffer, &event_discriminator, sizeof(lucas_water_event_type_t));
        lucas_fifo_push(buffer, "\n", 1);

        // this will be pushed on the fallthrough
        event_discriminator = LUCAS_WATER_EVENT_RELAY_TURNED_OFF;
        break;
    }

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
