#pragma once

#include <stdint.h>

#include "esp_err.h"

esp_err_t power_button_get_battery_percent(uint8_t *percent);
/* Initialize PMIC power-key polling and expose the current battery percentage. */
esp_err_t power_button_start(void);
