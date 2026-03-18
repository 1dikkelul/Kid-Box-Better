#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "sd_card.h"
#include "time.h"
#include "lvgl.h"
#include "display.h"
#include "ui.h"
#include "lvgl_setup.h"
#include "time_manager.h"
#include "weather_manager.h"
#include "file_server.h"
#include "esp_heap_caps.h"
#include <string.h>

#define BOOT_PHASE_PAUSE_MS 2000
#define FILE_SERVER_TASK_STACK 4096
#define WEATHER_BOOT_FETCH_MAX_ATTEMPTS 5
#define WEATHER_BOOT_FETCH_RETRY_MS 3000

char ip_str[16];

static bool s_sd_available = false;

static void heap_alloc_failed_cb(size_t requested_size, uint32_t caps, const char *function_name)
{
    ESP_LOGE("HEAP_OOM", "Alloc FAILED: %u bytes (caps=0x%08lX) in %s | free=%lu largest_block=%lu",
             (unsigned)requested_size,
             (unsigned long)caps,
             function_name ? function_name : "unknown",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void phase_pause(const char *label, uint32_t ms);
static void file_server_auto_start_task(void *arg);
static void weather_update_task(void *arg);

static void nvs_init_or_recover(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
}

static void weather_update_task(void *arg)
{
    (void)arg;
    ESP_LOGI("weather_task", "Boot fetch: waiting for WiFi...");

    // Wait until WiFi is actually connected and has an IP
    while (!wifi_manager_is_ready()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    ESP_LOGI("weather_task", "Boot fetch: WiFi connected. Waiting for SNTP time sync...");
    // Time must be synced before we can make HTTPS requests.
    while (!time_manager_wait_for_sync(10000)) {
        ESP_LOGW("weather_task", "SNTP time not synced yet. Retrying in 10s...");
    }
    ESP_LOGI("weather_task", "Boot fetch: SNTP synced. Fetching weather with retries.");

    bool fetch_ok = false;
    for (int attempt = 1; attempt <= WEATHER_BOOT_FETCH_MAX_ATTEMPTS; attempt++) {
        ESP_LOGI("weather_task", "Boot fetch attempt %d/%d", attempt, WEATHER_BOOT_FETCH_MAX_ATTEMPTS);
        fetch_ok = weather_manager_fetch();
        if (fetch_ok) {
            break;
        }
        if (attempt < WEATHER_BOOT_FETCH_MAX_ATTEMPTS) {
            ESP_LOGW("weather_task", "Boot fetch failed, retrying in %d ms", WEATHER_BOOT_FETCH_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(WEATHER_BOOT_FETCH_RETRY_MS));
        }
    }

    if (!fetch_ok) {
        ESP_LOGE("weather_task", "Boot fetch failed after %d attempts", WEATHER_BOOT_FETCH_MAX_ATTEMPTS);
    }

    // Weather data is cached by weather_manager and consumed by UI when available.
    ESP_LOGI("weather_task", "Boot fetch complete");
    vTaskDelete(NULL);
}

// File server starts automatically once WiFi is ready.
static void file_server_auto_start_task(void *arg)
{
    (void)arg;
    ESP_LOGI("file_server", "Waiting for WiFi before starting file server...");
    while (!wifi_manager_is_ready()) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI("file_server", "WiFi ready -- starting file server at /sdcard");
    if (start_file_server("/sdcard") != ESP_OK) {
        ESP_LOGE("file_server", "Failed to start file server");
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    printf("\n\n=== KIDSBOX RLCD BUILD %s %s ===\n\n", __DATE__, __TIME__);

    nvs_init_or_recover();
    heap_caps_register_failed_alloc_callback(heap_alloc_failed_cb);

    wifi_manager_start_sta();
    printf("WiFi init done\n");

    time_manager_init();

    esp_err_t sd_status = sd_card_mount();
    printf("SD Card status: %s\n", esp_err_to_name(sd_status));
    s_sd_available = (sd_status == ESP_OK);
    if (!s_sd_available) {
        ESP_LOGW("app_main", "SD not mounted -- file server unavailable");
    }

    display_init();
    printf("Display init done\n");

    // Boot sanity: BLACK screen then WHITE screen
    display_fill_screen(DISPLAY_COLOR_BLACK);
    phase_pause("Solid BLACK", BOOT_PHASE_PAUSE_MS);
    display_fill_screen(DISPLAY_COLOR_WHITE);
    phase_pause("Solid WHITE", BOOT_PHASE_PAUSE_MS);

    wifi_manager_status_t st = wifi_manager_get_status();
    printf("WiFi status: started=%d connected=%d got_ip=%d\n",
           st.started, st.connected, st.got_ip);

    lvgl_setup_start();
    phase_pause("LVGL init", BOOT_PHASE_PAUSE_MS);

    if (lvgl_setup_lock(-1)) {
        ui_init();
        lvgl_setup_unlock();
    }
    phase_pause("UI init", BOOT_PHASE_PAUSE_MS);

    // File server runs continuously when SD is available (PSRAM allows it).
    if (s_sd_available) {
        xTaskCreate(file_server_auto_start_task, "file_server", FILE_SERVER_TASK_STACK, NULL, 4, NULL);
    }

    xTaskCreate(weather_update_task, "weather_task", 8192, NULL, 5, NULL);

    // Keep app task alive; LVGL timer is driven by lvgl_task inside lvgl_setup.
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void phase_pause(const char *label, uint32_t ms)
{
    printf("[PHASE] %s (%lu ms)\n", label, (unsigned long)ms);
    vTaskDelay(pdMS_TO_TICKS(ms));
}
