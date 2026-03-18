#ifndef LVGL_SETUP_H
#define LVGL_SETUP_H

#include <stdbool.h>

// Initialise LVGL, allocate SPIRAM frame buffers, register flush callback, start LVGL task.
void lvgl_setup_start(void);

// Acquire / release the LVGL API mutex.
// timeout_ms < 0 → wait forever.  Returns true if lock was acquired.
bool lvgl_setup_lock(int timeout_ms);
void lvgl_setup_unlock(void);

#endif // LVGL_SETUP_H
