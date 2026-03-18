idf_component_register(
    SRCS "lvgl_setup.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl__lvgl esp_timer freertos display
)
