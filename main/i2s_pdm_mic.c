/*
 * PDM mic driver — exact ESP-IDF i2s_pdm_rx example config, only pins changed.
 * STEREO + ALL slots on ESP32-S3, no software extraction.
 * Output ALL samples to stream buffer at full hardware rate.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "soc/soc_caps.h"
#include "driver/i2s_pdm.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "sdkconfig.h"
#include "i2s_pdm_example.h"
#include "i2s_pdm_mic.h"

/* XIAO ESP32S3 Sense: CLK=GPIO42, DATA=GPIO41 */
#define PDM_CLK GPIO_NUM_42
#define PDM_DIN GPIO_NUM_41

static const char *TAG = "i2s_pdm_mic";
static StreamBufferHandle_t s_buf = NULL;

void i2s_mic_init(void) {
    s_buf = xStreamBufferCreate(audio_frame_bytes() * 16, audio_frame_bytes());
    assert(s_buf);
    ESP_LOGI(TAG, "buf: %dB trigger=%d", audio_frame_bytes()*16, audio_frame_bytes());
}
StreamBufferHandle_t i2s_mic_get_stream_buffer(void) { return s_buf; }

void i2s_mic_start_read(void *pv) {
    i2s_chan_handle_t rx;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    cc.dma_frame_num = 512;   /* Larger DMA buffer for smooth data */
    cc.dma_desc_num = 8;      /* More descriptors to avoid overflow */
    ESP_ERROR_CHECK(i2s_new_channel(&cc, NULL, &rx));

    i2s_pdm_rx_config_t pdm = {
        .clk_cfg  = I2S_PDM_RX_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_PDM_RX_SLOT_PCM_FMT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .clk = PDM_CLK,
#if SOC_I2S_PDM_MAX_RX_LINES == 4
            .dins = { PDM_DIN, GPIO_NUM_NC, GPIO_NUM_NC, GPIO_NUM_NC },
#else
            .din = PDM_DIN,
#endif
            .invert_flags = { .clk_inv = false },
        },
    };
#if CONFIG_IDF_TARGET_ESP32S3
    pdm.slot_cfg.slot_mode = I2S_SLOT_MODE_STEREO;
    pdm.slot_cfg.slot_mask = I2S_PDM_LINE_SLOT_ALL;
#endif
    ESP_ERROR_CHECK(i2s_channel_init_pdm_rx_mode(rx, &pdm));
    ESP_ERROR_CHECK(i2s_channel_enable(rx));

    int16_t *rb = calloc(1, EXAMPLE_BUFF_SIZE);
    assert(rb);
    size_t rbytes;

    /* Accumulate extracted D0_L samples across I2S reads.
     * Each read = 2048B = 1024 samples → 1024/8 = 128 D0_L samples = 256B.
     * Need 3 reads × 256B = 768B to fill one 640B frame. */
    uint8_t acc[1280];
    int acc_len = 0;
    int n = 0;
    ESP_LOGI(TAG, "started DMA:512×8");

    while (1) {
        if (i2s_channel_read(rx, rb, EXAMPLE_BUFF_SIZE, &rbytes, pdMS_TO_TICKS(100)) == ESP_OK && rbytes > 0) {
            int total = (int)rbytes / sizeof(int16_t);
            /* Extract D0_L: STEREO+ALL → index 0,8,16,... of [D0_L,D0_R,D1_L,D1_R,D2_L,D2_R,D3_L,D3_R] */
            for (int i = 0; i < total; i += 8) {
                if (acc_len + 2 > (int)sizeof(acc)) {
                    xStreamBufferSend(s_buf, acc, acc_len, pdMS_TO_TICKS(5));
                    acc_len = 0;
                }
                memcpy(acc + acc_len, &rb[i], sizeof(int16_t));
                acc_len += sizeof(int16_t);
            }
            if (++n <= 2) ESP_LOGI(TAG, "rd %dB → %d D0_L samples, acc=%dB",
                                     (int)rbytes, total/8, acc_len);
        }
    }
    free(rb);
    vTaskDelete(NULL);
}
