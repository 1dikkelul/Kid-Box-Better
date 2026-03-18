

#include "sd_card.h"
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <driver/spi_master.h>
#include "driver/gpio.h"

#define MOUNT_POINT "/sdcard"
#define EXAMPLE_MAX_CHAR_SIZE 64

#define SD_SPI_HOST SPI3_HOST // VSPI_HOST for SD card, other SPI used by TFT display
#define SD_SPI_DMA_CH 2 // Use DMA channel 2, TFT display uses channel 1

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

    esp_err_t ret;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    const char mount_point[] = MOUNT_POINT;

    ESP_LOGI(TAG, "Initializing SD card");
    ESP_LOGI(TAG, "Using SPI peripheral");
    // Use SD_SPI_HOST for SD card to avoid conflict with TFT
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    // Use dedicated SD Card SPI bus: MOSI=23, MISO=19, CLK=18, CS=5
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = 23,
        .miso_io_num = 19,
        .sclk_io_num = 18,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    // Only initialize SPI bus if not already initialized
    ret = spi_bus_initialize(host.slot, &bus_cfg, SD_SPI_DMA_CH);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "SPI bus already initialized, skipping initialization.");
        ret = ESP_OK;
    } else if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize bus.");
        return ret;
    }
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = 5; // SD card CS
    slot_config.host_id = SD_SPI_HOST; // Ensure device uses SD_SPI_HOST
    ESP_LOGI(TAG, "Mounting filesystem");
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount filesystem. SD card may not be present or pins may be incorrect.");
        return ret;
    }
    s_mounted = true;

    ESP_LOGI(TAG, "Filesystem mounted");
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
