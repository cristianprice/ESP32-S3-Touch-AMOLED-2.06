#pragma once

/**
 * @brief Create the SD-card WAV browser and player on the active screen.
 *
 * The view mounts and later releases shared SD-card resources as needed. It
 * must be entered from a context permitted to create LVGL objects.
 */
void multimedia_player_create(void);
