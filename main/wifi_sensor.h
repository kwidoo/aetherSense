#pragma once

/*
 * wifi_sensor – initialises Wi-Fi promiscuous mode and CSI capture,
 * registers callbacks, and pushes encoded binary records into g_ring_buffer.
 */

void wifi_sensor_init(void);
