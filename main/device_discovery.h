#pragma once

#include <stddef.h>
#include "esp_err.h"

/* Resolve the current device WebSocket URL and token from /api/device/ota/. */
esp_err_t device_discovery_fetch(const char *bootstrap_ws_uri,
                                 char *ws_uri, size_t ws_uri_size,
                                 char *token, size_t token_size);
