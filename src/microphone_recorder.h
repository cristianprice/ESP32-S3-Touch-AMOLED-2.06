#pragma once

/**
 * @brief Create the microphone recorder and start its capture lifecycle.
 *
 * The UI and recorder resources remain owned by the view until its Back
 * action performs cleanup. Call only from a legal LVGL context.
 */
void microphone_recorder_create(void);
