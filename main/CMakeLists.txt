set(srcs
    "i2s_pdm_example_main.c"
    "i2s_pdm_mic.c"
    "wifi_sta.c"
    "opus_encoder.c"
    "wss_streamer.c"
    "audio_vad.c"
    "cloud_uploader.c"
)

idf_component_register(SRCS "${srcs}"
                    PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_wifi esp_event nvs_flash esp_netif i2s_examples_common esp_http_client
                    INCLUDE_DIRS ".")
