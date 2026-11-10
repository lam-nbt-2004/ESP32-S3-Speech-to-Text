#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
esp_err_t voice_ws_start(const char *uri, const char *token);
bool voice_ws_is_ready(void);
bool voice_ws_is_listening(void);
bool voice_ws_is_playing(void);
bool voice_ws_is_conversation_active(void);
esp_err_t voice_ws_wake_start(const char *wake_phrase);
esp_err_t voice_ws_continue_listening(void);
esp_err_t voice_ws_send_opus(const uint8_t *packet, size_t len);
