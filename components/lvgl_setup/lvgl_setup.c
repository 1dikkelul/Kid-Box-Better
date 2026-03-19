#include "lvgl.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include <assert.h>

#include "../display/display.h"
#include "lvgl_setup.h"

#define LVGL_TICK_PERIOD_MS    5
#define LVGL_TASK_MAX_DELAY_MS 500
#define LVGL_TASK_MIN_DELAY_MS 10

static const char *TAG = "lvgl_setup";
static uint32_t s_flush_debug_count = 0;

// Mutex exposed via lvgl_setup_lock / lvgl_setup_unlock
static SemaphoreHandle_t s_lvgl_mux = NULL;

// ---- LVGL tick (esp_timer based, avoids dedicated task overhead) ----

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(LVGL_TICK_PERIOD_MS);
}

// ---- Flush callback ----
// LVGL renders into an RGB565 full-screen buffer.
// Each pixel is thresholded to black/white and written into the ST7305 frame buffer,
// then the entire frame buffer is pushed to the panel.

static void my_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    uint32_t dark_count = 0;
    int32_t dark_min_x = LCD_H_RES;
    int32_t dark_min_y = LCD_V_RES;
    int32_t dark_max_x = -1;
    int32_t dark_max_y = -1;

    if (s_flush_debug_count < 16) {
        uint32_t w = (uint32_t)(area->x2 - area->x1 + 1);
        uint32_t h = (uint32_t)(area->y2 - area->y1 + 1);
        ESP_LOGI(TAG, "Flush[%lu]: area=(%ld,%ld)-(%ld,%ld) size=%lux%lu px_map=%p",
                 (unsigned long)s_flush_debug_count,
                 (long)area->x1, (long)area->y1,
                 (long)area->x2, (long)area->y2,
                 (unsigned long)w, (unsigned long)h,
                 px_map);
    }

    const uint16_t *buf = (const uint16_t *)px_map;
    for (int y = area->y1; y <= area->y2; y++) {
        for (int x = area->x1; x <= area->x2; x++) {
            // Pixels above mid-gray → white, below → black
            bool is_dark = (*buf < 0x7FFFu);
            uint8_t color = is_dark ? DISPLAY_COLOR_BLACK : DISPLAY_COLOR_WHITE;
            if (s_flush_debug_count < 16 && is_dark) {
                dark_count++;
                if (x < dark_min_x) dark_min_x = x;
                if (y < dark_min_y) dark_min_y = y;
                if (x > dark_max_x) dark_max_x = x;
                if (y > dark_max_y) dark_max_y = y;
            }
            display_set_pixel((uint16_t)x, (uint16_t)y, color);
            buf++;
        }
    }
    display_flush();
    if (s_flush_debug_count < 16) {
        if (dark_count > 0) {
            ESP_LOGI(TAG, "Flush[%lu]: dark_pixels=%lu dark_bbox=(%ld,%ld)-(%ld,%ld)",
                     (unsigned long)s_flush_debug_count,
                     (unsigned long)dark_count,
                     (long)dark_min_x, (long)dark_min_y,
                     (long)dark_max_x, (long)dark_max_y);
        } else {
            ESP_LOGI(TAG, "Flush[%lu]: dark_pixels=0", (unsigned long)s_flush_debug_count);
        }
        s_flush_debug_count++;
    }
    lv_display_flush_ready(disp);
}

// ---- LVGL task ----

static void lvgl_port_task(void *arg)
{
    (void)arg;
    uint32_t delay_ms = LVGL_TASK_MAX_DELAY_MS;
    for (;;) {
        if (xSemaphoreTake(s_lvgl_mux, portMAX_DELAY) == pdTRUE) {
            delay_ms = lv_timer_handler();
            xSemaphoreGive(s_lvgl_mux);
        }
        if (delay_ms > LVGL_TASK_MAX_DELAY_MS) delay_ms = LVGL_TASK_MAX_DELAY_MS;
        if (delay_ms < LVGL_TASK_MIN_DELAY_MS) delay_ms = LVGL_TASK_MIN_DELAY_MS;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

// ---- Public API ----

bool lvgl_setup_lock(int timeout_ms)
{
    const TickType_t ticks = (timeout_ms < 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(s_lvgl_mux, ticks) == pdTRUE;
}

void lvgl_setup_unlock(void)
{
    xSemaphoreGive(s_lvgl_mux);
}

void lvgl_setup_start(void)
{
    s_lvgl_mux = xSemaphoreCreateMutex();
    assert(s_lvgl_mux);

    lv_init();

    // LVGL tick via esp_timer (no extra task needed)
    esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_timer, LVGL_TICK_PERIOD_MS * 1000));

    // Full-screen double buffers in SPIRAM
    // Each buffer: LCD_H_RES * LCD_V_RES * 2 bytes (RGB565) = 400*300*2 = 240 KB
    const size_t buf_size = (size_t)LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    uint8_t *buf1 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    uint8_t *buf2 = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!buf1 || !buf2) {
        ESP_LOGE(TAG, "Failed to allocate LVGL frame buffers in SPIRAM");
        assert(false);
    }

    lv_display_t *disp = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, buf1, buf2, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_default(disp);

    lv_obj_clean(lv_screen_active());

    // LVGL runs in its own task (pinned to core 0, matches Waveshare reference)
    xTaskCreatePinnedToCore(lvgl_port_task, "lvgl_task", 8 * 1024, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "LVGL ready: %dx%d RGB565→monochrome, full-screen SPIRAM buffers", LCD_H_RES, LCD_V_RES);
}
