#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * Bounded ring buffer for variable-length binary blobs.
 *
 * Each slot stores a fixed-capacity byte array.  The buffer is ISR/callback
 * safe on the producer side: rb_push copies data and updates the write index
 * atomically using a FreeRTOS spinlock so it can be called from a Wi-Fi
 * callback (which runs in a high-priority system task context, not an ISR, but
 * must still return quickly).
 *
 * rb_pop is intended for use only from a single consumer task.
 */

#define RB_SLOT_SIZE   512   /* max bytes per slot (covers CSI header + data) */
#define RB_SLOT_COUNT  128   /* number of slots; tune to available heap       */

typedef struct {
    uint8_t  data[RB_SLOT_SIZE];
    uint16_t len;
} rb_slot_t;

typedef struct {
    rb_slot_t slots[RB_SLOT_COUNT];
    volatile uint32_t head;          /* consumer reads here   */
    volatile uint32_t tail;          /* producer writes here  */
    volatile uint32_t dropped;       /* overrun counter       */
    volatile uint32_t high_watermark;/* max simultaneous used */
} ring_buffer_t;

void     rb_init(ring_buffer_t *rb);
bool     rb_push(ring_buffer_t *rb, const void *data, uint16_t len);  /* callback safe */
bool     rb_pop (ring_buffer_t *rb, void *out_data, uint16_t *out_len);
uint32_t rb_used(const ring_buffer_t *rb);
