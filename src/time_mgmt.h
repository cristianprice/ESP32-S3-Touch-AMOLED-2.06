#pragma once

#include <stdbool.h>
#include <time.h>

#include "esp_err.h"

/** @brief Report whether the PCF85063 has been initialized for this boot. */
bool time_mgmt_is_initialized(void);
/**
 * @brief Initialize the shared-I2C PCF85063 and create the clock screen/timer.
 *
 * @return ESP_OK or the first I2C, RTC, allocation, or time-validation error.
 *         On success, time_mgmt_stop() retires only the LVGL update timer.
 */
esp_err_t time_mgmt_start(void);
/**
 * @brief Convert and store a UTC Unix timestamp in the PCF85063 RTC.
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE before initialization, or an RTC error.
 */
esp_err_t time_mgmt_set_utc(time_t utc_time);
/**
 * @brief Read and write the current RTC value before deep sleep.
 *
 * @return ESP_OK, ESP_ERR_INVALID_STATE before initialization, or an RTC error.
 */
esp_err_t time_mgmt_save(void);
/** @brief Stop and release the LVGL clock timer after the menu replaces its screen. */
void time_mgmt_stop(void);
