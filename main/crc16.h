#pragma once
#include <stdint.h>
#include <stddef.h>

/*
 * CRC-16/CCITT-FALSE
 *   Polynomial : 0x1021
 *   Initial    : 0xFFFF
 *   Input/output reflect: false
 *
 * Used by all record types to cover the record bytes excluding the final
 * two-byte crc16 field itself.
 */
uint16_t crc16_ccitt(const uint8_t *data, size_t len);
