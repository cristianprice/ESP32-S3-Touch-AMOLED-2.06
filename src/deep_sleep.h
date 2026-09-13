#pragma once

/**
 * @brief Power down the display and enter deep sleep without returning.
 *
 * Saves the RTC when it is available, then configures the active-low boot
 * button as the ext0 wake source. Intended for task or timer context after
 * the display service has started; hardware failures are fatal.
 */
void deep_sleep_immediately(void);
/**
 * @brief Create and start the application's one-shot inactivity timer.
 *
 * Must be called once before the reset, pause, or resume APIs. Allocation and
 * FreeRTOS timer failures assert because sleeping is a core power policy.
 */
void sleep_after_timeout(void);
/** @brief Restart the inactivity countdown unless an activity has paused it. */
void sleep_timer_reset(void);
/** @brief Stop the inactivity countdown while a full-screen activity owns wakefulness. */
void sleep_timer_pause(void);
/** @brief Resume the inactivity countdown from its full configured delay. */
void sleep_timer_resume(void);
