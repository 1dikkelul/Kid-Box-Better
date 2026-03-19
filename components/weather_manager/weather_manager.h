#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEATHER_MANAGER_SLOT_COUNT 5

typedef struct {
	int hour_24;
	float temp_c;
	int weather_code;
	int is_day;
	bool valid;
} weather_manager_hourly_slot_t;

// Fetch weather data from the API and print it to the console.
// Returns true when a valid weather payload was parsed and applied.
bool weather_manager_fetch(void);

// Get the last fetched temperature
float weather_manager_get_temp(void);

// Get the last fetched humidity (%)
float weather_manager_get_humidity(void);

// Get the last fetched wind speed (km/h)
float weather_manager_get_wind_speed(void);

// Get the last fetched weather code (WMO)
int weather_manager_get_weather_code(void);

// Get is_day flag from the last fetch: 1 = daytime, 0 = night
int weather_manager_get_is_day(void);

// Get the icon file path for a given weather code (uses internal is_day state)
const char* weather_manager_get_icon_path_from_code(int code);

// Get icon file path for explicit day/night and icon size (65px or 144px)
const char* weather_manager_get_icon_path_from_code_size(int code, int is_day, int icon_size_px);

// Copy cached hourly slot by index (0..WEATHER_MANAGER_SLOT_COUNT-1).
// Returns true when slot data is valid.
bool weather_manager_get_hourly_slot(int index, weather_manager_hourly_slot_t *slot_out);

#ifdef __cplusplus
}
#endif