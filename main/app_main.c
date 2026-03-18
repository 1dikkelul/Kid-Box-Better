#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "sd_card.h"
#include "time.h"
#include "lvgl.h"
#include "display.h"
#include "driver/spi_master.h"
#include "ui.h"
#include "lvgl_setup.h"
#include "time_manager.h"
#include "weather_manager.h"
#include "file_server.h"
#include "esp_heap_caps.h"

#define BOOT_PHASE_PAUSE_MS 2000
#define BUTTON_MONITOR_STACK_SIZE 3072
#define FILE_SERVER_START_STACK_SIZE 4096
#define BUTTON_MONITOR_POLL_MS 20
#define WEATHER_BOOT_FETCH_MAX_ATTEMPTS 5
#define WEATHER_BOOT_FETCH_RETRY_MS 3000

#define BUTTON_1_GPIO GPIO_NUM_21
#define BUTTON_2_GPIO GPIO_NUM_22

char ip_str[16];
extern spi_device_handle_t spi;
// SquareLine helper for screen transitions (defined in ui_helpers.c)
extern void _ui_screen_change(lv_obj_t ** target, lv_scr_load_anim_t fademode, int spd, int delay, void (*target_init)(void));

static SemaphoreHandle_t lvgl_api_mutex = NULL;
static bool s_sd_available = false;
static bool s_file_server_started = false;
static bool s_file_server_start_requested = false;
static bool s_file_server_stop_requested = false;
static bool s_was_on_screen2 = false;

static void heap_alloc_failed_cb(size_t requested_size, uint32_t caps, const char *function_name)
{
    ESP_LOGE("HEAP_OOM", "Alloc FAILED: %u bytes (caps=0x%08lX) in %s | free=%lu largest_block=%lu",
             (unsigned)requested_size,
             (unsigned long)caps,
             function_name ? function_name : "unknown",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

static void phase_pause(const char* label, uint32_t ms);
static void phase_pause_with_lvgl(const char *label, uint32_t ms);
static void button_monitor_init(void);
static void button_monitor_task(void *arg);
static void file_server_start_task(void *arg);
static void file_server_stop_task(void *arg);
static void time_update_task(void *arg);
static void weather_update_task(void *arg);
static void update_weather_ui(void);
static void log_weather_icon_debug(const char *icon_path);

static void nvs_init_or_recover(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
}

static void update_weather_ui(void) {
    // This function should be called after weather data is fetched.
    // It updates the labels and icons on the weather screen.
    if (lvgl_api_mutex && xSemaphoreTake(lvgl_api_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
        float temp = weather_manager_get_temp();
        int code = weather_manager_get_weather_code();
        const char* icon_path = weather_manager_get_icon_path_from_code(code);

        ESP_LOGI("ui_update", "Updating Weather UI. Temp: %.1f, Code: %d, Icon: %s", temp, code, icon_path);

        // Check if the UI objects exist before using them (good practice)
        if (ui_TemperatureLabel) {
            lv_label_set_text_fmt(ui_TemperatureLabel, "%.1f C", temp);
        }
        if (ui_WeatherIcon && icon_path) {
            if (strlen(icon_path) > 0) {
                log_weather_icon_debug(icon_path);
                lv_image_set_src(ui_WeatherIcon, icon_path);
                lv_obj_invalidate(ui_WeatherIcon);
                ESP_LOGI("ui_update",
                         "WeatherIcon after set_src: src=%s obj_wh=%ldx%ld hidden=%d screen_active=%d",
                         (const char *)lv_image_get_src(ui_WeatherIcon),
                         (long)lv_obj_get_width(ui_WeatherIcon),
                         (long)lv_obj_get_height(ui_WeatherIcon),
                         (int)lv_obj_has_flag(ui_WeatherIcon, LV_OBJ_FLAG_HIDDEN),
                         (int)(lv_screen_active() == ui_Screen2));
            } else {
                lv_image_set_src(ui_WeatherIcon, NULL); // Hide icon if path is empty
            }
        }
        xSemaphoreGive(lvgl_api_mutex);
    }
}

static void log_weather_icon_debug(const char *icon_path)
{
    lv_fs_file_t file;
    lv_fs_res_t fs_res = lv_fs_open(&file, icon_path, LV_FS_MODE_RD);
    if (fs_res == LV_FS_RES_OK) {
        uint32_t file_size = 0;
        if (lv_fs_seek(&file, 0, LV_FS_SEEK_END) == LV_FS_RES_OK &&
            lv_fs_tell(&file, &file_size) == LV_FS_RES_OK) {
            ESP_LOGI("ui_update", "WeatherIcon FS open OK: path=%s size=%lu", icon_path, (unsigned long)file_size);
        } else {
            ESP_LOGW("ui_update", "WeatherIcon FS open OK but size query failed: path=%s", icon_path);
        }
        lv_fs_close(&file);
    } else {
        ESP_LOGE("ui_update", "WeatherIcon FS open failed: path=%s res=%d", icon_path, (int)fs_res);
    }

    lv_image_header_t header;
    lv_result_t info_res = lv_image_decoder_get_info(icon_path, &header);
    if (info_res == LV_RESULT_OK) {
        ESP_LOGI("ui_update",
                 "WeatherIcon decode info OK: path=%s w=%ld h=%ld cf=%d",
                 icon_path,
                 (long)header.w,
                 (long)header.h,
                 (int)header.cf);
    } else {
        ESP_LOGE("ui_update", "WeatherIcon decode info failed: path=%s res=%d", icon_path, (int)info_res);
    }
}

static void time_update_task(void *arg)
{
    (void)arg;
    char time_str[9]; // "HH:MM:SS\0"
    time_t now;
    struct tm timeinfo;

    // Wait a moment for UI to be ready
    vTaskDelay(pdMS_TO_TICKS(2000));

    while (1) {
        time(&now);
        localtime_r(&now, &timeinfo);

        if (lvgl_api_mutex && xSemaphoreTake(lvgl_api_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            // This assumes you have a label named `TimeLabel` on your UI screen.
            // The generated variable name will be `ui_TimeLabel`.
            if (timeinfo.tm_year > 70) { // Year > 1970, so time is likely synced
                strftime(time_str, sizeof(time_str), "%H:%M:%S", &timeinfo);
                lv_label_set_text(ui_TimeLabel, time_str);
            } else {
                lv_label_set_text(ui_TimeLabel, "Syncing..");
            }
            xSemaphoreGive(lvgl_api_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(1000)); // Update every second
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

    // Don't paint here -- Screen 2 entry watcher will call update_weather_ui() on demand.
    ESP_LOGI("weather_task", "Boot fetch complete");
    vTaskDelete(NULL);
}

static void button_monitor_init(void)
{
    const gpio_config_t button_config = {
        .pin_bit_mask = (1ULL << BUTTON_1_GPIO) | (1ULL << BUTTON_2_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&button_config));
    xTaskCreate(button_monitor_task, "button_monitor", BUTTON_MONITOR_STACK_SIZE, NULL, 5, NULL);
}

static void file_server_start_task(void *arg)
{
    (void)arg;

    if (!s_sd_available) {
        ESP_LOGW("file_server", "Start requested but SD is not mounted");
        s_file_server_start_requested = false;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI("file_server", "Starting file server task");
    if (start_file_server("/sdcard") == ESP_OK) {
        s_file_server_started = true;
    } else {
        ESP_LOGE("file_server", "Failed to start file server");
    }

    s_file_server_start_requested = false;
    vTaskDelete(NULL);
}

static void file_server_stop_task(void *arg)
{
    (void)arg;

    ESP_LOGI("file_server", "Stopping file server task");
    if (stop_file_server() == ESP_OK) {
        s_file_server_started = false;
    } else {
        ESP_LOGE("file_server", "Failed to stop file server");
    }

    s_file_server_stop_requested = false;
    vTaskDelete(NULL);
}

static void button_monitor_task(void *arg)
{
    (void)arg;

    const char *tag = "button_monitor";
    int last_button_1 = gpio_get_level(BUTTON_1_GPIO);
    int last_button_2 = gpio_get_level(BUTTON_2_GPIO);
    uint32_t both_held_ms = 0;

    ESP_LOGI(tag, "Monitoring buttons on GPIO%d and GPIO%d", BUTTON_1_GPIO, BUTTON_2_GPIO);
    ESP_LOGI(tag, "Buttons are active-low. Hold BOTH for 2s to start the file server.");

    while (1) {
        const int button_1_level = gpio_get_level(BUTTON_1_GPIO);
        const int button_2_level = gpio_get_level(BUTTON_2_GPIO);

        if (button_1_level == 0 && button_2_level == 0) {
            // Both held: count towards file server trigger, suppress individual actions
            both_held_ms += BUTTON_MONITOR_POLL_MS;
            if (both_held_ms >= 2000 && !s_file_server_started && !s_file_server_start_requested) {
                if (s_sd_available) {
                    ESP_LOGI(tag, "Both buttons held 2s -- requesting file server start");
                    s_file_server_start_requested = true;
                    if (xTaskCreate(file_server_start_task,
                                    "file_server_start",
                                    FILE_SERVER_START_STACK_SIZE,
                                    NULL,
                                    4,
                                    NULL) != pdPASS) {
                        s_file_server_start_requested = false;
                        ESP_LOGE(tag, "Failed to create file_server_start task");
                    }
                } else {
                    ESP_LOGW(tag, "Both buttons held 2s but SD not mounted -- file server unavailable");
                }
                both_held_ms = 0; // Reset to avoid repeated log spam
            }
        } else {
            both_held_ms = 0;

            // Button 1 press -- go to Screen 1 (only when button 2 is not also pressed)
            if (button_1_level != last_button_1 && button_1_level == 0) {
                if (s_file_server_started && !s_file_server_stop_requested) {
                    ESP_LOGI(tag, "GPIO21 pressed -- requesting file server stop");
                    s_file_server_stop_requested = true;
                    if (xTaskCreate(file_server_stop_task,
                                    "file_server_stop",
                                    FILE_SERVER_START_STACK_SIZE,
                                    NULL,
                                    4,
                                    NULL) != pdPASS) {
                        s_file_server_stop_requested = false;
                        ESP_LOGE(tag, "Failed to create file_server_stop task");
                    }
                }

                if (lvgl_api_mutex && xSemaphoreTake(lvgl_api_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                    _ui_screen_change(&ui_Screen1, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen1_screen_init);
                    xSemaphoreGive(lvgl_api_mutex);
                }
            }

            // Button 2 press -- go to Screen 2 (only when button 1 is not also pressed)
            if (button_2_level != last_button_2 && button_2_level == 0) {
                if (lvgl_api_mutex && xSemaphoreTake(lvgl_api_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
                    ESP_LOGI(tag, "Switching to UI Screen 2");
                    _ui_screen_change(&ui_Screen2, LV_SCR_LOAD_ANIM_NONE, 0, 0, &ui_Screen2_screen_init);
                    if (ui_WeatherIcon) {
                        const char *icon_src = lv_image_get_src(ui_WeatherIcon);
                        ESP_LOGI(tag,
                                 "Screen2 active=%d WeatherIcon src=%s obj_wh=%ldx%ld",
                                 (int)(lv_screen_active() == ui_Screen2),
                                 icon_src ? icon_src : "(null)",
                                 (long)lv_obj_get_width(ui_WeatherIcon),
                                 (long)lv_obj_get_height(ui_WeatherIcon));
                    }
                    xSemaphoreGive(lvgl_api_mutex);
                }
            }
        }

        last_button_1 = button_1_level;
        last_button_2 = button_2_level;
        vTaskDelay(pdMS_TO_TICKS(BUTTON_MONITOR_POLL_MS));
    }
}

void app_main(void)
{
    printf("\n\n=== KIDSBOX NEW BUILD %s %s ===\n\n", __DATE__, __TIME__);
    
    nvs_init_or_recover(); // Initialize NVS, erasing if necessary to recover from issues like "no free pages"
    heap_caps_register_failed_alloc_callback(heap_alloc_failed_cb);

    wifi_manager_start_sta(); // Start WiFi in station mode
    printf("Finished WiFi Initialization\n");

    // Initialize SNTP to get network time after WiFi is connected
    time_manager_init();
    
    esp_err_t sd_status = sd_card_mount();
    printf("SD Card init status: %s\n", esp_err_to_name(sd_status));

    display_init(); // Initialize the display hardware
    printf("Display Initialized\n");

    // Simple hardware color sanity check with full-screen updates.
    display_fill_screen_rgb565(0x001F);
    phase_pause("Solid BLUE", BOOT_PHASE_PAUSE_MS);
    display_fill_screen_rgb565(0xF81F);
    phase_pause("Solid PURPLE", BOOT_PHASE_PAUSE_MS);

    button_monitor_init();
    
    printf("Running WIFI Status\n");    // Print WiFi status to console
    wifi_manager_status_t st =wifi_manager_get_status(); // Get WiFi status
    printf("WIFI status: started=%d connected=%d got_ip=%d\n",
        st.started, st.connected, st.got_ip);
    
    // File server starts on demand: hold both buttons (GPIO21 + GPIO22) for 2 seconds.
    s_sd_available = (sd_status == ESP_OK);
    if (!s_sd_available) {
        ESP_LOGW("app_main", "SD not mounted -- file server will not be available on demand");
    }

    lvgl_api_mutex = xSemaphoreCreateRecursiveMutex();

    lvgl_setup_start(); // Initialize LVGL
    phase_pause_with_lvgl("LVGL Initialized", BOOT_PHASE_PAUSE_MS);

    xSemaphoreTake(lvgl_api_mutex, portMAX_DELAY);
    ui_init(); // Initialize the SquareLine UI
    xSemaphoreGive(lvgl_api_mutex);

    phase_pause_with_lvgl("UI Initialized...", BOOT_PHASE_PAUSE_MS); 

    // Create a task to update the time display on the UI
    xTaskCreate(time_update_task, "time_update", 3072, NULL, 5, NULL);

    // Fetch weather once at boot; Screen 2 only paints cached values.
    xTaskCreate(weather_update_task, "weather_task", 8192, NULL, 5, NULL);
    
    while (1) {
        bool has_screen_sample = false;
        bool on_screen2_now = false;

        // Lock the mutex while LVGL works
        if (xSemaphoreTake(lvgl_api_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            lv_timer_handler();
            on_screen2_now = (lv_screen_active() == ui_Screen2);
            has_screen_sample = true;
            xSemaphoreGive(lvgl_api_mutex);
        }

        if (has_screen_sample && on_screen2_now && !s_was_on_screen2) {
            ESP_LOGI("ui_update", "Screen 2 entered (UI) -> paint cached weather");
            update_weather_ui();
        }

        if (has_screen_sample) {
            s_was_on_screen2 = on_screen2_now;
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void phase_pause(const char* label, uint32_t ms)
{
    printf("[PHASE] %s (%lu ms)\n", label, (unsigned long)ms);
    vTaskDelay(pdMS_TO_TICKS(ms));
}

static void phase_pause_with_lvgl(const char *label, uint32_t ms)
{
    printf("[PHASE] %s (%lu ms, LVGL active)\n", label, (unsigned long)ms);
    const uint32_t step_ms = 10;
    uint32_t elapsed = 0;
    while (elapsed < ms) {
        if (xSemaphoreTake(lvgl_api_mutex, pdMS_TO_TICKS(10)) == pdTRUE) {
            lv_timer_handler();
            xSemaphoreGive(lvgl_api_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        elapsed += step_ms;
    }
}
