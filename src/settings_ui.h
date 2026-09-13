#pragma once

/**
 * @brief Create Settings and start the WatchConfig SoftAP/web-portal lifecycle.
 *
 * The view owns its HTTP, SPIFFS, Wi-Fi, and NVS resources until its Back
 * action shuts them down in dependency order. Call from an LVGL-safe context.
 */
void settings_ui_create(void);
