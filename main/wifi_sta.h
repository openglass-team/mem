/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize Wi-Fi in station mode.
 *
 * Connects to the AP configured via menuconfig (WIFI_SSID / WIFI_PASSWORD).
 * Blocks until connection is established or max retries reached.
 */
void wifi_init_sta(void);

/**
 * @brief Check if Wi-Fi is currently connected.
 * @return true if connected and got IP, false otherwise.
 */
bool wifi_is_connected(void);

#ifdef __cplusplus
}
#endif
