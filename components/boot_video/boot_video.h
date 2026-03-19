#pragma once

#include "esp_err.h"

// Plays an RLV1 boot video from SD card.
// Returns ESP_OK when playback succeeds, or an error if file/format cannot be used.
// Friendly failure: returns ESP_ERR_NOT_FOUND if file doesn't exist (non-blocking startup).
esp_err_t boot_video_play_from_file(const char *path);
