#include <string.h>
#include <stdio.h>
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

// CONFIGURATION: User-provided coordinates
// 34° 55' 37.20" S, 138° 30' 3.60" E -> -34.927000, 138.501000
#define LATITUDE  "-34.927000"
#define LONGITUDE "138.501000"

// Open-Meteo API URL (Free, No API Key required)
#define WEATHER_URL "https://api.open-meteo.com/v1/forecast?latitude=" LATITUDE "&longitude=" LONGITUDE "&timezone=auto&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,is_day&hourly=temperature_2m,weather_code,is_day"

enum {
    WEATHER_ICON_SIZE_65 = 65,
    WEATHER_ICON_SIZE_144 = 144,
};

typedef enum {
    ICON_NA = 0,
    ICON_DAY_SUNNY,
    ICON_NIGHT_CLEAR,
    ICON_DAY_CLOUDY,
    ICON_NIGHT_ALT_CLOUDY,
    ICON_CLOUDY,
    ICON_DAY_FOG,
    ICON_FOG,
    ICON_SPRINKLE,
    ICON_RAINDROP,
    ICON_RAIN,
    ICON_SHOWERS,
    ICON_DAY_THUNDERSTORM,
    ICON_NIGHT_ALT_THUNDERSTORM,
    ICON_COUNT,
} weather_icon_id_t;

static const int s_slot_hours[WEATHER_MANAGER_SLOT_COUNT] = {7, 10, 13, 16, 19};

static weather_manager_hourly_slot_t s_hourly_slots[WEATHER_MANAGER_SLOT_COUNT] = {
    {.hour_24 = 7, .valid = false},
    {.hour_24 = 10, .valid = false},
    {.hour_24 = 13, .valid = false},
    {.hour_24 = 16, .valid = false},
    {.hour_24 = 19, .valid = false},
};

static const char *s_icon_paths_65[ICON_COUNT] = {
    [ICON_NA] = "A:/sdcard/weather/wi-na_65.bin",
    [ICON_DAY_SUNNY] = "A:/sdcard/weather/wi-day-sunny_65.bin",
    [ICON_NIGHT_CLEAR] = "A:/sdcard/weather/wi-night-clear_65.bin",
    [ICON_DAY_CLOUDY] = "A:/sdcard/weather/wi-day-cloudy_65.bin",
    [ICON_NIGHT_ALT_CLOUDY] = "A:/sdcard/weather/wi-night-alt-cloudy_65.bin",
    [ICON_CLOUDY] = "A:/sdcard/weather/wi-cloudy_65.bin",
    [ICON_DAY_FOG] = "A:/sdcard/weather/wi-day-fog_65.bin",
    [ICON_FOG] = "A:/sdcard/weather/wi-fog_65.bin",
    [ICON_SPRINKLE] = "A:/sdcard/weather/wi-sprinkle_65.bin",
    [ICON_RAINDROP] = "A:/sdcard/weather/wi-raindrop_65.bin",
    [ICON_RAIN] = "A:/sdcard/weather/wi-rain_65.bin",
    [ICON_SHOWERS] = "A:/sdcard/weather/wi-showers_65.bin",
    [ICON_DAY_THUNDERSTORM] = "A:/sdcard/weather/wi-day-thunderstorm_65.bin",
    [ICON_NIGHT_ALT_THUNDERSTORM] = "A:/sdcard/weather/wi-night-alt-thunderstorm_65.bin",
};

static const char *s_icon_paths_144[ICON_COUNT] = {
    [ICON_NA] = "A:/sdcard/weather/wi-na_144.bin",
    [ICON_DAY_SUNNY] = "A:/sdcard/weather/wi-day-sunny_144.bin",
    [ICON_NIGHT_CLEAR] = "A:/sdcard/weather/wi-night-clear_144.bin",
    [ICON_DAY_CLOUDY] = "A:/sdcard/weather/wi-day-cloudy_144.bin",
    [ICON_NIGHT_ALT_CLOUDY] = "A:/sdcard/weather/wi-night-alt-cloudy_144.bin",
    [ICON_CLOUDY] = "A:/sdcard/weather/wi-cloudy_144.bin",
    [ICON_DAY_FOG] = "A:/sdcard/weather/wi-day-fog_144.bin",
    [ICON_FOG] = "A:/sdcard/weather/wi-fog_144.bin",
    [ICON_SPRINKLE] = "A:/sdcard/weather/wi-sprinkle_144.bin",
    [ICON_RAINDROP] = "A:/sdcard/weather/wi-raindrop_144.bin",
    [ICON_RAIN] = "A:/sdcard/weather/wi-rain_144.bin",
    [ICON_SHOWERS] = "A:/sdcard/weather/wi-showers_144.bin",
    [ICON_DAY_THUNDERSTORM] = "A:/sdcard/weather/wi-day-thunderstorm_144.bin",
    [ICON_NIGHT_ALT_THUNDERSTORM] = "A:/sdcard/weather/wi-night-alt-thunderstorm_144.bin",
};

static char response_buffer[8192];
static int response_len = 0;
static float current_temp = 0.0f;
static float current_humidity = 0.0f;
static float current_wind_speed = 0.0f;
static int current_weather_code = -1; // -1 indicates not yet fetched
static int current_is_day = 1;        // 1 = daytime, 0 = night

static weather_icon_id_t weather_icon_id_from_code(int code, int is_day)
{
    switch (code) {
        case 0:
            return is_day ? ICON_DAY_SUNNY : ICON_NIGHT_CLEAR;
        case 1:
        case 2:
            return is_day ? ICON_DAY_CLOUDY : ICON_NIGHT_ALT_CLOUDY;
        case 3:
            return ICON_CLOUDY;
        case 45:
        case 48:
            return is_day ? ICON_DAY_FOG : ICON_FOG;
        case 51:
        case 53:
        case 55:
            return ICON_SPRINKLE;
        case 56:
        case 57:
            return ICON_RAINDROP;
        case 61:
        case 63:
            return ICON_RAIN;
        case 65:
            return ICON_SHOWERS;
        case 66:
        case 67:
            return ICON_SHOWERS;
        case 71:
        case 73:
        case 75:
        case 77:
        case 85:
        case 86:
            return ICON_CLOUDY;
        case 80:
        case 81:
        case 82:
            return ICON_SHOWERS;
        case 95:
        case 96:
        case 99:
            return is_day ? ICON_DAY_THUNDERSTORM : ICON_NIGHT_ALT_THUNDERSTORM;
        default:
            return ICON_NA;
    }
}

static int parse_hour_from_iso8601(const char *iso_time)
{
    int hour = -1;
    if (iso_time == NULL) {
        return -1;
    }
    if (sscanf(iso_time, "%*d-%*d-%*dT%d", &hour) == 1 && hour >= 0 && hour <= 23) {
        return hour;
    }
    return -1;
}

static int slot_index_for_hour(int hour)
{
    for (int i = 0; i < WEATHER_MANAGER_SLOT_COUNT; i++) {
        if (s_slot_hours[i] == hour) {
            return i;
        }
    }
    return -1;
}

static void reset_hourly_slots(void)
{
    for (int i = 0; i < WEATHER_MANAGER_SLOT_COUNT; i++) {
        s_hourly_slots[i].hour_24 = s_slot_hours[i];
        s_hourly_slots[i].temp_c = 0.0f;
        s_hourly_slots[i].weather_code = -1;
        s_hourly_slots[i].is_day = 1;
        s_hourly_slots[i].valid = false;
    }
}

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

float weather_manager_get_humidity(void)
{
    return current_humidity;
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
    reset_hourly_slots();

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
                    cJSON *humidity_item = cJSON_GetObjectItemCaseSensitive(current, "relative_humidity_2m");
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
                    if (cJSON_IsNumber(humidity_item)) {
                        current_humidity = (float)humidity_item->valuedouble;
                        ESP_LOGI(TAG, "   HUMIDITY: %.0f%%", current_humidity);
                    }
                    if (cJSON_IsNumber(code_item)) {
                        current_weather_code = code_item->valueint;
                        ESP_LOGI(TAG, "   WEATHER CODE: %d", code_item->valueint);
                    }
                    if (cJSON_IsNumber(is_day_item)) {
                        current_is_day = is_day_item->valueint;
                        ESP_LOGI(TAG, "   IS_DAY: %d", current_is_day);
                    }

                    cJSON *hourly = cJSON_GetObjectItemCaseSensitive(json, "hourly");
                    if (hourly != NULL) {
                        cJSON *time_arr = cJSON_GetObjectItemCaseSensitive(hourly, "time");
                        cJSON *temp_arr = cJSON_GetObjectItemCaseSensitive(hourly, "temperature_2m");
                        cJSON *code_arr = cJSON_GetObjectItemCaseSensitive(hourly, "weather_code");
                        cJSON *is_day_arr = cJSON_GetObjectItemCaseSensitive(hourly, "is_day");

                        if (cJSON_IsArray(time_arr) && cJSON_IsArray(temp_arr) && cJSON_IsArray(code_arr)) {
                            int count = cJSON_GetArraySize(time_arr);
                            int count_temp = cJSON_GetArraySize(temp_arr);
                            int count_code = cJSON_GetArraySize(code_arr);
                            int count_is_day = cJSON_IsArray(is_day_arr) ? cJSON_GetArraySize(is_day_arr) : 0;
                            int min_count = count;
                            if (count_temp < min_count) {
                                min_count = count_temp;
                            }
                            if (count_code < min_count) {
                                min_count = count_code;
                            }

                            for (int i = 0; i < min_count; i++) {
                                cJSON *time_item = cJSON_GetArrayItem(time_arr, i);
                                cJSON *hour_temp_item = cJSON_GetArrayItem(temp_arr, i);
                                cJSON *hour_code_item = cJSON_GetArrayItem(code_arr, i);
                                cJSON *hour_day_item = (i < count_is_day) ? cJSON_GetArrayItem(is_day_arr, i) : NULL;

                                if (!cJSON_IsString(time_item) || !cJSON_IsNumber(hour_temp_item) || !cJSON_IsNumber(hour_code_item)) {
                                    continue;
                                }

                                int hour = parse_hour_from_iso8601(time_item->valuestring);
                                int slot_index = slot_index_for_hour(hour);
                                if (slot_index < 0 || s_hourly_slots[slot_index].valid) {
                                    continue;
                                }

                                s_hourly_slots[slot_index].hour_24 = hour;
                                s_hourly_slots[slot_index].temp_c = (float)hour_temp_item->valuedouble;
                                s_hourly_slots[slot_index].weather_code = hour_code_item->valueint;
                                s_hourly_slots[slot_index].is_day = cJSON_IsNumber(hour_day_item) ? hour_day_item->valueint : current_is_day;
                                s_hourly_slots[slot_index].valid = true;
                            }

                            for (int i = 0; i < WEATHER_MANAGER_SLOT_COUNT; i++) {
                                if (s_hourly_slots[i].valid) {
                                    ESP_LOGI(TAG, "   SLOT %02d: temp=%.1f code=%d is_day=%d",
                                             s_hourly_slots[i].hour_24,
                                             s_hourly_slots[i].temp_c,
                                             s_hourly_slots[i].weather_code,
                                             s_hourly_slots[i].is_day);
                                } else {
                                    ESP_LOGW(TAG, "   SLOT %02d: missing in hourly payload", s_slot_hours[i]);
                                }
                            }
                        } else {
                            ESP_LOGW(TAG, "Hourly JSON missing required arrays");
                        }
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
    return weather_manager_get_icon_path_from_code_size(code, current_is_day, WEATHER_ICON_SIZE_144);
}

const char* weather_manager_get_icon_path_from_code_size(int code, int is_day, int icon_size_px)
{
    weather_icon_id_t icon_id = weather_icon_id_from_code(code, is_day);
    if (icon_id < 0 || icon_id >= ICON_COUNT) {
        icon_id = ICON_NA;
    }
    if (icon_size_px == WEATHER_ICON_SIZE_65) {
        return s_icon_paths_65[icon_id];
    }
    return s_icon_paths_144[icon_id];
}

bool weather_manager_get_hourly_slot(int index, weather_manager_hourly_slot_t *slot_out)
{
    if (index < 0 || index >= WEATHER_MANAGER_SLOT_COUNT || slot_out == NULL) {
        return false;
    }

    *slot_out = s_hourly_slots[index];
    return s_hourly_slots[index].valid;
}