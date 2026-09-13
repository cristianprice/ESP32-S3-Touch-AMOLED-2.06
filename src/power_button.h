#pragma once

#include <stdint.h>

#include "esp_err.h"

/**
 * @brief Read the PMIC's reported battery percentage.
 *
 * @param[out] percent Destination for a value in the inclusive range 0–100.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG for NULL, ESP_ERR_INVALID_STATE
 *         before power_button_start(), or an I2C/response error.
 */
esp_err_t power_button_get_battery_percent(uint8_t *percent);
/**
 * @brief Initialize the shared-I2C AXP2101 and start its power-key polling task.
 *
 * The module owns the PMIC device handle and its task for the rest of the boot.
 * @return ESP_OK, an I2C initialization error, or ESP_ERR_NO_MEM if the task
 *         cannot be created.
 */
esp_err_t power_button_start(void);
