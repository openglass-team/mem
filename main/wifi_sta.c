/*
 * SPDX-FileCopyrightText: 2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/timers.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "sdkconfig.h"
#include "wifi_sta.h"

static const char *TAG = "wifi_sta";

/* FreeRTOS event group to signal Wi-Fi connection status */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

static int s_retry_count = 0;
static const int WIFI_MAX_RETRY = 5;

/* Timeout per connection attempt (ms) */
#define WIFI_CONNECT_TIMEOUT_MS   15000
/* Overall timeout for all retries (ms) */
#define WIFI_TOTAL_TIMEOUT_MS     90000

static TimerHandle_t s_connect_timer = NULL;

/* Called when a single connection attempt times out */
static void wifi_connect_timeout_cb(TimerHandle_t timer)
{
    ESP_LOGW(TAG, "Connection attempt timeout (%d ms)", WIFI_CONNECT_TIMEOUT_MS);
    /* Force disconnect to trigger retry or fail */
    esp_wifi_disconnect();
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        /* Start per-connection timer, then connect */
        xTimerStart(s_connect_timer, 0);
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        /* Stop the per-connection timer — we got a result */
        xTimerStop(s_connect_timer, 0);

        wifi_event_sta_disconnected_t *disconn = (wifi_event_sta_disconnected_t *)event_data;
        ESP_LOGW(TAG, "Disconnected, reason: %d", disconn->reason);
        if (s_retry_count < WIFI_MAX_RETRY) {
            s_retry_count++;
            ESP_LOGI(TAG, "Retry %d/%d after %d ms...",
                     s_retry_count, WIFI_MAX_RETRY, WIFI_CONNECT_TIMEOUT_MS);
            vTaskDelay(pdMS_TO_TICKS(2000));  /* Small delay between retries */
            xTimerStart(s_connect_timer, 0);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "Max retries (%d) reached. Giving up.", WIFI_MAX_RETRY);
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        /* Stop the per-connection timer — success! */
        xTimerStop(s_connect_timer, 0);

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    /* Create one-shot timer for per-connection-attempt timeout */
    s_connect_timer = xTimerCreate("wifi_to",
                                   pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS),
                                   pdFALSE,   /* one-shot */
                                   NULL,
                                   wifi_connect_timeout_cb);

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "Wi-Fi init finished. Connecting to SSID: \"%s\"", CONFIG_WIFI_SSID);

    /* Wait for connection or failure, with overall timeout */
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(WIFI_TOTAL_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to \"%s\"", CONFIG_WIFI_SSID);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "All retries exhausted. Failed to connect to \"%s\"", CONFIG_WIFI_SSID);
    } else {
        /* Timeout: neither succeeded nor explicitly failed within total timeout */
        ESP_LOGE(TAG, "Wi-Fi connection timed out after %d ms", WIFI_TOTAL_TIMEOUT_MS);
        xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }

    /* Clean up the timer */
    xTimerStop(s_connect_timer, 0);
    xTimerDelete(s_connect_timer, 0);
    s_connect_timer = NULL;
}

bool wifi_is_connected(void)
{
    if (s_wifi_event_group == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}
