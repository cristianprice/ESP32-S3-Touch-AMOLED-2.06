#pragma once

#include "lvgl.h"

/**
 * @brief Safely queue work for execution in LVGL's display-task context.
 *
 * The BSP owns LVGL through a recursive mutex. This helper locks that same
 * mutex while it modifies LVGL's asynchronous callback queue, so it is safe
 * from event callbacks, LVGL timers, and unrelated FreeRTOS worker tasks.
 *
 * @return The result reported by lv_async_call().
 */
lv_result_t ui_async_call(lv_async_cb_t callback, void *user_data);
