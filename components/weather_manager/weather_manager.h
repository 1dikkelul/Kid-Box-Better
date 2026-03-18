#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fetch weather data from the API and print it to the console.
// Returns true when a valid weather payload was parsed and applied.
bool weather_manager_fetch(void);

// Get the last fetched temperature
float weather_manager_get_temp(void);

// Get the last fetched wind speed (km/h)
float weather_manager_get_wind_speed(void);

// Get the last fetched weather code (WMO)
int weather_manager_get_weather_code(void);

// Get is_day flag from the last fetch: 1 = daytime, 0 = night
int weather_manager_get_is_day(void);

// Get the icon file path for a given weather code (uses internal is_day state)
const char* weather_manager_get_icon_path_from_code(int code);

#ifdef __cplusplus
}
#endif