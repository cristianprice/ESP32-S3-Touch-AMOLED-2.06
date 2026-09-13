#pragma once

/*
 * Build the Internet Radio screen and pause automatic deep sleep while it is
 * open. The screen owns the Play/Pause, volume, and Back controls; Back asks
 * the workers to stop and returns to the main menu.
 *
 * Must be called from the LVGL context.
 */
void internet_radio_create(void);
