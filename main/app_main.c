#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "wifi_manager.h"
#include "sd_card.h"
#include "lvgl.h"
#include "display.h"
#include "driver/spi_master.h"
#include "ui.h"
#include "lvgl_setup.h"

#define BOOT_PHASE_PAUSE_MS 2000
#define BUTTON_MONITOR_STACK_SIZE 2048
#define BUTTON_MONITOR_POLL_MS 20

#define BUTTON_1_GPIO GPIO_NUM_21
#define BUTTON_2_GPIO GPIO_NUM_22

char ip_str[16];
extern spi_device_handle_t spi;


static void phase_pause(const char* label, uint32_t ms);
static void phase_pause_with_lvgl(const char *label, uint32_t ms);
static void button_monitor_init(void);
static void button_monitor_task(void *arg);

static void nvs_init_or_recover(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
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

static void button_monitor_task(void *arg)
{
    (void)arg;

    const char *tag = "button_monitor";
    int last_button_1 = gpio_get_level(BUTTON_1_GPIO);
    int last_button_2 = gpio_get_level(BUTTON_2_GPIO);

    ESP_LOGI(tag, "Monitoring buttons on GPIO%d and GPIO%d", BUTTON_1_GPIO, BUTTON_2_GPIO);
    ESP_LOGI(tag, "Buttons are active-low with internal pull-ups enabled");

    while (1) {
        const int button_1_level = gpio_get_level(BUTTON_1_GPIO);
        const int button_2_level = gpio_get_level(BUTTON_2_GPIO);

        if (button_1_level != last_button_1) {
            last_button_1 = button_1_level;
            ESP_LOGI(tag, "GPIO%d button %s", BUTTON_1_GPIO, button_1_level == 0 ? "pressed" : "released");
        }

        if (button_2_level != last_button_2) {
            last_button_2 = button_2_level;
            ESP_LOGI(tag, "GPIO%d button %s", BUTTON_2_GPIO, button_2_level == 0 ? "pressed" : "released");
        }

        vTaskDelay(pdMS_TO_TICKS(BUTTON_MONITOR_POLL_MS));
    }
}

void app_main(void)
{
    printf("\n\n=== KIDSBOX NEW BUILD %s %s ===\n\n", __DATE__, __TIME__);
    
    nvs_init_or_recover(); // Initialize NVS, erasing if necessary to recover from issues like "no free pages"

    wifi_manager_start_sta(); // Start WiFi in station mode
    printf("Finished WiFi Initialization\n");
    
    sd_card_init_and_demo(); // Initialize SD card and run demo operations
    printf("SD Card Initialized\n");

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
    
    lvgl_setup_start(); // Initialize LVGL
    phase_pause_with_lvgl("LVGL Initialized", BOOT_PHASE_PAUSE_MS);

    ui_init(); // Initialize the SquareLine UI
    phase_pause_with_lvgl("UI Initialized...", BOOT_PHASE_PAUSE_MS); 
    
    while (1) {
        lv_timer_handler();
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
        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        elapsed += step_ms;
    }
}
