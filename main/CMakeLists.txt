# Keep source files grouped by responsibility: startup/state, connectivity,
# audio capture, recording policy, STT transport, and ESP-SR processing.
idf_component_register(
    SRCS
        "main.c"
        "voice_aiot_state.c"
        "wifi_manager.c"
        "audio_io.c"
        "audio_recorder.c"
        "stt_client.c"
        "voice_afe.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        esp-sr
        cjson
        nvs_flash
        esp_wifi
        esp_event
        esp_http_client
        esp_netif
        driver
        esp_driver_gpio
        esp_driver_i2s
)
