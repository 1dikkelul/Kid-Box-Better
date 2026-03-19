#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "sd_card.h"
#include "time.h"
#include "lvgl.h"
#include "display.h"
#include "ui.h"
#include "lvgl_setup.h"
#include "time_manager.h"
#include "weather_manager.h"
#include "file_server.h"
#include "boot_video.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#define FILE_SERVER_TASK_STACK 4096
#define WEATHER_BOOT_FETCH_MAX_ATTEMPTS 5
#define WEATHER_BOOT_FETCH_RETRY_MS 3000
#define BOOT_VIDEO_PATH "/sdcard/intro.rlv"
#define WEATHER_ICON_NOW_BOOT_PATH "A:/sdcard/weather/wi-day-sunny_144.bin"

char ip_str[16];

static bool s_sd_available = false;

// SquareLine will export this symbol once an Image widget is named "WeatherIconNow".
// Keep it weak so builds still succeed before that UI element exists.
extern lv_obj_t * ui_WeatherIconNow __attribute__((weak));
extern lv_obj_t * ui_WeatherIcon __attribute__((weak));
extern lv_obj_t * ui_WindNow __attribute__((weak));
extern lv_obj_t * ui_TextArea2 __attribute__((weak));
extern lv_obj_t * ui_TextArea5 __attribute__((weak));

static void heap_alloc_failed_cb(size_t requested_size, uint32_t caps, const char *function_name)
{
    ESP_LOGE("HEAP_OOM", "Alloc FAILED: %u bytes (caps=0x%08lX) in %s | free=%lu largest_block=%lu",
             (unsigned)requested_size,
             (unsigned long)caps,
             function_name ? function_name : "unknown",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void file_server_auto_start_task(void *arg);
static void weather_update_task(void *arg);
static void ui_set_boot_weather_icon_now(void);
static void ui_apply_weather_data_locked(void);
static void ui_set_textarea_value(lv_obj_t *obj, const char *text);
static void format_temp_c(char *out, size_t out_len, float temp_c);
static void format_humidity_percent(char *out, size_t out_len, float humidity);
static void format_wind_kmh(char *out, size_t out_len, float wind_kmh);
static void format_today_day(char *out, size_t out_len);
static lv_obj_t *resolve_forecast_times_obj(void);

static lv_obj_t *resolve_weather_icon_now_obj(void)
{
    if (&ui_WeatherIconNow == NULL) {
        return NULL;
    }
    return ui_WeatherIconNow;
}

static lv_obj_t *resolve_legacy_weather_icon_obj(void)
{
    if (&ui_WeatherIcon == NULL) {
        return NULL;
    }
    return ui_WeatherIcon;
}

static lv_obj_t *resolve_forecast_times_obj(void)
{
    if (&ui_TextArea2 != NULL && ui_TextArea2 != NULL) {
        return ui_TextArea2;
    }
    if (&ui_TextArea5 != NULL && ui_TextArea5 != NULL) {
        return ui_TextArea5;
    }
    return NULL;
}

static void nvs_init_or_recover(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
}

static void weather_update_task(void *arg)
{
    (void)arg;
    ESP_LOGI("weather_task", "Boot fetch: waiting for WiFi...");

    // Wait until WiFi is actually connected and has an IP
    while (!wifi_manager_is_ready()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI("weather_task", "Boot fetch: WiFi connected. Waiting for SNTP time sync...");
    // Time must be synced before we can make HTTPS requests.
    while (!time_manager_wait_for_sync(10000)) {
        ESP_LOGW("weather_task", "SNTP time not synced yet. Retrying in 10s...");
    }
    ESP_LOGI("weather_task", "Boot fetch: SNTP synced. Fetching weather with retries.");

    bool fetch_ok = false;
    for (int attempt = 1; attempt <= WEATHER_BOOT_FETCH_MAX_ATTEMPTS; attempt++) {
        ESP_LOGI("weather_task", "Boot fetch attempt %d/%d", attempt, WEATHER_BOOT_FETCH_MAX_ATTEMPTS);
        fetch_ok = weather_manager_fetch();
        if (fetch_ok) {
            break;
        }
        if (attempt < WEATHER_BOOT_FETCH_MAX_ATTEMPTS) {
            ESP_LOGW("weather_task", "Boot fetch failed, retrying in %d ms", WEATHER_BOOT_FETCH_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(WEATHER_BOOT_FETCH_RETRY_MS));
        }
    }

    if (!fetch_ok) {
        ESP_LOGE("weather_task", "Boot fetch failed after %d attempts", WEATHER_BOOT_FETCH_MAX_ATTEMPTS);
    } else {
        if (lvgl_setup_lock(2000)) {
            ui_apply_weather_data_locked();
            lvgl_setup_unlock();
            ESP_LOGI("weather_task", "UI weather values applied");
        } else {
            ESP_LOGW("weather_task", "Could not lock LVGL to apply weather values");
        }
    }

    // Weather data is cached by weather_manager and consumed by UI when available.
    ESP_LOGI("weather_task", "Boot fetch complete");
    vTaskDelete(NULL);
}

static void format_temp_c(char *out, size_t out_len, float temp_c)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    int rounded = (temp_c >= 0.0f) ? (int)(temp_c + 0.5f) : (int)(temp_c - 0.5f);
    snprintf(out, out_len, "%d\xC2\xB0" "C", rounded);
}

static void format_humidity_percent(char *out, size_t out_len, float humidity)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    int rounded = (humidity >= 0.0f) ? (int)(humidity + 0.5f) : (int)(humidity - 0.5f);
    if (rounded < 0) {
        rounded = 0;
    }
    snprintf(out, out_len, "%d%%", rounded);
}

static void format_wind_kmh(char *out, size_t out_len, float wind_kmh)
{
    if (out == NULL || out_len == 0) {
        return;
    }
    int rounded = (wind_kmh >= 0.0f) ? (int)(wind_kmh + 0.5f) : (int)(wind_kmh - 0.5f);
    snprintf(out, out_len, "%d km/h", rounded);
}

static void format_today_day(char *out, size_t out_len)
{
    if (out == NULL || out_len == 0) {
        return;
    }

    time_t now = time(NULL);
    struct tm tm_now;
    if (now <= 0 || localtime_r(&now, &tm_now) == NULL) {
        snprintf(out, out_len, "TODAY");
        return;
    }

    if (strftime(out, out_len, "%A", &tm_now) == 0) {
        snprintf(out, out_len, "TODAY");
    }
}

static void ui_set_textarea_value(lv_obj_t *obj, const char *text)
{
    if (obj == NULL || !lv_obj_is_valid(obj) || text == NULL) {
        return;
    }

    lv_textarea_set_text(obj, text);

    // SquareLine uses textareas for read-only values here. Reset the cursor so
    // the visible viewport stays at the start of the string after updates.
    lv_textarea_set_cursor_pos(obj, 0);
}

static void ui_apply_weather_data_locked(void)
{
    const float current_temp = weather_manager_get_temp();
    const float current_humidity = weather_manager_get_humidity();
    const float current_wind_speed = weather_manager_get_wind_speed();
    const int current_code = weather_manager_get_weather_code();
    const int current_is_day = weather_manager_get_is_day();

    char value_buf[24];

    if (ui_TempSensor != NULL && lv_obj_is_valid(ui_TempSensor)) {
        format_temp_c(value_buf, sizeof(value_buf), current_temp);
        ui_set_textarea_value(ui_TempSensor, value_buf);
    }

    if (ui_TodayDay != NULL && lv_obj_is_valid(ui_TodayDay)) {
        format_today_day(value_buf, sizeof(value_buf));
        ui_set_textarea_value(ui_TodayDay, value_buf);
    }

    if (ui_HumiditySensor != NULL && lv_obj_is_valid(ui_HumiditySensor)) {
        format_humidity_percent(value_buf, sizeof(value_buf), current_humidity);
        ui_set_textarea_value(ui_HumiditySensor, value_buf);
    }

    if (&ui_WindNow != NULL && ui_WindNow != NULL && lv_obj_is_valid(ui_WindNow)) {
        format_wind_kmh(value_buf, sizeof(value_buf), current_wind_speed);
        ui_set_textarea_value(ui_WindNow, value_buf);
    }

    if (s_sd_available && ui_WeatherIconNow != NULL && lv_obj_is_valid(ui_WeatherIconNow)) {
        const char *icon_now = weather_manager_get_icon_path_from_code_size(current_code, current_is_day, 144);
        lv_image_set_src(ui_WeatherIconNow, icon_now);
        lv_obj_set_size(ui_WeatherIconNow, 144, 144);
    }

    lv_obj_t *hour_icon_objs[WEATHER_MANAGER_SLOT_COUNT] = {
        ui_Weather7am,
        ui_Weather10am,
        ui_Weather1pm,
        ui_Weather4pm,
        ui_Weather7pm,
    };
    lv_obj_t *hour_temp_objs[WEATHER_MANAGER_SLOT_COUNT] = {
        ui_Temp7am,
        ui_Temp10am,
        ui_Temp1pm,
        ui_Temp4pm,
        ui_Temp7pm,
    };

    for (int i = 0; i < WEATHER_MANAGER_SLOT_COUNT; i++) {
        weather_manager_hourly_slot_t slot;
        bool slot_valid = weather_manager_get_hourly_slot(i, &slot);

        if (hour_temp_objs[i] != NULL && lv_obj_is_valid(hour_temp_objs[i])) {
            if (slot_valid) {
                format_temp_c(value_buf, sizeof(value_buf), slot.temp_c);
            } else {
                snprintf(value_buf, sizeof(value_buf), "--");
            }
            ui_set_textarea_value(hour_temp_objs[i], value_buf);
        }

        if (s_sd_available && hour_icon_objs[i] != NULL && lv_obj_is_valid(hour_icon_objs[i])) {
            const char *icon_path = slot_valid
                                        ? weather_manager_get_icon_path_from_code_size(slot.weather_code, slot.is_day, 65)
                                        : "A:/sdcard/weather/wi-na_65.bin";
            lv_image_set_src(hour_icon_objs[i], icon_path);
            lv_obj_set_size(hour_icon_objs[i], 65, 65);
        }
    }

    lv_obj_t *forecast_times = resolve_forecast_times_obj();
    if (forecast_times != NULL && lv_obj_is_valid(forecast_times)) {
        lv_obj_clear_flag(forecast_times, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(forecast_times);
    }
}

// File server starts automatically once WiFi is ready.
static void file_server_auto_start_task(void *arg)
{
    (void)arg;
    ESP_LOGI("file_server", "Waiting for WiFi before starting file server...");
    while (!wifi_manager_is_ready()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI("file_server", "WiFi ready -- starting file server at /sdcard");
    if (start_file_server("/sdcard") != ESP_OK) {
        ESP_LOGE("file_server", "Failed to start file server");
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    printf("\n\n=== KIDSBOX RLCD BUILD %s %s ===\n\n", __DATE__, __TIME__);

    nvs_init_or_recover();
    heap_caps_register_failed_alloc_callback(heap_alloc_failed_cb);

    wifi_manager_start_sta();
    printf("WiFi init done\n");

    time_manager_init();

    esp_err_t sd_status = sd_card_mount();
    printf("SD Card status: %s\n", esp_err_to_name(sd_status));
    s_sd_available = (sd_status == ESP_OK);
    if (!s_sd_available) {
        ESP_LOGW("app_main", "SD not mounted -- file server unavailable");
    }

    display_init();
    printf("Display init done\n");

    if (s_sd_available) {
        esp_err_t boot_video_status = boot_video_play_from_file(BOOT_VIDEO_PATH);
        if (boot_video_status != ESP_OK) {
            ESP_LOGW("app_main", "Boot video skipped (%s): %s",
                     BOOT_VIDEO_PATH, esp_err_to_name(boot_video_status));
        }

        // Ensure no stale pixels from the last video frame remain before LVGL starts drawing.
        display_fill_screen(DISPLAY_COLOR_WHITE);
    }

    wifi_manager_status_t st = wifi_manager_get_status();
    printf("WiFi status: started=%d connected=%d got_ip=%d\n",
           st.started, st.connected, st.got_ip);

    lvgl_setup_start();

    if (lvgl_setup_lock(-1)) {
        ui_init();
        ui_set_boot_weather_icon_now();
        lvgl_setup_unlock();
    }

    // File server runs continuously when SD is available (PSRAM allows it).
    if (s_sd_available) {
        xTaskCreate(file_server_auto_start_task, "file_server", FILE_SERVER_TASK_STACK, NULL, 4, NULL);
    }

    xTaskCreate(weather_update_task, "weather_task", 8192, NULL, 5, NULL);

    // Keep app task alive; LVGL timer is driven by lvgl_task inside lvgl_setup.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void ui_set_boot_weather_icon_now(void)
{
    if (!s_sd_available) {
        ESP_LOGW("app_main", "Skipping WeatherIconNow boot icon: SD not mounted");
        return;
    }

    lv_obj_t *weather_icon_now = resolve_weather_icon_now_obj();
    lv_obj_t *legacy_weather_icon = resolve_legacy_weather_icon_obj();

    if (weather_icon_now != NULL && lv_obj_is_valid(weather_icon_now)) {
        lv_image_header_t icon_header;
        lv_result_t icon_info = lv_image_decoder_get_info(WEATHER_ICON_NOW_BOOT_PATH, &icon_header);
        if (icon_info == LV_RESULT_OK) {
            ESP_LOGI("app_main", "Boot icon info: %s -> %ldx%ld", WEATHER_ICON_NOW_BOOT_PATH,
                     (long)icon_header.w, (long)icon_header.h);
        } else {
            ESP_LOGW("app_main", "Boot icon info unavailable: %s", WEATHER_ICON_NOW_BOOT_PATH);
        }

        // Force predictable image behavior: no tiling, no zoom, visible object.
        lv_obj_clear_flag(weather_icon_now, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(weather_icon_now, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_opa(weather_icon_now, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(weather_icon_now, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_outline_opa(weather_icon_now, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_shadow_opa(weather_icon_now, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_image_opa(weather_icon_now, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_image_recolor_opa(weather_icon_now, LV_OPA_TRANSP, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_blend_mode(weather_icon_now, LV_BLEND_MODE_NORMAL, LV_PART_MAIN | LV_STATE_DEFAULT);
        
        // VISIBILITY DEBUG: Force a black border around the icon widget
        // This proves the widget is positioned and visible, even if the image data is "invisible"
        lv_obj_set_style_border_width(weather_icon_now, 2, LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_color(weather_icon_now, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
        lv_obj_set_style_border_opa(weather_icon_now, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

        lv_image_set_inner_align(weather_icon_now, LV_IMAGE_ALIGN_CENTER);
        lv_image_set_scale(weather_icon_now, LV_SCALE_NONE);
        lv_image_set_rotation(weather_icon_now, 0);
        lv_image_set_src(weather_icon_now, WEATHER_ICON_NOW_BOOT_PATH);

        // Keep the original SquareLine position/align instead of forcing center.

        if (icon_info == LV_RESULT_OK) {
            lv_obj_set_size(weather_icon_now, icon_header.w, icon_header.h);
        } else {
            lv_obj_set_size(weather_icon_now, 144, 144);
        }

        lv_obj_invalidate(lv_screen_active());

        if (ui_Screen1) {
            // Hide stale/duplicate image widgets if they overlap this icon position.
            lv_obj_update_layout(ui_Screen1);
            int32_t tx = lv_obj_get_x_aligned(weather_icon_now);
            int32_t ty = lv_obj_get_y_aligned(weather_icon_now);

            uint32_t child_cnt = lv_obj_get_child_cnt(ui_Screen1);
            for (uint32_t i = 0; i < child_cnt; i++) {
                lv_obj_t *child = lv_obj_get_child(ui_Screen1, i);

                if (child == weather_icon_now) {
                    continue;
                }

                if (legacy_weather_icon != NULL && child == legacy_weather_icon) {
                    lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
                    ESP_LOGW("app_main", "Hidden legacy ui_WeatherIcon");
                    continue;
                }

                if (!lv_obj_has_class(child, &lv_image_class)) {
                    continue;
                }

                int32_t cx = lv_obj_get_x_aligned(child);
                int32_t cy = lv_obj_get_y_aligned(child);
                if (abs(cx - tx) <= 2 && abs(cy - ty) <= 2) {
                    lv_obj_add_flag(child, LV_OBJ_FLAG_HIDDEN);
                    ESP_LOGW("app_main", "Hidden overlapping image duplicate at (%ld,%ld)",
                             (long)cx, (long)cy);
                }
            }
        }

        lv_obj_t *forecast_times = resolve_forecast_times_obj();
        if (forecast_times != NULL && lv_obj_is_valid(forecast_times)) {
            lv_obj_clear_flag(forecast_times, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(forecast_times);
        }

        ESP_LOGI("app_main", "WeatherIconNow set to %s", WEATHER_ICON_NOW_BOOT_PATH);
        return;
    }

    ESP_LOGW("app_main", "Skipping weather icon boot image: ui_WeatherIconNow not found");
}
