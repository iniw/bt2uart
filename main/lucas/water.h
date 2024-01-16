#pragma once

#include <esp_err.h>
#include <lucas/event.h>
#include <lucas/util/fifo.h>

typedef enum {
    LUCAS_WATER_EVENT_PULSE = 0,
} lucas_water_event_t;

typedef enum {
    LUCAS_WATER_CMD_ENABLE_RELAY = 0,
} lucas_water_cmd_type_t;

typedef struct {
    lucas_water_cmd_type_t type;

    union {
        struct {
            uint32_t duration;
        } enable_relay;
    };
} __attribute__((packed)) lucas_water_cmd_t;

void lucas_water_send_cmd(lucas_water_event_t*, lucas_fifo_t*);

void lucas_water_interpret_cmd(void* data);

esp_err_t lucas_water_init();
