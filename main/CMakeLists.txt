idf_component_register(SRCS "app_main.c"
                    INCLUDE_DIRS "."
                    REQUIRES nvs_flash
                             wifi_manager
                             time_manager
                             file_server
                             boot_video
                             display
                             lvgl_squareline
                             sd_card
                             lvgl_setup
                             weather_manager)