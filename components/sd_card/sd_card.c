

#include "sd_card.h"
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <driver/sdmmc_host.h>
#include <errno.h>
#include <string.h>

// SD card uses the SDMMC peripheral in 1-bit mode.
// GPIO assignments for the ESP32-S3-RLCD-4.2 board:
#define SD_CLK_PIN  38
#define SD_CMD_PIN  21
#define SD_D0_PIN   39

#define MOUNT_POINT "/sdcard"
#define EXAMPLE_MAX_CHAR_SIZE 64

static const char *TAG = "sd_card";
static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;

esp_err_t sd_card_write_file(const char *path, char *data)
{
    ESP_LOGI(TAG, "Opening file %s", path);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing: %s", strerror(errno));
        return ESP_FAIL;
    }
    fprintf(f, data);
    fclose(f);
    ESP_LOGI(TAG, "File written");
    return ESP_OK;
}

esp_err_t sd_card_read_file(const char *path)
{
    ESP_LOGI(TAG, "Reading file %s", path);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for reading: %s", strerror(errno));
        return ESP_FAIL;
    }
    char line[EXAMPLE_MAX_CHAR_SIZE];
    fgets(line, sizeof(line), f);
    fclose(f);
    char *pos = strchr(line, '\n');
    if (pos) {
        *pos = '\0';
    }
    ESP_LOGI(TAG, "Read from file: '%s'", line);
    return ESP_OK;
}

esp_err_t sd_card_mount(void)
{
    if (s_mounted) {
        ESP_LOGI(TAG, "Filesystem already mounted at %s", MOUNT_POINT);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing SD card (SDMMC 1-bit, CLK=%d CMD=%d D0=%d)",
             SD_CLK_PIN, SD_CMD_PIN, SD_D0_PIN);

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    slot_config.clk   = SD_CLK_PIN;
    slot_config.cmd   = SD_CMD_PIN;
    slot_config.d0    = SD_D0_PIN;
    // Enable internal pull-ups (the board may have external ones too)
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting filesystem");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount filesystem (%s). SD card may not be present.",
                 esp_err_to_name(ret));
        return ret;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "Filesystem mounted at %s", MOUNT_POINT);
    sdmmc_card_print_info(stdout, s_card);
    return ESP_OK;
}

esp_err_t sd_card_unmount(void)
{
    const char mount_point[] = MOUNT_POINT;

    if (!s_mounted || !s_card) {
        return ESP_OK;
    }

    esp_vfs_fat_sdcard_unmount(mount_point, s_card);
    s_card = NULL;
    s_mounted = false;
    ESP_LOGI(TAG, "Card unmounted");
    return ESP_OK;
}

esp_err_t sd_card_init_and_demo(void)
{
    return sd_card_mount();
}
