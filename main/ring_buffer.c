#include "ring_buffer.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

void rb_init(ring_buffer_t *rb)
{
    memset(rb, 0, sizeof(*rb));
}

uint32_t rb_used(const ring_buffer_t *rb)
{
    uint32_t tail = rb->tail;
    uint32_t head = rb->head;
    if (tail >= head) return tail - head;
    return RB_SLOT_COUNT - head + tail;
}

bool rb_push(ring_buffer_t *rb, const void *data, uint16_t len)
{
    if (len == 0 || len > RB_SLOT_SIZE) return false;

    /* Check capacity without a lock first (optimistic path). */
    uint32_t next_tail = (rb->tail + 1) % RB_SLOT_COUNT;
    if (next_tail == rb->head) {
        /* Buffer full – drop and count. */
        rb->dropped++;
        return false;
    }

    rb_slot_t *slot = &rb->slots[rb->tail];
    memcpy(slot->data, data, len);
    slot->len = len;

    /* Publish by advancing tail.  On Xtensa a 32-bit store is atomic, so
     * a full critical section is not strictly needed, but we use a compiler
     * barrier to prevent reordering. */
    __atomic_store_n(&rb->tail, next_tail, __ATOMIC_RELEASE);

    uint32_t used = rb_used(rb);
    if (used > rb->high_watermark) rb->high_watermark = used;

    return true;
}

bool rb_pop(ring_buffer_t *rb, void *out_data, uint16_t *out_len)
{
    uint32_t head = rb->head;
    uint32_t tail = __atomic_load_n(&rb->tail, __ATOMIC_ACQUIRE);
    if (head == tail) return false;

    rb_slot_t *slot = &rb->slots[head];
    memcpy(out_data, slot->data, slot->len);
    *out_len = slot->len;

    rb->head = (head + 1) % RB_SLOT_COUNT;
    return true;
}
