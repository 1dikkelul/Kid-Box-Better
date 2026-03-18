#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t start_file_server(const char *base_path);
esp_err_t stop_file_server(void);

#ifdef __cplusplus
}
#endif