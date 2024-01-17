#include "event.h"
#include <esp_spp_api.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <lucas/water.h>
#include <lucas/shared.h>
#include <lucas/util/log.h>
#include <lucas/util/err.h>
#include <lucas/util/fifo.h>

struct event_loop_ctx_t {
    lucas_fifo_t spp_fifo_buffer;
};

static QueueHandle_t s_event_queue = NULL;

static void write_fifo_to_spp(lucas_fifo_t* fifo, uint32_t spp_handle) {
    // SAFETY: this function deep copies the buffer internally,
    //         there's no need to worry about modifications to the fifo being made before the transfer is finished
    //         https://github.com/espressif/esp-idf/blob/release/v5.2/components/bt/host/bluedroid/btc/profile/std/spp/btc_spp.c#L1442C13-L1442C33
    //         see the `btc_spp_arg_deep_copy` param
    esp_spp_write(spp_handle, fifo->len, fifo->data);
}

static void event_loop(void* octx) {
    struct event_loop_ctx_t* ctx = octx;

    bool spp_congested = false;
    lucas_event_t event;
    while (true) {
        xQueueReceive(s_event_queue, &event, portMAX_DELAY);

        switch (event.type) {
        case LUCAS_EVENT_SPP_SEND: {
            assert(event.send.data && event.send.len);

            LOGI("sending spp data [%zu bytes - %zu total]", event.send.len, ctx->spp_fifo_buffer.len);

            // if there's no data currently buffered begin writing straight away
            // NOTE: this has to be evaluated before `lucas_fifo_push` so that the len is not affected by the push.
            bool send_straight_away = ctx->spp_fifo_buffer.len == 0 && !spp_congested;
            lucas_fifo_push(&ctx->spp_fifo_buffer, event.send.data, event.send.len);
            if (send_straight_away)
                write_fifo_to_spp(&ctx->spp_fifo_buffer, g_shared_ctx.spp_handle);

            free(event.send.data);
        } break;
        case LUCAS_EVENT_WATER: {
            LOGI("water event [%hu]", event.water.type);

            bool send_straight_away = ctx->spp_fifo_buffer.len == 0 && !spp_congested;
            lucas_water_handle_event_send(event.water.type, &ctx->spp_fifo_buffer);
            if (send_straight_away)
                write_fifo_to_spp(&ctx->spp_fifo_buffer, g_shared_ctx.spp_handle);
        } break;
        case LUCAS_EVENT_SPP_RECV:
            assert(event.recv.data && event.recv.len);
            ESP_LOG_BUFFER_HEX("cmd", event.recv.data, event.recv.len);
            // maybe need to buffer this for newlines
            lucas_water_interpret_cmd(event.recv.data);
            free(event.recv.data);
            break;
        case LUCAS_EVENT_SPP_WRITE_SUCCEEDED:
            assert(ctx->spp_fifo_buffer.len &&
                   event.write_succeeded.num_bytes_written <= ctx->spp_fifo_buffer.len &&
                   !spp_congested);

            LOGI("sucessful spp write [%zu bytes - %zu left]", event.write_succeeded.num_bytes_written, ctx->spp_fifo_buffer.len - event.write_succeeded.num_bytes_written);

            // pop the bytes that were written
            lucas_fifo_pop(&ctx->spp_fifo_buffer, event.write_succeeded.num_bytes_written);

            spp_congested = event.write_succeeded.congested;
            if (!spp_congested && ctx->spp_fifo_buffer.len) {
                LOGI("continuing spp write [%zu bytes]", ctx->spp_fifo_buffer.len);
                write_fifo_to_spp(&ctx->spp_fifo_buffer, g_shared_ctx.spp_handle);
            }

            break;
        case LUCAS_EVENT_SPP_WRITE_AGAIN:
            assert(ctx->spp_fifo_buffer.len);

            // receiving this event definitely means that there's no congestion,
            // either because a congestion has ended (see `ESP_SPP_WRITE_EVT` handling),
            // or because the last write failed but *not* because of congestion.
            spp_congested = false;

            LOGW("retrying to write spp data [%zu bytes]", ctx->spp_fifo_buffer.len);
            write_fifo_to_spp(&ctx->spp_fifo_buffer, g_shared_ctx.spp_handle);

            break;
        case LUCAS_EVENT_SPP_CLEAR_BUFFER:
            LOGW("cleared spp buffer [%zu bytes]", ctx->spp_fifo_buffer.len);
            lucas_fifo_clear(&ctx->spp_fifo_buffer);

            break;
        }
    }
}

void lucas_event_send(lucas_event_t* event) {
    assert(xQueueSend(s_event_queue, event, portMAX_DELAY) == pdPASS);
}

bool lucas_event_send_from_isr(lucas_event_t* event) {
    BaseType_t higher_prio_task_awoken;
    assert(xQueueSendFromISR(s_event_queue, event, &higher_prio_task_awoken) == pdPASS);
    return higher_prio_task_awoken;
}

esp_err_t lucas_event_loop_init() {
    // NOTE: the static lifetime is very important,
    //       this object has to live as long as the task itself, which is forever
    static struct event_loop_ctx_t ctx = { 0 };

    if (!lucas_fifo_init(&ctx.spp_fifo_buffer, 128))
        return ESP_ERR_NO_MEM;

    s_event_queue = xQueueCreate(120, sizeof(lucas_event_t));
    if (s_event_queue == NULL)
        return ESP_ERR_NO_MEM;

    RTOS_TRY(xTaskCreatePinnedToCore(event_loop, "MAIN", 4096, &ctx, 20, NULL, 0));

    return ESP_OK;
}
