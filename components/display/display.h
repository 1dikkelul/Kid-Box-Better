#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>

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

#endif // DISPLAY_H
