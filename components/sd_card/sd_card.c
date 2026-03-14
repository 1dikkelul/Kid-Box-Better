

#include "sd_card.h"
#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <sdmmc_cmd.h>
#include <unistd.h>
#include <sys/stat.h>
#include <driver/spi_master.h>
#include "driver/gpio.h"

#define MOUNT_POINT "/sdcard"
#define EXAMPLE_MAX_CHAR_SIZE 64

#define SD_SPI_HOST SPI3_HOST // VSPI_HOST for SD card, other SPI used by TFT display
#define SD_SPI_DMA_CH 2 // Use DMA channel 2, TFT display uses channel 1

static const char *TAG = "sd_card";

esp_err_t sd_card_write_file(const char *path, char *data)
{
    ESP_LOGI(TAG, "Opening file %s", path);
    FILE *f = fopen(path, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open file for writing");
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
        ESP_LOGE(TAG, "Failed to open file for reading");
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

esp_err_t sd_card_init_and_demo(void)
{
    esp_err_t ret;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    sdmmc_card_t *card;
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
    ret = esp_vfs_fat_sdspi_mount(mount_point, &host, &slot_config, &mount_config, &card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to mount filesystem. SD card may not be present or pins may be incorrect.");
        return ret;
    }
    ESP_LOGI(TAG, "Filesystem mounted");
    sdmmc_card_print_info(stdout, card);
    const char *file_hello = MOUNT_POINT"/hello.txt";
    char data[EXAMPLE_MAX_CHAR_SIZE];
    snprintf(data, EXAMPLE_MAX_CHAR_SIZE, "%s %s!\n", "Hello", card->cid.name);
    ret = sd_card_write_file(file_hello, data);
    if (ret != ESP_OK) {
        esp_vfs_fat_sdcard_unmount(mount_point, card);
        return ret;
    }
    const char *file_foo = MOUNT_POINT"/foo.txt";
    struct stat st;
    if (stat(file_foo, &st) == 0) {
        unlink(file_foo);
    }
    ESP_LOGI(TAG, "Renaming file %s to %s", file_hello, file_foo);
    if (rename(file_hello, file_foo) != 0) {
        ESP_LOGE(TAG, "Rename failed");
        esp_vfs_fat_sdcard_unmount(mount_point, card);
        return ESP_FAIL;
    }
    ret = sd_card_read_file(file_foo);
    if (ret != ESP_OK) {
        esp_vfs_fat_sdcard_unmount(mount_point, card);
        return ret;
    }
    const char *file_nihao = MOUNT_POINT"/nihao.txt";
    memset(data, 0, EXAMPLE_MAX_CHAR_SIZE);
    snprintf(data, EXAMPLE_MAX_CHAR_SIZE, "%s %s!\n", "Nihao", card->cid.name);
    ret = sd_card_write_file(file_nihao, data);
    if (ret != ESP_OK) {
        esp_vfs_fat_sdcard_unmount(mount_point, card);
        return ret;
    }
    ret = sd_card_read_file(file_nihao);
    if (ret != ESP_OK) {
        esp_vfs_fat_sdcard_unmount(mount_point, card);
        return ret;
    }
    esp_vfs_fat_sdcard_unmount(mount_point, card);
    ESP_LOGI(TAG, "Card unmounted");
    // Keep SD SPI bus initialized. On this board, freeing SPI3 can disrupt TFT updates.
    return ESP_OK;
}
