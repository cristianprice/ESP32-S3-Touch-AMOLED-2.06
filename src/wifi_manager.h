#pragma once

/**
 * @brief Create the nearby-network scan and station-connection view.
 *
 * The view initiates scans and owns its UI lifecycle; callers must already be
 * in an LVGL-safe context and have replaced any incompatible prior view.
 */
void wifi_manager_create(void);
