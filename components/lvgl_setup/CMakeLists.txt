idf_component_register(
    SRCS "lvgl_setup.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl esp_lcd_ili9341 esp_lcd_touch esp_lcd_touch_xpt2046
)
