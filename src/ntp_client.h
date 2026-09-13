#pragma once

/**
 * @brief Create the network time synchronization view on the active screen.
 *
 * The view coordinates station connectivity and asynchronous NTP work; call
 * from an LVGL-safe context after the prior view is removed.
 */
void ntp_client_create(void);
