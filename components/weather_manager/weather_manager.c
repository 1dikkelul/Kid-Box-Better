#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "mbedtls/error.h"
#include "cJSON.h"
#include "weather_manager.h"

static const char *TAG = "weather_manager";

// CONFIGURATION: Set your location here (Adelaide, SA)
#define LATITUDE  "-34.9285"
#define LONGITUDE "138.6007"

// Open-Meteo API URL (Free, No API Key required)
#define WEATHER_URL "https://api.open-meteo.com/v1/forecast?latitude=" LATITUDE "&longitude=" LONGITUDE "&current=temperature_2m,weather_code,wind_speed_10m,is_day"

static char response_buffer[2048];
static int response_len = 0;
static float current_temp = 0.0f;
static float current_wind_speed = 0.0f;
static int current_weather_code = -1; // -1 indicates not yet fetched
static int current_is_day = 1;        // 1 = daytime, 0 = night

static void log_heap_state(const char *label)
{
    ESP_LOGI(TAG, "%s | free_heap=%lu largest_block=%lu",
             label,
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

float weather_manager_get_temp(void)
{
    return current_temp;
}

float weather_manager_get_wind_speed(void)
{
    return current_wind_speed;
}

int weather_manager_get_weather_code(void)
{
    return current_weather_code;
}

int weather_manager_get_is_day(void)
{
    return current_is_day;
}



static esp_err_t _http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            // Accumulate data into the buffer (Chunked or not)
            if (response_len + evt->data_len < sizeof(response_buffer) - 1) {
                memcpy(response_buffer + response_len, evt->data, evt->data_len);
                response_len += evt->data_len;
                response_buffer[response_len] = '\0'; // Null-terminate
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

bool weather_manager_fetch(void)
{
    bool success = false;

    ESP_LOGI(TAG, "Fetching weather data...");
    log_heap_state("Before HTTP");
    
    // Reset buffer
    response_len = 0;
    memset(response_buffer, 0, sizeof(response_buffer));

    esp_http_client_config_t config = {
        .url = WEATHER_URL,
        .event_handler = _http_event_handler,
        .timeout_ms = 12000,
        .buffer_size = 1024,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach, // Use ESP-IDF default certificate bundle
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return false;
    }

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
                    cJSON *wind_item = cJSON_GetObjectItemCaseSensitive(current, "wind_speed_10m");
                    cJSON *is_day_item = cJSON_GetObjectItemCaseSensitive(current, "is_day");

                    if (cJSON_IsNumber(temp_item)) {
                        current_temp = (float)temp_item->valuedouble;
                        ESP_LOGI(TAG, "**************************************");
                        ESP_LOGI(TAG, "   CURRENT TEMP: %.1f C", current_temp);
                        ESP_LOGI(TAG, "**************************************");
                    }
                    if (cJSON_IsNumber(wind_item)) {
                        current_wind_speed = (float)wind_item->valuedouble;
                        ESP_LOGI(TAG, "   WIND SPEED: %.1f km/h", current_wind_speed);
                    }
                    if (cJSON_IsNumber(code_item)) {
                        current_weather_code = code_item->valueint;
                        ESP_LOGI(TAG, "   WEATHER CODE: %d", code_item->valueint);
                    }
                    if (cJSON_IsNumber(is_day_item)) {
                        current_is_day = is_day_item->valueint;
                        ESP_LOGI(TAG, "   IS_DAY: %d", current_is_day);
                    }

                    success = cJSON_IsNumber(temp_item) &&
                              cJSON_IsNumber(code_item) &&
                              cJSON_IsNumber(is_day_item);
                    if (!success) {
                        ESP_LOGW(TAG, "Weather JSON missing required fields");
                    }
                }
                cJSON_Delete(json);
            } else {
                ESP_LOGE(TAG, "Failed to parse JSON response");
            }
        }
    } else {
        int tls_code = 0;
        int tls_flags = 0;
        if (esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags) == ESP_OK && tls_code != 0) {
            char err_buf[128] = {0};
            mbedtls_strerror(tls_code, err_buf, sizeof(err_buf));
            ESP_LOGE(TAG, "TLS error: 0x%X (%s), verify_flags: 0x%X", (unsigned int)(-tls_code), err_buf, (unsigned int)tls_flags);
        }
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }

    esp_http_client_cleanup(client);
    log_heap_state("After HTTP cleanup");
    return success;
}

const char* weather_manager_get_icon_path_from_code(int code)
{
    // WMO weather interpretation codes mapped to SD card icon filenames (_144px).
    // Paths use LVGL POSIX FS driver (letter 'A'), files under /sdcard/weather/.
    switch (code) {
        case 0: // Clear sky
            return current_is_day ? "A:/sdcard/weather/clear_day_48.png"
                                  : "A:/sdcard/weather/clear_night_48.png";
        case 1: // Mainly clear
            return current_is_day ? "A:/sdcard/weather/mostly_clear_day_48.png"
                                  : "A:/sdcard/weather/mostly_clear_night_48.png";
        case 2: // Partly cloudy
            return current_is_day ? "A:/sdcard/weather/partly_cloudy_day_48.png"
                                  : "A:/sdcard/weather/partly_cloudy_night_48.png";
        case 3: // Overcast
            return "A:/sdcard/weather/cloudy_48.png";
        case 45:
        case 48: // Fog / rime fog
            return "A:/sdcard/weather/haze_fog_dust_smoke_48.png";
        case 51:
        case 53:
        case 55: // Drizzle: light, moderate, dense
            return "A:/sdcard/weather/drizzle_48.png";
        case 56:
        case 57: // Freezing drizzle
            return "A:/sdcard/weather/mixed_rain_hail_sleet_48.png";
        case 61:
        case 63: // Rain: slight, moderate
            return "A:/sdcard/weather/rain_with_cloudy_48.png";
        case 65: // Rain heavy
            return "A:/sdcard/weather/heavy_rain_48.png";
        case 66:
        case 67: // Freezing rain
            return "A:/sdcard/weather/sleet_hail_48.png";
        case 71:
        case 73:
        case 75:
        case 77:
        case 85:
        case 86: // Snow / snow showers — no snow icon, use sleet
            return "A:/sdcard/weather/sleet_hail_48.png";
        case 80: // Showers slight
            return current_is_day ? "A:/sdcard/weather/scattered_showers_day_48.png"
                                  : "A:/sdcard/weather/scattered_showers_night_48.png";
        case 81:
        case 82: // Showers moderate / violent
            return "A:/sdcard/weather/showers_rain_48.png";
        case 95: // Thunderstorm slight / moderate
            return "A:/sdcard/weather/thunderstorms_48.png";
        case 96: // Thunderstorm with slight hail
            return current_is_day ? "A:/sdcard/weather/isolated_scattered_thunderstorms_day_48.png"
                                  : "A:/sdcard/weather/isolated_scattered_thunderstorms_night_48.png";
        case 99: // Thunderstorm with heavy hail
            return "A:/sdcard/weather/strong_thunderstorms_48.png";
        default:
            return "";
    }
}