#ifndef TIME_MANAGER_H
#define TIME_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

void time_manager_init(void);
bool time_manager_wait_for_sync(uint32_t timeout_ms);

#endif // TIME_MANAGER_H
