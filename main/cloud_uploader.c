/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 *
 * Cloud uploader: POST/PUT recognized text to cloud storage via HTTP.
 * Designed for Baidu BOS (Object Storage) with Bearer token auth.
 */

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include "esp_log.h"
#include "esp_http_client.h"
#include "sdkconfig.h"
#include "cloud_uploader.h"

static const char *TAG = "cloud_upload";

bool cloud_upload_text(const char *text, int length)
{
    if (text == NULL || length <= 0) {
        return false;
    }

    /* Build object key: {prefix}asr_YYYYMMDD_HHMMSS.txt */
    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char filename[128];
    snprintf(filename, sizeof(filename),
             "%sasr_%04d%02d%02d_%02d%02d%02d.txt",
             CONFIG_CLOUD_UPLOAD_PATH_PREFIX,
             tm_info->tm_year + 1900, tm_info->tm_mon + 1, tm_info->tm_mday,
             tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

    /* Build full URL */
    char url[512];
    snprintf(url, sizeof(url), "%s/%s", CONFIG_CLOUD_UPLOAD_URL, filename);

    /* Build Authorization header */
    char auth_hdr[384];
    const char *token = CONFIG_VC_API_KEY;
    snprintf(auth_hdr, sizeof(auth_hdr), "Bearer %s",
             (token != NULL && strlen(token) > 0) ? token : "");

    ESP_LOGI(TAG, "Uploading %d bytes to: %s", length, url);

    esp_http_client_config_t http_cfg = {
        .url = url,
        .method = HTTP_METHOD_PUT,
        .timeout_ms = 10000,
        .buffer_size = 1024,
    };

#if CONFIG_WSS_SKIP_CERT_VERIFY
    http_cfg.skip_cert_common_name_check = true;
    ESP_LOGW(TAG, "TLS certificate verification DISABLED (testing mode)");
#endif

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to init HTTP client");
        return false;
    }

    /* Set headers */
    esp_http_client_set_header(client, "Authorization", auth_hdr);
    esp_http_client_set_header(client, "Content-Type", "text/plain; charset=utf-8");

    /* Attach body */
    esp_http_client_set_post_field(client, text, length);

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);

    esp_http_client_cleanup(client);

    if (err == ESP_OK && status >= 200 && status < 300) {
        ESP_LOGI(TAG, "Upload OK: HTTP %d → %s", status, filename);
        return true;
    } else {
        ESP_LOGE(TAG, "Upload failed: err=%d, HTTP %d", err, status);
        return false;
    }
}
