#pragma once

/**
 * @brief Create the CSI motion chart and begin its Wi-Fi capture session.
 *
 * The view owns its queue, LVGL refresh timer, and promiscuous Wi-Fi setup
 * until Back tears them down. Must be called from an LVGL-safe context.
 */
void motion_detection_create(void);
