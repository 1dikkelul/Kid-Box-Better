#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "weather_manager.h"

static const char *TAG = "weather_manager";

// CONFIGURATION: Set your location here (Adelaide, SA)
#define LATITUDE  "-34.9285"
#define LONGITUDE "138.6007"

// Open-Meteo API URL (Free, No API Key required)
#define WEATHER_URL "https://api.open-meteo.com/v1/forecast?latitude=" LATITUDE "&longitude=" LONGITUDE "&current=temperature_2m,weather_code"

static char response_buffer[2048];
static int response_len = 0;
static float current_temp = 0.0f;

float weather_manager_get_temp(void)
{
    return current_temp;
}

static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (!esp_http_client_is_chunked_response(evt->client)) {
                // Accumulate data into the buffer
                int copy_len = evt->data_len;
                if (response_len + copy_len < sizeof(response_buffer) - 1) {
                    memcpy(response_buffer + response_len, evt->data, copy_len);
                    response_len += copy_len;
                    response_buffer[response_len] = '\0'; // Null-terminate
                }
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            break;
        case HTTP_EVENT_DISCONNECTED:
            break;
        default:
            break;
    }
    return ESP_OK;
}

void weather_manager_fetch(void)
{
    ESP_LOGI(TAG, "Fetching weather data...");
    
    // Reset buffer
    response_len = 0;
    memset(response_buffer, 0, sizeof(response_buffer));

    esp_http_client_config_t config = {
        .url = WEATHER_URL,
        .event_handler = _http_event_handler,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach, // Use ESP-IDF default certificate bundle
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    if (err == ESP_OK) {
        int status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP Status: %d, Response Len: %d", status_code, response_len);

        if (status_code == 200 && response_len > 0) {
            // Parse JSON
            cJSON *json = cJSON_Parse(response_buffer);
            if (json) {
                // Open-Meteo returns: { "current": { "temperature_2m": 15.5, "weather_code": 3 } ... }
                cJSON *current = cJSON_GetObjectItemCaseSensitive(json, "current");
                if (current) {
                    cJSON *temp_item = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
                    cJSON *code_item = cJSON_GetObjectItemCaseSensitive(current, "weather_code");

                    if (temp_item) {
                        current_temp = (float)temp_item->valuedouble;
                        ESP_LOGI(TAG, "**************************************");
                        ESP_LOGI(TAG, "   CURRENT TEMP: %.1f C", current_temp);
                        ESP_LOGI(TAG, "**************************************");
                    }
                    if (code_item) {
                        ESP_LOGI(TAG, "   WEATHER CODE: %d", code_item->valueint);
                    }
                }
                cJSON_Delete(json);
            } else {
                ESP_LOGE(TAG, "Failed to parse JSON response");
            }
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
}