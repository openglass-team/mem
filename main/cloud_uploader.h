/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#pragma once

/**
 * @brief Upload a text document to cloud storage (Baidu BOS).
 *
 * Uses HTTP PUT with Bearer token authorization (reuses CONFIG_WSS_AUTH_TOKEN).
 * File is named: {CONFIG_CLOUD_UPLOAD_PATH_PREFIX}asr_YYYYMMDD_HHMMSS.txt
 *
 * @param text    Null-terminated text content to upload.
 * @param length  Length of the text in bytes.
 * @return        true if upload succeeded (HTTP 200), false otherwise.
 */
bool cloud_upload_text(const char *text, int length);
