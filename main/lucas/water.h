#pragma once

#include <esp_err.h>
#include <lucas/util/fifo.h>

typedef enum {
    LUCAS_WATER_EVENT_RELAY_TURNED_ON = 0,
    LUCAS_WATER_EVENT_RELAY_TURNED_OFF,
    LUCAS_WATER_EVENT_PULSE,
    // sent as PULSE + RELAY_TURNED_OFF
    LUCAS_WATER_EVENT_MAX_PULSES_REACHED,
} lucas_water_event_t;

typedef uint8_t lucas_water_event_type_t;

typedef enum {
    LUCAS_WATER_CMD_ENABLE_RELAY_WITH_DURATION = 0,
    LUCAS_WATER_CMD_ENABLE_RELAY_WITH_MAX_PULSES,
    LUCAS_WATER_CMD_CANCEL_COMMAND,
} lucas_water_cmd_type_t;

typedef struct {
    lucas_water_cmd_type_t type;

    union {
        struct {
            uint32_t duration;
        } enable_relay;
    };
} __attribute__((packed)) lucas_water_cmd_t;

void lucas_water_handle_event_send(lucas_water_event_type_t, lucas_fifo_t*);

void lucas_water_interpret_cmd(void* data);

esp_err_t lucas_water_init();
