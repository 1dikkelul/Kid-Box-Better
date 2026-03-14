#ifndef DISPLAY_H
#define DISPLAY_H


#include "esp_lcd_types.h"
#include "esp_lcd_panel_io.h"
#include "esp_err.h"
#include <stdbool.h>
void display_init(void);
void display_test(void);
void display_fill_screen_rgb565(uint16_t color);
void display_set_orientation(bool swap_xy, bool mirror_x, bool mirror_y);
esp_lcd_panel_handle_t display_get_panel_handle(void);
esp_lcd_panel_io_handle_t display_get_panel_io_handle(void);
esp_err_t display_reinit_spi_bus(void);

#endif // DISPLAY_H
