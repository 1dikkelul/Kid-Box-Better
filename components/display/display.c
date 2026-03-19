
#include "display.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <assert.h>
#include <string.h>

// ---- Pins (ESP32-S3-RLCD-4.2 board) ----
#define LCD_HOST       SPI3_HOST
#define RLCD_MOSI_PIN  12
#define RLCD_SCK_PIN   11
#define RLCD_DC_PIN    5
#define RLCD_CS_PIN    40
#define RLCD_RST_PIN   41

// ---- Frame buffer ----
// ST7305 is 1-bit monochrome. Each byte packs 8 sub-pixels in a 4x2 block.
// Buffer size = (400/4) * (300/2) = 100 * 150 = 15,000 bytes.
#define DISP_BUF_LEN   ((LCD_H_RES / 4) * (LCD_V_RES / 2))
#define INPUT_MONO_FRAME_LEN ((LCD_H_RES * LCD_V_RES) / 8)

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_panel_io = NULL;

// Frame buffer lives in SPIRAM
static uint8_t *s_disp_buf = NULL;

// LUT for fast pixel address lookup (also in SPIRAM)
// Indexed as [x * LCD_V_RES + y]
static uint16_t *s_pixel_index_lut = NULL;  // 400*300*2 = 240 KB
static uint8_t  *s_pixel_bit_lut   = NULL;  // 400*300*1 = 120 KB

// ---- Low-level SPI helpers ----

static void rlcd_send_cmd(uint8_t cmd)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_panel_io, cmd, NULL, 0));
}

static void rlcd_send_data(uint8_t data)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_panel_io, -1, &data, 1));
}

static void rlcd_send_buf(uint8_t *data, int len)
{
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_color(s_panel_io, -1, data, len));
}

// ---- Hardware reset ----

static void rlcd_reset(void)
{
    gpio_set_level(RLCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(RLCD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(RLCD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

// ---- LUT init (landscape orientation used by this panel init) ----
// Pixel packing for this display (landscape, 400 wide x 300 tall):
// Each byte holds a 2x4 block with Y mirrored in hardware scan order.
// byte_x  = x / 2
// block_y = (LCD_V_RES - 1 - y) / 4
// index   = byte_x * (LCD_V_RES/4) + block_y
// bit     = 7 - (((LCD_V_RES - 1 - y) % 4) * 2 + (x % 2))

static void init_pixel_lut(void)
{
    const uint16_t H4 = LCD_V_RES / 4; // = 75
    for (uint16_t x = 0; x < LCD_H_RES; x++) {
        uint16_t byte_x  = x >> 1;
        uint8_t  local_x = x & 0x01;
        for (uint16_t y = 0; y < LCD_V_RES; y++) {
            uint16_t inv_y   = (uint16_t)(LCD_V_RES - 1u - y);
            uint16_t block_y = inv_y >> 2;
            uint8_t  local_y = inv_y & 0x03;
            uint32_t index   = (uint32_t)byte_x * H4 + block_y;
            uint8_t  bit     = 7u - ((local_y << 1) | local_x);
            s_pixel_index_lut[(uint32_t)x * LCD_V_RES + y] = (uint16_t)index;
            s_pixel_bit_lut  [(uint32_t)x * LCD_V_RES + y] = (uint8_t)(1u << bit);
        }
    }
}

// ---- ST7305 init sequence (from Waveshare reference, landscape mode) ----

static void rlcd_panel_init(void)
{
    rlcd_reset();

    rlcd_send_cmd(0xD6);
    rlcd_send_data(0x17);
    rlcd_send_data(0x02);

    rlcd_send_cmd(0xD1);
    rlcd_send_data(0x01);

    rlcd_send_cmd(0xC0);
    rlcd_send_data(0x11);
    rlcd_send_data(0x04);

    rlcd_send_cmd(0xC1);
    rlcd_send_data(0x69); rlcd_send_data(0x69);
    rlcd_send_data(0x69); rlcd_send_data(0x69);

    rlcd_send_cmd(0xC2);
    rlcd_send_data(0x19); rlcd_send_data(0x19);
    rlcd_send_data(0x19); rlcd_send_data(0x19);

    rlcd_send_cmd(0xC4);
    rlcd_send_data(0x4B); rlcd_send_data(0x4B);
    rlcd_send_data(0x4B); rlcd_send_data(0x4B);

    rlcd_send_cmd(0xC5);
    rlcd_send_data(0x19); rlcd_send_data(0x19);
    rlcd_send_data(0x19); rlcd_send_data(0x19);

    rlcd_send_cmd(0xD8);
    rlcd_send_data(0x80);
    rlcd_send_data(0xE9);

    rlcd_send_cmd(0xB2);
    rlcd_send_data(0x02);

    rlcd_send_cmd(0xB3);
    rlcd_send_data(0xE5); rlcd_send_data(0xF6); rlcd_send_data(0x05);
    rlcd_send_data(0x46); rlcd_send_data(0x77); rlcd_send_data(0x77);
    rlcd_send_data(0x77); rlcd_send_data(0x77); rlcd_send_data(0x76);
    rlcd_send_data(0x45);

    rlcd_send_cmd(0xB4);
    rlcd_send_data(0x05); rlcd_send_data(0x46); rlcd_send_data(0x77);
    rlcd_send_data(0x77); rlcd_send_data(0x77); rlcd_send_data(0x77);
    rlcd_send_data(0x76); rlcd_send_data(0x45);

    rlcd_send_cmd(0x62);
    rlcd_send_data(0x32); rlcd_send_data(0x03); rlcd_send_data(0x1F);

    rlcd_send_cmd(0xB7);
    rlcd_send_data(0x13);

    rlcd_send_cmd(0xB0);
    rlcd_send_data(0x64);

    rlcd_send_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(200));

    rlcd_send_cmd(0xC9);
    rlcd_send_data(0x00);

    rlcd_send_cmd(0x36);
    rlcd_send_data(0x48);   // landscape orientation, no mirror/flip

    rlcd_send_cmd(0x3A);
    rlcd_send_data(0x11);

    rlcd_send_cmd(0xB9);
    rlcd_send_data(0x20);

    rlcd_send_cmd(0xB8);
    rlcd_send_data(0x29);

    rlcd_send_cmd(0x21);    // display inversion on (needed for this physical panel)

    rlcd_send_cmd(0x2A);    // column address: 0x12..0x2A = cols 18..42 (25 x 16px = 400)
    rlcd_send_data(0x12);
    rlcd_send_data(0x2A);

    rlcd_send_cmd(0x2B);    // page address: 0x00..0xC7 = rows 0..199 (200 x 2-row = 300?)
    rlcd_send_data(0x00);
    rlcd_send_data(0xC7);

    rlcd_send_cmd(0x35);
    rlcd_send_data(0x00);

    rlcd_send_cmd(0xD0);
    rlcd_send_data(0xFF);

    rlcd_send_cmd(0x38);
    rlcd_send_cmd(0x29);    // display on
}

// ---- Public API ----

void display_init(void)
{
    ESP_LOGI(TAG, "Initialising ST7305 RLCD 400x300");

    // Configure RST GPIO
    const gpio_config_t rst_cfg = {
        .pin_bit_mask  = (1ULL << RLCD_RST_PIN),
        .mode          = GPIO_MODE_OUTPUT,
        .pull_up_en    = GPIO_PULLUP_ENABLE,
        .pull_down_en  = GPIO_PULLDOWN_DISABLE,
        .intr_type     = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&rst_cfg));
    gpio_set_level(RLCD_RST_PIN, 1);

    // Allocate SPIRAM buffers
    s_disp_buf = heap_caps_malloc(DISP_BUF_LEN, MALLOC_CAP_SPIRAM);
    assert(s_disp_buf && "SPIRAM frame buffer alloc failed");

    s_pixel_index_lut = heap_caps_malloc((uint32_t)LCD_H_RES * LCD_V_RES * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    s_pixel_bit_lut   = heap_caps_malloc((uint32_t)LCD_H_RES * LCD_V_RES * sizeof(uint8_t),  MALLOC_CAP_SPIRAM);
    assert(s_pixel_index_lut && s_pixel_bit_lut && "SPIRAM pixel LUT alloc failed");
    init_pixel_lut();
    ESP_LOGI(TAG, "Pixel LUT initialised");

    // Init SPI bus (SPI3_HOST, no MISO needed)
    const spi_bus_config_t bus_cfg = {
        .mosi_io_num     = RLCD_MOSI_PIN,
        .miso_io_num     = -1,
        .sclk_io_num     = RLCD_SCK_PIN,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = DISP_BUF_LEN,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    // Init panel IO
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num        = RLCD_DC_PIN,
        .cs_gpio_num        = RLCD_CS_PIN,
        .pclk_hz            = 10 * 1000 * 1000,
        .lcd_cmd_bits       = 8,
        .lcd_param_bits     = 8,
        .spi_mode           = 0,
        .trans_queue_depth  = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &s_panel_io));

    // Run ST7305 init sequence
    rlcd_panel_init();

    // Start with a white screen
    display_fill_screen(DISPLAY_COLOR_WHITE);

    ESP_LOGI(TAG, "ST7305 display ready");
}

void display_set_pixel(uint16_t x, uint16_t y, uint8_t color)
{
    uint32_t lut_idx = (uint32_t)x * LCD_V_RES + y;
    uint32_t buf_idx = s_pixel_index_lut[lut_idx];
    uint8_t  mask    = s_pixel_bit_lut[lut_idx];
    if (color)
        s_disp_buf[buf_idx] |=  mask;
    else
        s_disp_buf[buf_idx] &= ~mask;
}

void display_flush(void)
{
    rlcd_send_cmd(0x2A);
    rlcd_send_data(0x12);
    rlcd_send_data(0x2A);
    rlcd_send_cmd(0x2B);
    rlcd_send_data(0x00);
    rlcd_send_data(0xC7);
    rlcd_send_cmd(0x2C);
    rlcd_send_buf(s_disp_buf, DISP_BUF_LEN);
}

void display_fill_screen(uint8_t color)
{
    memset(s_disp_buf, color, DISP_BUF_LEN);
    display_flush();
}

void display_fill_screen_rgb565(uint16_t color)
{
    // Map RGB565 to monochrome: pixels above mid-gray → white, below → black
    display_fill_screen((color >= 0x7FFFu) ? DISPLAY_COLOR_WHITE : DISPLAY_COLOR_BLACK);
}

void display_draw_frame_mono_1bpp(const uint8_t *frame, size_t frame_len)
{
    if (!frame || frame_len != INPUT_MONO_FRAME_LEN) {
        ESP_LOGW(TAG, "Invalid mono frame input (ptr=%p, len=%u)",
                 frame, (unsigned)frame_len);
        return;
    }

    memset(s_disp_buf, 0x00, DISP_BUF_LEN);

    for (uint16_t y = 0; y < LCD_V_RES; y++) {
        const size_t row_base = (size_t)y * (LCD_H_RES / 8);
        for (uint16_t x = 0; x < LCD_H_RES; x++) {
            const size_t byte_idx = row_base + (x >> 3);
            const uint8_t bit_mask = (uint8_t)(0x80u >> (x & 0x07u));
            const uint8_t is_white = (frame[byte_idx] & bit_mask) ? 1u : 0u;
            display_set_pixel(x, y, is_white ? DISPLAY_COLOR_WHITE : DISPLAY_COLOR_BLACK);
        }
    }

    display_flush();
}

void display_draw_frame_native_1bpp(const uint8_t *frame, size_t frame_len)
{
    if (!frame || frame_len != DISP_BUF_LEN) {
        ESP_LOGW(TAG, "Invalid native mono frame input (ptr=%p, len=%u)",
                 frame, (unsigned)frame_len);
        return;
    }

    memcpy(s_disp_buf, frame, DISP_BUF_LEN);
    display_flush();
}

void display_show_frame(const uint8_t *rgb565_frame, uint16_t width, uint16_t height)
{
    if (!rgb565_frame || width != LCD_H_RES || height != LCD_V_RES) {
        ESP_LOGW(TAG, "Invalid RGB565 frame (ptr=%p, %ux%u)", rgb565_frame, width, height);
        return;
    }

    memset(s_disp_buf, 0x00, DISP_BUF_LEN);

    // Fast path for boot video: pack monochrome bits directly into panel buffer.
    const uint16_t h4 = LCD_V_RES / 4;  // 75 for 300px panel height
    for (uint16_t y = 0; y < height; y++) {
        const uint16_t inv_y = (uint16_t)(LCD_V_RES - 1u - y);
        const uint16_t block_y = inv_y >> 2;
        const uint8_t local_y = (uint8_t)(inv_y & 0x03u);
        const uint8_t bit_even = (uint8_t)(1u << (7u - (local_y << 1)));
        const uint8_t bit_odd = (uint8_t)(bit_even >> 1);

        size_t pixel_offset = (size_t)y * width * 2;
        for (uint16_t x = 0; x < width; x += 2) {
            const uint32_t buf_idx = (uint32_t)(x >> 1) * h4 + block_y;

            // Pixel 0 (x even)
            const uint16_t rgb0 = (uint16_t)rgb565_frame[pixel_offset] |
                                  ((uint16_t)rgb565_frame[pixel_offset + 1] << 8);
            pixel_offset += 2;

            // Brightness in RGB565 domain, range 0..156.
            const uint16_t lum0 =
                (uint16_t)(((rgb0 >> 11) & 0x1Fu) << 1) +
                (uint16_t)((rgb0 >> 5) & 0x3Fu) +
                (uint16_t)(rgb0 & 0x1Fu);
            if (lum0 >= 78u) {
                s_disp_buf[buf_idx] |= bit_even;
            }

            // Pixel 1 (x odd)
            const uint16_t rgb1 = (uint16_t)rgb565_frame[pixel_offset] |
                                  ((uint16_t)rgb565_frame[pixel_offset + 1] << 8);
            pixel_offset += 2;

            const uint16_t lum1 =
                (uint16_t)(((rgb1 >> 11) & 0x1Fu) << 1) +
                (uint16_t)((rgb1 >> 5) & 0x3Fu) +
                (uint16_t)(rgb1 & 0x1Fu);
            if (lum1 >= 78u) {
                s_disp_buf[buf_idx] |= bit_odd;
            }
        }
    }

    display_flush();
}
