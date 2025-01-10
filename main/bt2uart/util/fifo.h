#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t* data;
    size_t len;
    size_t cap;
} bt2uart_fifo_t;

bool bt2uart_fifo_init(bt2uart_fifo_t*, size_t initial_cap);

void bt2uart_fifo_free(bt2uart_fifo_t*);

void bt2uart_fifo_clear(bt2uart_fifo_t*);

void bt2uart_fifo_push(bt2uart_fifo_t*, uint8_t* data, size_t len);

void bt2uart_fifo_pop(bt2uart_fifo_t*, size_t num);