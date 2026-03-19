#include "boot_video.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "display.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#define BOOT_VIDEO_WIDTH 400u
#define BOOT_VIDEO_HEIGHT 300u
#define BOOT_VIDEO_NATIVE_FRAME_SIZE ((BOOT_VIDEO_WIDTH / 2u) * (BOOT_VIDEO_HEIGHT / 4u))

static const char *TAG = "boot_video";

static uint16_t rd_u16_le(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_u32_le(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static esp_err_t boot_video_play_rlv_from_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Boot video file not found: %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t header[20] = {0};
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        ESP_LOGW(TAG, "RLV header read failed: %s", path);
        return ESP_ERR_INVALID_SIZE;
    }

    if (memcmp(header, "RLV1", 4) != 0) {
        fclose(f);
        ESP_LOGW(TAG, "Invalid RLV magic: %s", path);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const uint16_t width = rd_u16_le(&header[4]);
    const uint16_t height = rd_u16_le(&header[6]);
    uint16_t fps = rd_u16_le(&header[8]);
    const uint32_t frame_count = rd_u32_le(&header[12]);
    const uint32_t frame_size = rd_u32_le(&header[16]);

    if (width != BOOT_VIDEO_WIDTH || height != BOOT_VIDEO_HEIGHT) {
        fclose(f);
        ESP_LOGW(TAG, "RLV dimensions mismatch: %ux%u (expected %ux%u)",
                 (unsigned)width, (unsigned)height,
                 BOOT_VIDEO_WIDTH, BOOT_VIDEO_HEIGHT);
        return ESP_ERR_INVALID_SIZE;
    }

    if (frame_size != BOOT_VIDEO_NATIVE_FRAME_SIZE) {
        fclose(f);
        ESP_LOGW(TAG, "RLV frame size mismatch: %u (expected %u)",
                 (unsigned)frame_size, (unsigned)BOOT_VIDEO_NATIVE_FRAME_SIZE);
        return ESP_ERR_INVALID_SIZE;
    }

    if (frame_count == 0) {
        fclose(f);
        ESP_LOGW(TAG, "RLV has zero frames: %s", path);
        return ESP_ERR_INVALID_SIZE;
    }

    if (fps == 0) {
        fps = 15;
    }

    uint8_t *frame_buf = malloc(frame_size);
    if (!frame_buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    const int64_t frame_period_us = 1000000LL / fps;
    int64_t next_deadline = esp_timer_get_time();
    uint32_t frames = 0;

    for (uint32_t i = 0; i < frame_count; i++) {
        size_t n = fread(frame_buf, 1, frame_size, f);
        if (n != frame_size) {
            ESP_LOGW(TAG, "RLV frame read failed at %u/%u", (unsigned)i, (unsigned)frame_count);
            break;
        }

        display_draw_frame_native_1bpp(frame_buf, frame_size);
        frames++;

        next_deadline += frame_period_us;
        int64_t now = esp_timer_get_time();
        int64_t sleep_us = next_deadline - now;
        if (sleep_us > 0) {
            esp_rom_delay_us((uint32_t)sleep_us);
        } else {
            next_deadline = now;
        }
    }

    free(frame_buf);
    fclose(f);

    if (frames == 0) {
        ESP_LOGW(TAG, "RLV playback had no rendered frames: %s", path);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "RLV playback finished (%u/%u frames, %u fps)",
             (unsigned)frames, (unsigned)frame_count, (unsigned)fps);
    return ESP_OK;
}

esp_err_t boot_video_play_from_file(const char *path)
{
    if (!path || !path[0]) {
        return ESP_ERR_INVALID_ARG;
    }

    return boot_video_play_rlv_from_file(path);
}

