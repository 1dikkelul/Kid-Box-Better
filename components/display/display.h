#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stddef.h>

// Physical resolution (landscape)
#define LCD_H_RES  400
#define LCD_V_RES  300

// Monochrome pixel values (match ST7305 DispBuffer encoding)
#define DISPLAY_COLOR_BLACK  0x00u
#define DISPLAY_COLOR_WHITE  0xFFu

// Initialise SPI bus, ST7305 panel, pixel LUT, and frame buffer.
// Must be called before any other display_ function.
void display_init(void);

// Set a single pixel in the frame buffer (does NOT push to hardware).
// color: DISPLAY_COLOR_WHITE or DISPLAY_COLOR_BLACK
void display_set_pixel(uint16_t x, uint16_t y, uint8_t color);

// Push the current frame buffer to the display over SPI.
void display_flush(void);

// Fill the entire frame buffer with color and flush.
// color: DISPLAY_COLOR_WHITE or DISPLAY_COLOR_BLACK
void display_fill_screen(uint8_t color);

// Compatibility shim: maps an RGB565 value to black/white and calls display_fill_screen.
void display_fill_screen_rgb565(uint16_t color);

// Draw a full 1bpp frame in row-major bit-packed format and flush.
// Input is expected to be 400x300 monochrome: (400 * 300) / 8 = 15000 bytes.
// Bit order per byte is MSB-first: bit7 -> left-most pixel, bit0 -> right-most pixel.
void display_draw_frame_mono_1bpp(const uint8_t *frame, size_t frame_len);

// Draw a frame that is already encoded in this panel's native packed 1bpp layout and flush.
// Expected frame size for 400x300 is (400/2) * (300/4) = 15000 bytes.
void display_draw_frame_native_1bpp(const uint8_t *frame, size_t frame_len);

// Draw an RGB565 frame (400x300) and flush to display.
// Converts RGB565 colors to device monochrome (above mid-gray -> white, below -> black).
// Input is expected to be 400 * 300 * 2 = 240,000 bytes.
void display_show_frame(const uint8_t *rgb565_frame, uint16_t width, uint16_t height);

#endif // DISPLAY_H
