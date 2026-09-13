#pragma once

/**
 * @brief Create the FTP control view on the active LVGL screen.
 *
 * It coordinates station connectivity or the WatchFTP APSTA fallback and
 * owns the FTP server lifecycle until the view is closed.
 */
void ftp_server_create(void);
