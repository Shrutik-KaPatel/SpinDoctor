#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "spindoctor_gateway";

#define MAX_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT       BIT1

#define STM32_UART_PORT UART_NUM_2
#define STM32_UART_RX_PIN 16
#define STM32_UART_TX_PIN 17
#define STM32_UART_BUF_SIZE 256

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

static void stm32_uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    };
    ESP_ERROR_CHECK(uart_param_config(STM32_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(STM32_UART_PORT, STM32_UART_TX_PIN, STM32_UART_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(STM32_UART_PORT, STM32_UART_BUF_SIZE * 2, 0, 0, NULL, 0));
}

static void stm32_uart_task(void *arg)
{
    static char line_buf[STM32_UART_BUF_SIZE];
    static int line_pos = 0;
    uint8_t byte;

    while (1) {
        int len = uart_read_bytes(STM32_UART_PORT, &byte, 1, pdMS_TO_TICKS(1000));
        if (len > 0) {
            if (byte == '\n') {
                line_buf[line_pos] = '\0';
                ESP_LOGI("STM32_LINK", "Received: %s", line_buf);
                line_pos = 0;
            } else if (line_pos < STM32_UART_BUF_SIZE - 1) {
                line_buf[line_pos++] = byte;
            } else {
                /* Line too long, something's wrong upstream; reset to
                 * avoid overflowing the buffer silently. */
                ESP_LOGW("STM32_LINK", "Line overflow, discarding");
                line_pos = 0;
            }
        }
    }
}

void app_main(void)
{
    esp_err_t err;

    err = nvs_flash_init_partition("creds");
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase_partition("creds"));
        err = nvs_flash_init_partition("creds");
    }
    ESP_ERROR_CHECK(err);

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

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

    stm32_uart_init();
    xTaskCreate(stm32_uart_task, "stm32_uart_task", 4096, NULL, 5, NULL);
}
