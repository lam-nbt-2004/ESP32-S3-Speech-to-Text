#include "device_discovery.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_mac.h"
#include "esp_log.h"
#include "esp_check.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DISCOVERY_BODY_MAX 2048

static const char *TAG = "device_discovery";

typedef struct {
    char body[DISCOVERY_BODY_MAX];
    size_t used;
    bool overflow;
} response_buffer_t;

static esp_err_t response_event(esp_http_client_event_t *event)
{
    response_buffer_t *response = event->user_data;
    if (event->event_id != HTTP_EVENT_ON_DATA || !event->data || event->data_len <= 0) {
        return ESP_OK;
    }
    size_t len = (size_t)event->data_len;
    if (len >= sizeof(response->body) - response->used) {
        response->overflow = true;
        return ESP_FAIL;
    }
    memcpy(response->body + response->used, event->data, len);
    response->used += len;
    response->body[response->used] = '\0';
    return ESP_OK;
}

static esp_err_t make_discovery_url(const char *ws_uri, char *http_url, size_t cap)
{
    const char *authority;
    const char *scheme;
    if (strncmp(ws_uri, "wss://", 6) == 0) {
        scheme = "https://";
        authority = ws_uri + 6;
    } else if (strncmp(ws_uri, "ws://", 5) == 0) {
        scheme = "http://";
        authority = ws_uri + 5;
    } else {
        return ESP_ERR_INVALID_ARG;
    }

    const char *path = strchr(authority, '/');
    size_t host_len = path ? (size_t)(path - authority) : strlen(authority);
    if (host_len == 0) return ESP_ERR_INVALID_ARG;
    int n = snprintf(http_url, cap, "%s%.*s/api/device/ota/", scheme,
                     (int)host_len, authority);
    return (n >= 0 && (size_t)n < cap) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t device_discovery_fetch(const char *bootstrap_ws_uri,
                                 char *ws_uri, size_t ws_uri_size,
                                 char *token, size_t token_size)
{
    if (!bootstrap_ws_uri || !ws_uri || !ws_uri_size || !token || !token_size) {
        return ESP_ERR_INVALID_ARG;
    }

    char url[320];
    esp_err_t err = make_discovery_url(bootstrap_ws_uri, url, sizeof(url));
    if (err != ESP_OK) return err;

    response_buffer_t response = {0};
    const char *request = "{\"application\":{\"version\":\"1.0.0\"}}";
    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = response_event,
        .user_data = &response,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;

    uint8_t mac[6];
    err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    char device_id[18];
    int device_id_len = snprintf(device_id, sizeof(device_id),
                                 "%02x:%02x:%02x:%02x:%02x:%02x",
                                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    if (device_id_len < 0 || (size_t)device_id_len >= sizeof(device_id)) {
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    err = esp_http_client_set_header(client, "Content-Type", "application/json");
    if (err == ESP_OK) err = esp_http_client_set_header(client, "Device-Id", device_id);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }
    ESP_ERROR_CHECK(esp_http_client_set_post_field(client, request, (int)strlen(request)));
    err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    if (response.overflow) return ESP_ERR_INVALID_SIZE;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA discovery request failed: %s", esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "OTA discovery returned HTTP %d", status);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_ParseWithLength(response.body, response.used);
    if (!root) return ESP_ERR_INVALID_RESPONSE;
    const cJSON *websocket = cJSON_GetObjectItemCaseSensitive(root, "websocket");
    if (!cJSON_IsObject(websocket)) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    const cJSON *url_item = cJSON_GetObjectItemCaseSensitive(websocket, "url");
    const cJSON *token_item = cJSON_GetObjectItemCaseSensitive(websocket, "token");
    if (!cJSON_IsString(url_item) || !url_item->valuestring ||
        (token_item && !cJSON_IsString(token_item))) {
        cJSON_Delete(root);
        return ESP_ERR_INVALID_RESPONSE;
    }
    int url_len = snprintf(ws_uri, ws_uri_size, "%s", url_item->valuestring);
    const char *token_value = cJSON_IsString(token_item) ? token_item->valuestring : "";
    int token_len = snprintf(token, token_size, "%s", token_value);
    cJSON_Delete(root);
    if (url_len < 0 || (size_t)url_len >= ws_uri_size ||
        token_len < 0 || (size_t)token_len >= token_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (strcmp(token, "none") == 0) {
        ESP_LOGW(TAG, "Device %s is not paired; approve it in the server device page, then reboot",
                 device_id);
    } else {
        ESP_LOGI(TAG, "Discovered device WebSocket endpoint and token for %s", device_id);
    }
    return ESP_OK;
}
