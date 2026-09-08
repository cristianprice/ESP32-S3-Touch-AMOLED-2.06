#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* The RTC is initialized by time_mgmt_start and persists while the device sleeps. */
bool time_mgmt_is_initialized(void);
esp_err_t time_mgmt_start(void);
/* Re-write the current RTC value before deep sleep. */
esp_err_t time_mgmt_save(void);
/* Stop the LVGL clock timer after the main menu replaces the clock screen. */
void time_mgmt_stop(void);
