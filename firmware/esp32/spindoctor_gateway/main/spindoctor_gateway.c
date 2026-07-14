#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

static const char *TAG = "spindoctor_gateway";

#define MAX_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT       BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGW(TAG, "Retrying WiFi connect (%d/%d)", s_retry_count, MAX_RETRY);
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(const char *ssid, const char *pass)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                &wifi_event_handler, NULL));

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished, waiting for connection...");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to SSID: %s", ssid);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID: %s after %d attempts", ssid, MAX_RETRY);
    }
}

void app_main(void)
{
    esp_err_t err;

    // Init the CREDS partition specifically (not the default NVS partition)
    err = nvs_flash_init_partition("creds");
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition("creds"));
        err = nvs_flash_init_partition("creds");
    }
    ESP_ERROR_CHECK(err);

    // Also init the default NVS partition, since the WiFi driver needs it internally
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Open a handle scoped to the "wifi_creds" namespace inside the "creds" partition
    nvs_handle_t handle;
    err = nvs_open_from_partition("creds", "wifi_creds", NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open creds partition: %s", esp_err_to_name(err));
        return;
    }

    char ssid[33] = {0};
    size_t ssid_len = sizeof(ssid);
    err = nvs_get_str(handle, "wifi_ssid", ssid, &ssid_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read wifi_ssid: %s", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }

    char pass[65] = {0};
    size_t pass_len = sizeof(pass);
    err = nvs_get_str(handle, "wifi_pass", pass, &pass_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read wifi_pass: %s", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }

    nvs_close(handle);

    ESP_LOGI(TAG, "Loaded SSID: %s", ssid);
    ESP_LOGI(TAG, "Password length: %d chars", (int)strlen(pass));

    wifi_init_sta(ssid, pass);
}
