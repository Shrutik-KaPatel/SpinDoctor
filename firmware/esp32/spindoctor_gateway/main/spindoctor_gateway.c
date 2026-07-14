#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "driver/uart.h"
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

static const char *TAG = "spindoctor_gateway";

#define MAX_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT       BIT1

#define STM32_UART_PORT UART_NUM_2
#define STM32_UART_RX_PIN 16
#define STM32_UART_TX_PIN 17
#define STM32_UART_BUF_SIZE 256

#define GEMINI_MAX_RESPONSE 4096

#define APPS_SCRIPT_URL "https://script.google.com/macros/s/AKfycbwybMfmNkcJr13etcb2Gt6GcHDdKg6KT9faJbAHrI92y__-Ti73OX6MKpL_-6UCHf_MVg/exec"

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_count = 0;

typedef struct {
    char fault_class[16];
    float confidence;
    float healthy;
    float imbalance;
    float obstruction;
    float temp_c;
    bool valid;
} spindoctor_reading_t;

typedef struct {
    char *buffer;
    int len;
} http_response_buf_t;

/* Pushed by stm32_uart_task on every fault_class change, drained by
 * sheets_log_task. Keeps the (slow, sometimes-flaky) HTTPS POST
 * completely off the UART-reading task, since a blocked network call
 * there was causing the STM32's internal UART ring buffer to overflow
 * and corrupt subsequent lines. */
typedef struct {
    char fault_class[16];
    float temp_c;
} log_request_t;

static SemaphoreHandle_t s_reading_mutex;
static spindoctor_reading_t s_latest_reading = { .valid = false };
static char s_last_logged_class[16] = {0};
static QueueHandle_t s_log_queue;

/* Serializes all HTTP calls to script.google.com. Two independent
 * tasks (gateway_poll_task and sheets_log_task) can both want to hit
 * this same hostname at once; lwIP's DNS resolver has a limited number
 * of concurrent outstanding lookups per hostname, and racing both
 * tasks against it was causing intermittent getaddrinfo() failures.
 * Serializing avoids the race entirely. */
static SemaphoreHandle_t s_apps_script_mutex;

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

static spindoctor_reading_t parse_stm32_line(const char *line)
{
    spindoctor_reading_t reading = { .valid = false };

    cJSON *root = cJSON_Parse(line);
    if (root == NULL) {
        ESP_LOGW("PARSE", "JSON parse failed for line: %s", line);
        return reading;
    }

    cJSON *fault_class = cJSON_GetObjectItem(root, "fault_class");
    cJSON *confidence  = cJSON_GetObjectItem(root, "confidence");
    cJSON *healthy     = cJSON_GetObjectItem(root, "healthy");
    cJSON *imbalance   = cJSON_GetObjectItem(root, "imbalance");
    cJSON *obstruction = cJSON_GetObjectItem(root, "obstruction");
    cJSON *temp_c      = cJSON_GetObjectItem(root, "temp_c");

    if (!cJSON_IsString(fault_class) || !cJSON_IsNumber(confidence) ||
        !cJSON_IsNumber(healthy) || !cJSON_IsNumber(imbalance) ||
        !cJSON_IsNumber(obstruction) || !cJSON_IsNumber(temp_c)) {
        ESP_LOGW("PARSE", "Missing or malformed field in line: %s", line);
        cJSON_Delete(root);
        return reading;
    }

    strncpy(reading.fault_class, fault_class->valuestring, sizeof(reading.fault_class) - 1);
    reading.confidence  = (float)confidence->valuedouble;
    reading.healthy     = (float)healthy->valuedouble;
    reading.imbalance   = (float)imbalance->valuedouble;
    reading.obstruction = (float)obstruction->valuedouble;
    reading.temp_c      = (float)temp_c->valuedouble;
    reading.valid       = true;

    cJSON_Delete(root);
    return reading;
}

static esp_err_t gemini_http_event_handler(esp_http_client_event_t *evt)
{
    http_response_buf_t *resp = (http_response_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA) {
        if (resp->len + evt->data_len < GEMINI_MAX_RESPONSE) {
            memcpy(resp->buffer + resp->len, evt->data, evt->data_len);
            resp->len += evt->data_len;
        }
    }
    return ESP_OK;
}

static bool call_gemini(const char *api_key, const spindoctor_reading_t *reading, char *out_explanation, size_t out_size)
{
    char request_body[512];
    snprintf(request_body, sizeof(request_body),
        "{\"contents\":[{\"parts\":[{\"text\":"
        "\"Explain this fan diagnostic reading in plain English, 2-3 sentences: "
        "fault class %s, confidence %.2f, temperature %.1fC.\"}]}]}",
        reading->fault_class, reading->confidence, reading->temp_c);

    char url[256];
    snprintf(url, sizeof(url),
        "https://generativelanguage.googleapis.com/v1beta/models/gemini-flash-lite-latest:generateContent?key=%s",
        api_key);

    static char response_data[GEMINI_MAX_RESPONSE];
    http_response_buf_t resp = { .buffer = response_data, .len = 0 };
    memset(response_data, 0, sizeof(response_data));

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .event_handler = gemini_http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, request_body, strlen(request_body));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGE("GEMINI", "HTTP request failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI("GEMINI", "HTTP status: %d", status_code);

    if (status_code != 200) {
        ESP_LOGE("GEMINI", "Non-200 response: %s", response_data);
        return false;
    }

    cJSON *root = cJSON_Parse(response_data);
    if (root == NULL) {
        ESP_LOGE("GEMINI", "Failed to parse Gemini response JSON");
        return false;
    }

    bool success = false;
    cJSON *candidates = cJSON_GetObjectItem(root, "candidates");
    if (cJSON_IsArray(candidates) && cJSON_GetArraySize(candidates) > 0) {
        cJSON *first = cJSON_GetArrayItem(candidates, 0);
        cJSON *content = cJSON_GetObjectItem(first, "content");
        cJSON *parts = cJSON_GetObjectItem(content, "parts");
        if (cJSON_IsArray(parts) && cJSON_GetArraySize(parts) > 0) {
            cJSON *part0 = cJSON_GetArrayItem(parts, 0);
            cJSON *text = cJSON_GetObjectItem(part0, "text");
            if (cJSON_IsString(text)) {
                strncpy(out_explanation, text->valuestring, out_size - 1);
                success = true;
            }
        }
    }

    cJSON_Delete(root);
    return success;
}

/* Apps Script executes the POST side effect (writing to the sheet)
 * before issuing its redirect, so a 302 here already means success.
 * We never use the response body for log_reading/submit_explanation,
 * so we don't follow the redirect at all, avoiding an unnecessary
 * second network round-trip that could itself flake.
 *
 * Serialized behind s_apps_script_mutex: see comment at declaration. */
static bool apps_script_post(const char *json_body)
{
    xSemaphoreTake(s_apps_script_mutex, portMAX_DELAY);

    static char response_data[1024];
    http_response_buf_t resp = { .buffer = response_data, .len = 0 };
    memset(response_data, 0, sizeof(response_data));

    esp_http_client_config_t config = {
        .url = APPS_SCRIPT_URL,
        .method = HTTP_METHOD_POST,
        .event_handler = gemini_http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json_body, strlen(json_body));

    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    xSemaphoreGive(s_apps_script_mutex);

    if (err != ESP_OK || (status_code != 200 && status_code != 302)) {
        ESP_LOGE("APPS_SCRIPT", "POST failed, status=%d err=%s", status_code, esp_err_to_name(err));
        return false;
    }

    return true;
}

/* Serialized behind s_apps_script_mutex: see comment at declaration. */
static bool apps_script_get(const char *query_suffix, char *out_response, size_t out_size)
{
    xSemaphoreTake(s_apps_script_mutex, portMAX_DELAY);

    static char response_data[1024];
    http_response_buf_t resp = { .buffer = response_data, .len = 0 };
    memset(response_data, 0, sizeof(response_data));

    char url[512];
    snprintf(url, sizeof(url), "%s%s", APPS_SCRIPT_URL, query_suffix);

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_GET,
        .event_handler = gemini_http_event_handler,
        .user_data = &resp,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
        .disable_auto_redirect = true,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);

    if (err == ESP_OK && (status_code == 302 || status_code == 303)) {
        esp_http_client_set_redirection(client);
        resp.len = 0;
        memset(response_data, 0, sizeof(response_data));
        err = esp_http_client_perform(client);
        status_code = esp_http_client_get_status_code(client);
    }

    esp_http_client_cleanup(client);
    xSemaphoreGive(s_apps_script_mutex);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGE("APPS_SCRIPT", "GET failed, status=%d err=%s", status_code, esp_err_to_name(err));
        return false;
    }

    strncpy(out_response, response_data, out_size - 1);
    return true;
}

/* Drains s_log_queue and performs the (slow, sometimes-flaky) HTTPS POST
 * to Apps Script, completely off the UART-reading task. */
static void sheets_log_task(void *arg)
{
    log_request_t req;
    while (1) {
        if (xQueueReceive(s_log_queue, &req, portMAX_DELAY) == pdTRUE) {
            char log_body[256];
            snprintf(log_body, sizeof(log_body),
                "{\"action\":\"log_reading\",\"fault_class\":\"%s\",\"temp_c\":%.1f,\"note\":\"auto\"}",
                req.fault_class, req.temp_c);

            if (apps_script_post(log_body)) {
                ESP_LOGI("SHEETS", "Logged fault_class change: %s", req.fault_class);
            } else {
                ESP_LOGW("SHEETS", "Failed to log fault_class change: %s", req.fault_class);
            }
        }
    }
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
                line_pos = 0;

                spindoctor_reading_t reading = parse_stm32_line(line_buf);
                if (reading.valid) {
                    ESP_LOGI("PARSE", "fault_class=%s confidence=%.2f healthy=%.2f imbalance=%.2f obstruction=%.2f temp_c=%.1f",
                             reading.fault_class, reading.confidence,
                             reading.healthy, reading.imbalance,
                             reading.obstruction, reading.temp_c);

                    xSemaphoreTake(s_reading_mutex, portMAX_DELAY);
                    s_latest_reading = reading;
                    xSemaphoreGive(s_reading_mutex);

                    if (strcmp(reading.fault_class, s_last_logged_class) != 0) {
                        strncpy(s_last_logged_class, reading.fault_class, sizeof(s_last_logged_class) - 1);

                        log_request_t req;
                        strncpy(req.fault_class, reading.fault_class, sizeof(req.fault_class) - 1);
                        req.temp_c = reading.temp_c;

                        /* Non-blocking: if the queue is momentarily full
                         * (log task mid-request), drop this one rather
                         * than ever blocking UART reading. */
                        if (xQueueSend(s_log_queue, &req, 0) != pdTRUE) {
                            ESP_LOGW("SHEETS", "Log queue full, dropping change event: %s", reading.fault_class);
                        }
                    }
                }
            } else if (line_pos < STM32_UART_BUF_SIZE - 1) {
                line_buf[line_pos++] = byte;
            } else {
                ESP_LOGW("STM32_LINK", "Line overflow, discarding");
                line_pos = 0;
            }
        }
    }
}

static void gateway_poll_task(void *arg)
{
    const char *api_key = (const char *)arg;

    while (1) {
        char response[1024] = {0};
       if (apps_script_get("?action=check_trigger", response, sizeof(response))) {
            ESP_LOGI("POLL", "check_trigger response: %s", response);
            cJSON *root = cJSON_Parse(response);
            if (root) {
                cJSON *trigger = cJSON_GetObjectItem(root, "trigger");
                if (cJSON_IsTrue(trigger)) {
                    ESP_LOGI("POLL", "Trigger detected, calling Gemini with latest reading");

                    spindoctor_reading_t reading_copy;
                    xSemaphoreTake(s_reading_mutex, portMAX_DELAY);
                    reading_copy = s_latest_reading;
                    xSemaphoreGive(s_reading_mutex);

                    char explanation[1024] = {0};
                    if (reading_copy.valid && call_gemini(api_key, &reading_copy, explanation, sizeof(explanation))) {
                        char post_body[1536];
                        snprintf(post_body, sizeof(post_body),
                            "{\"action\":\"submit_explanation\",\"explanation\":\"%s\"}",
                            explanation);
                        if (apps_script_post(post_body)) {
                            ESP_LOGI("POLL", "Explanation submitted");
                        } else {
                            ESP_LOGW("POLL", "Failed to submit explanation");
                        }
                    } else {
                        ESP_LOGW("POLL", "No valid reading yet, or Gemini call failed");
                    }
                }
                cJSON_Delete(root);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5000));
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

    char gemini_key[80] = {0};
    size_t gemini_key_len = sizeof(gemini_key);
    err = nvs_get_str(handle, "gemini_api_key", gemini_key, &gemini_key_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read gemini_api_key: %s", esp_err_to_name(err));
        nvs_close(handle);
        return;
    }
    ESP_LOGI(TAG, "Gemini key length: %d chars", (int)strlen(gemini_key));
    nvs_close(handle);

    ESP_LOGI(TAG, "Loaded SSID: %s", ssid);
    ESP_LOGI(TAG, "Password length: %d chars", (int)strlen(pass));

    wifi_init_sta(ssid, pass);

    s_reading_mutex = xSemaphoreCreateMutex();
    s_apps_script_mutex = xSemaphoreCreateMutex();
    s_log_queue = xQueueCreate(5, sizeof(log_request_t));

    stm32_uart_init();
    xTaskCreate(stm32_uart_task, "stm32_uart_task", 4096, NULL, 5, NULL);
    xTaskCreate(sheets_log_task, "sheets_log_task", 8192, NULL, 4, NULL);

    static char s_gemini_key[80];
    strncpy(s_gemini_key, gemini_key, sizeof(s_gemini_key) - 1);
    xTaskCreate(gateway_poll_task, "gateway_poll_task", 8192, s_gemini_key, 5, NULL);
}
