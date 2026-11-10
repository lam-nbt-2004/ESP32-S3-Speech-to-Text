set(app_sources "app_main.c" "audio_io.c")
set(app_requires esp_driver_gpio esp_driver_i2s esp_netif esp_wifi esp_event nvs_flash esp_hw_support)

if(CONFIG_VA_STAGE_AFE_TEST OR CONFIG_VA_STAGE_SERVER)
    list(APPEND app_sources "voice_afe.c")
    list(APPEND app_requires esp-sr)
endif()

if(CONFIG_VA_STAGE_SERVER)
    list(APPEND app_sources "voice_ws.c" "wifi_connect.c" "device_discovery.c")
    list(APPEND app_requires esp_websocket_client cjson mbedtls micro-opus)
endif()

if(CONFIG_VA_STAGE_SERVER)
    idf_component_register(SRCS ${app_sources} INCLUDE_DIRS "."
                           PRIV_REQUIRES ${app_requires} esp_http_client)
    target_include_directories(${COMPONENT_LIB} PRIVATE
                               "$ENV{IDF_PATH}/components/esp_http_client/include")
else()
    idf_component_register(SRCS ${app_sources} INCLUDE_DIRS "."
                           PRIV_REQUIRES ${app_requires})
endif()
