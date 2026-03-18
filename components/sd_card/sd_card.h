#pragma once
#include "esp_err.h"

esp_err_t sd_card_write_file(const char *path, char *data);
esp_err_t sd_card_read_file(const char *path);
esp_err_t sd_card_mount(void);
esp_err_t sd_card_unmount(void);
esp_err_t sd_card_init_and_demo(void);
