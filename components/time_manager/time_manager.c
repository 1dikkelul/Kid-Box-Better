#include "time_manager.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <stdlib.h>

static const char *TAG = "time_manager";

#define MIN_VALID_YEAR 2024

static bool time_is_valid(void)
{
    time_t now = 0;
    struct tm timeinfo = {0};

    time(&now);
    localtime_r(&now, &timeinfo);
    return (timeinfo.tm_year + 1900) >= MIN_VALID_YEAR;
}

// SNTP time sync notification callback
static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synced, system time is set.");
    esp_sntp_stop(); // One-shot sync: stop polling to free lwIP resources
}

void time_manager_init(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    // Set timezone to Adelaide, Australia (ACST/ACDT)
    // ACST-9:30ACDT,M10.1.0,M4.1.0/3
    setenv("TZ", "ACST-9:30ACDT,M10.1.0,M4.1.0/3", 1);
    tzset();
    ESP_LOGI(TAG, "Timezone set to Adelaide");
}

bool time_manager_wait_for_sync(uint32_t timeout_ms)
{
    const TickType_t poll_delay = pdMS_TO_TICKS(500);
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);

    if (time_is_valid()) {
        return true;
    }

    ESP_LOGI(TAG, "Waiting for SNTP time sync (%lu ms timeout)", (unsigned long)timeout_ms);

    while ((xTaskGetTickCount() - start) < timeout) {
        if (time_is_valid()) {
            ESP_LOGI(TAG, "SNTP time is valid");
            return true;
        }
        vTaskDelay(poll_delay);
    }

    ESP_LOGW(TAG, "SNTP sync timeout; current time is still not valid");
    return false;
}
