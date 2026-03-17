#include "time_manager.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include <time.h>
#include <stdlib.h>

static const char *TAG = "time_manager";

// SNTP time sync notification callback
static void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synced, system time is set.");
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
