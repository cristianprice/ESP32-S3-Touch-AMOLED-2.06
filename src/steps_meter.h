#pragma once

/**
 * @brief Create the steps-meter screen and start its session display.
 *
 * The active view owns its periodic UI update timer. Invoke only from a
 * context that may create LVGL objects.
 */
void steps_meter_create(void);
