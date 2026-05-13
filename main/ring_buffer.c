#include "ring_buffer.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

void rb_init(ring_buffer_t *rb)
{
    memset(rb, 0, sizeof(*rb));
    rb->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
}

uint32_t rb_used(ring_buffer_t *rb)
{
    portENTER_CRITICAL_SAFE(&rb->lock);
    uint32_t tail = rb->tail;
    uint32_t head = rb->head;
    portEXIT_CRITICAL_SAFE(&rb->lock);
    if (tail >= head) return tail - head;
    return RB_SLOT_COUNT - head + tail;
}

bool rb_push(ring_buffer_t *rb, const void *data, uint16_t len)
{
    if (len == 0 || len > RB_SLOT_SIZE) return false;

    portENTER_CRITICAL_SAFE(&rb->lock);

    uint32_t next_tail = (rb->tail + 1) % RB_SLOT_COUNT;
    if (next_tail == rb->head) {
        /* Buffer full – count the drop inside the lock. */
        rb->dropped++;
        portEXIT_CRITICAL_SAFE(&rb->lock);
        return false;
    }

    rb_slot_t *slot = &rb->slots[rb->tail];
    slot->len = len;
    memcpy(slot->data, data, len);

    /* Publish by advancing tail. */
    rb->tail = next_tail;

    /* Compute and update high-watermark inside the lock (avoids torn reads). */
    uint32_t head = rb->head;
    uint32_t used = (next_tail >= head) ? (next_tail - head)
                                        : (RB_SLOT_COUNT - head + next_tail);
    if (used > rb->high_watermark) rb->high_watermark = used;

    portEXIT_CRITICAL_SAFE(&rb->lock);
    return true;
}

bool rb_pop(ring_buffer_t *rb, void *out_data, uint16_t *out_len)
{
    portENTER_CRITICAL_SAFE(&rb->lock);

    uint32_t head = rb->head;
    uint32_t tail = rb->tail;
    if (head == tail) {
        portEXIT_CRITICAL_SAFE(&rb->lock);
        return false;
    }

    rb_slot_t *slot = &rb->slots[head];
    memcpy(out_data, slot->data, slot->len);
    *out_len = slot->len;

    rb->head = (head + 1) % RB_SLOT_COUNT;

    portEXIT_CRITICAL_SAFE(&rb->lock);
    return true;
}

void rb_count_drop(ring_buffer_t *rb)
{
    portENTER_CRITICAL_SAFE(&rb->lock);
    rb->dropped++;
    portEXIT_CRITICAL_SAFE(&rb->lock);
}
