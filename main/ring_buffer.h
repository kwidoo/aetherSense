#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

/*
 * Bounded ring buffer for variable-length binary blobs.
 *
 * Supports multiple concurrent producers (Wi-Fi promiscuous callback and CSI
 * callback both call rb_push).  A portMUX_TYPE spinlock guards every access to
 * shared state, so rb_push, rb_pop, rb_used, and rb_count_drop are all safe
 * from any FreeRTOS task context on both cores.
 * portENTER_CRITICAL_SAFE / portEXIT_CRITICAL_SAFE are used because they work
 * in both task and ISR contexts.
 *
 * rb_pop is intended for use by a SINGLE consumer task only.
 */

#define RB_SLOT_SIZE   512   /* max bytes per slot (covers CSI header + data) */
#define RB_SLOT_COUNT  128   /* number of slots; tune to available heap       */

typedef struct {
    uint8_t  data[RB_SLOT_SIZE];
    uint16_t len;
} rb_slot_t;

typedef struct {
    rb_slot_t    slots[RB_SLOT_COUNT];
    portMUX_TYPE lock;               /* multi-producer spinlock               */
    volatile uint32_t head;          /* consumer reads here (single consumer) */
    volatile uint32_t tail;          /* producer writes here (lock-protected) */
    volatile uint32_t dropped;       /* overrun counter (lock-protected)      */
    volatile uint32_t high_watermark;/* max simultaneous used (lock-protected)*/
} ring_buffer_t;

void     rb_init(ring_buffer_t *rb);
bool     rb_push(ring_buffer_t *rb, const void *data, uint16_t len);  /* callback safe */
bool     rb_pop (ring_buffer_t *rb, void *out_data, uint16_t *out_len);
uint32_t rb_used(ring_buffer_t *rb);
void     rb_count_drop(ring_buffer_t *rb);  /* thread-safe drop counter increment */
