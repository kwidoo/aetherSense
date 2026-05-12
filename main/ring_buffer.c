#include "ring_buffer.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

void rb_init(ring_buffer_t *rb)
{
    memset(rb, 0, sizeof(*rb));
    rb->lock = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
}

uint32_t rb_used(const ring_buffer_t *rb)
{
    /* Snapshot under the lock so callers outside rb_push see a consistent view. */
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    uint32_t head = __atomic_load_n(&rb->head, __ATOMIC_ACQUIRE);
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
    uint32_t head = rb->head;
    /* Acquire barrier ensures we see the slot data written before tail advanced. */
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    if (head == tail) return false;

    rb_slot_t *slot = &rb->slots[head];
    memcpy(out_data, slot->data, slot->len);
    *out_len = slot->len;

    /* Publish consumed slot by advancing head (only this task writes head). */
    __atomic_store_n(&rb->head, (head + 1) % RB_SLOT_COUNT, __ATOMIC_RELEASE);
    return true;
}
