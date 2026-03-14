
#include "esp_log.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_types.h"
#include "esp_lcd_ili9341.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"

#define LCD_HOST    SPI2_HOST  //SPI Bus for TFT display
#define TFT_DMA_CH 1 // DMA channel for TFT SPI

#define PIN_NUM_MOSI 13
#define PIN_NUM_CLK  14
#define PIN_NUM_CS   15
#define PIN_NUM_DC   2
#define PIN_NUM_RST  -1
#define LCD_H_RES    320
#define LCD_V_RES    240
#define TFT_BL 27  // Backlight pin

static const char *TAG = "display";

esp_lcd_panel_handle_t panel_handle = NULL;
static esp_lcd_panel_io_handle_t panel_io_handle = NULL;

void display_init(void) {
    // Turn on backlight
    gpio_set_direction(TFT_BL, GPIO_MODE_OUTPUT);
    gpio_set_level(TFT_BL, 1);

    ESP_LOGI(TAG, "Initialize SPI bus");
    const spi_bus_config_t bus_config = ILI9341_PANEL_BUS_SPI_CONFIG(PIN_NUM_CLK, PIN_NUM_MOSI, LCD_H_RES * 80 * sizeof(uint16_t));
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_config, TFT_DMA_CH));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = ILI9341_PANEL_IO_SPI_CONFIG(PIN_NUM_CS, PIN_NUM_DC, NULL, NULL);
    io_config.pclk_hz = 20 * 1000 * 1000; // Lower to 20MHz to prevent "snow" interference
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &panel_io_handle));

    ESP_LOGI(TAG, "Install ILI9341 panel driver");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = PIN_NUM_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io_handle, &panel_config, &panel_handle));
    ESP_LOGI(TAG, "panel_handle address: %p", panel_handle);
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    // DONT TOUCH: this panel requires swap_xy=false; swap_xy=true causes a fixed right-side strip.
    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, false));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));
}

esp_lcd_panel_handle_t display_get_panel_handle(void) {
    return panel_handle;
}

esp_lcd_panel_io_handle_t display_get_panel_io_handle(void) {
    return panel_io_handle;
}

esp_err_t display_reinit_spi_bus(void) {
    const spi_bus_config_t bus_config = ILI9341_PANEL_BUS_SPI_CONFIG(
        PIN_NUM_CLK,
        PIN_NUM_MOSI,
        LCD_H_RES * 80 * sizeof(uint16_t)
    );
    return spi_bus_initialize(LCD_HOST, &bus_config, TFT_DMA_CH);
}

void display_set_orientation(bool swap_xy, bool mirror_x, bool mirror_y) {
    if (!panel_handle) {
        ESP_LOGE(TAG, "Panel handle is NULL in display_set_orientation");
        return;
    }

    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel_handle, swap_xy));
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, mirror_x, mirror_y));
    ESP_LOGI(TAG, "Orientation set: swap_xy=%d mirror_x=%d mirror_y=%d",
             swap_xy, mirror_x, mirror_y);
}

void display_fill_screen_rgb565(uint16_t color) {
    if (!panel_handle) {
        ESP_LOGE(TAG, "Panel handle is NULL in display_fill_screen_rgb565");
        return;
    }

    uint16_t *line_buf = heap_caps_malloc(LCD_H_RES * sizeof(uint16_t), MALLOC_CAP_DMA);
    if (!line_buf) {
        ESP_LOGE(TAG, "Failed to allocate line buffer for full-screen fill");
        return;
    }

    // ILI9341 SPI write path expects RGB565 bytes MSB first.
    const uint16_t tx_color = (uint16_t)((color << 8) | (color >> 8));

    for (int x = 0; x < LCD_H_RES; x++) {
        line_buf[x] = tx_color;
    }

    for (int y = 0; y < LCD_V_RES; y++) {
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, y, LCD_H_RES, y + 1, line_buf));
    }

    free(line_buf);
}

void display_test(void) {
    ESP_LOGI(TAG, "panel_handle in test: %p", panel_handle);
    ESP_LOGI(TAG, "Free heap before allocation: %u", esp_get_free_heap_size());
    // Draw a small white rectangle at top left
    int rect_w = 80, rect_h = 40;
    uint16_t *rect = calloc(rect_w * rect_h, sizeof(uint16_t));
    if (rect) {
        for (int i = 0; i < rect_w * rect_h; ++i) rect[i] = 0xFFFF; // White in RGB565
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel_handle, 0, 0, rect_w, rect_h, rect));
        ESP_LOGI(TAG, "Rectangle draw command sent");
        vTaskDelay(pdMS_TO_TICKS(1000)); // Wait 1 second to ensure display update
        free(rect);
        ESP_LOGI(TAG, "Test rectangle drawn");
    } else {
        ESP_LOGE(TAG, "Failed to allocate small rectangle framebuffer");
    }
    ESP_LOGI(TAG, "Free heap after allocation: %u", esp_get_free_heap_size());
}
