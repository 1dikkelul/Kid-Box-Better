#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Fetch weather data from the API and print it to the console
void weather_manager_fetch(void);

// Get the last fetched temperature
float weather_manager_get_temp(void);

#ifdef __cplusplus
}
#endif