#include "lvgl.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_types.h"
#include "esp_lcd_ili9341.h"
#include "esp_heap_caps.h"

#include "../display/display.h"

#define LVGL_TICK_PERIOD_MS 5
#define LV_BUF_WIDTH 320
#define LV_BUF_HEIGHT 40
#define LV_SCREEN_WIDTH 320
#define LV_SCREEN_HEIGHT 240

static const char *TAG = "lvgl_setup";

// Global panel handle for flush callback
static esp_lcd_panel_handle_t panel_handle = NULL;
static SemaphoreHandle_t flush_done_sem = NULL;

static bool panel_io_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                      esp_lcd_panel_io_event_data_t *edata,
                                      void *user_ctx)
{
    (void)panel_io;
    (void)edata;
    (void)user_ctx;
    BaseType_t high_task_wakeup = pdFALSE;
    xSemaphoreGiveFromISR(flush_done_sem, &high_task_wakeup);
    return high_task_wakeup == pdTRUE;
}

static void lv_tick_task(void *arg) {
    while (1) {
        lv_tick_inc(LVGL_TICK_PERIOD_MS);
        vTaskDelay(pdMS_TO_TICKS(LVGL_TICK_PERIOD_MS));
    }
}

static void my_flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
    int x1 = area->x1;
    int y1 = area->y1;
    int x2 = area->x2;
    int y2 = area->y2;

    // Ignore out-of-range areas and clip partially visible regions.
    if (x2 < 0 || y2 < 0 || x1 > (LV_SCREEN_WIDTH - 1) || y1 > (LV_SCREEN_HEIGHT - 1)) {
        lv_display_flush_ready(display);
        return;
    }
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > (LV_SCREEN_WIDTH - 1)) x2 = LV_SCREEN_WIDTH - 1;
    if (y2 > (LV_SCREEN_HEIGHT - 1)) y2 = LV_SCREEN_HEIGHT - 1;

    // Send RGB565 data directly; avoid in-place byte swapping artifacts.
    esp_lcd_panel_draw_bitmap(panel_handle, x1, y1, x2 + 1, y2 + 1, (void *)px_map);
    if (xSemaphoreTake(flush_done_sem, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "Flush wait timed out (x1=%d y1=%d x2=%d y2=%d)", x1, y1, x2, y2);
    }
    lv_display_flush_ready(display);
}

void lvgl_setup_start(void) {
    lv_init();
    xTaskCreate(lv_tick_task, "lv_tick_task", 2048, NULL, 1, NULL);

    // Get the panel handle from display
    panel_handle = display_get_panel_handle();
    esp_lcd_panel_io_handle_t io_handle = display_get_panel_io_handle();

    flush_done_sem = xSemaphoreCreateBinary();
    if (!flush_done_sem) {
        ESP_LOGE(TAG, "Failed to create flush semaphore");
        return;
    }

    lv_color_t *buf1 = heap_caps_malloc(LV_BUF_WIDTH * LV_BUF_HEIGHT * sizeof(lv_color_t), MALLOC_CAP_DMA);
    if (!buf1) {
        ESP_LOGE(TAG, "Failed to allocate LVGL buffer");
        return;
    }

    lv_display_t *display = lv_display_create(LV_SCREEN_WIDTH, LV_SCREEN_HEIGHT);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buf1, NULL, LV_BUF_WIDTH * LV_BUF_HEIGHT * sizeof(lv_color_t), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, my_flush_cb);

    const esp_lcd_panel_io_callbacks_t cbs = {
        .on_color_trans_done = panel_io_color_trans_done,
    };
    ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(io_handle, &cbs, NULL));

    // LVGL 9: Set as default display
    lv_display_set_default(display);

    // Remove any leftover demo/test UI from the screen
    lv_obj_clean(lv_screen_active());

    ESP_LOGI(TAG, "LVGL display driver and buffers initialized");
   
}
