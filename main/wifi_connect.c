#include "wifi_connect.h"
#include "sdkconfig.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

#define WIFI_READY BIT0
static EventGroupHandle_t s_events;
static const char *TAG = "wifi";

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG, "Disconnected; retrying");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) xEventGroupSetBits(s_events, WIFI_READY);
}

esp_err_t wifi_connect_sta(void)
{
    if (CONFIG_VA_WIFI_SSID[0] == '\0') {
        ESP_LOGE(TAG, "Set Wi-Fi SSID/password in menuconfig");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        e = nvs_flash_init();
    }
    if (e != ESP_OK) return e;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL));
    wifi_config_t sta = { .sta = { .ssid = CONFIG_VA_WIFI_SSID, .password = CONFIG_VA_WIFI_PASSWORD,
                                   .threshold.authmode = WIFI_AUTH_OPEN } };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    ESP_ERROR_CHECK(esp_wifi_start());
    /* Continuous audio benefits from immediate delivery rather than DTIM
       power-saving bursts. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_READY, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    return (bits & WIFI_READY) ? ESP_OK : ESP_ERR_TIMEOUT;
}
