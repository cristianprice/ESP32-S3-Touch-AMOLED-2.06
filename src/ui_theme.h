#pragma once

#include "lvgl.h"

/**
 * @brief Apply the shared charcoal/orange button treatment.
 *
 * The caller retains ownership of @p button and must call this only while it
 * is legal to mutate LVGL objects (normally the display/LVGL context).
 */
void ui_theme_apply_button(lv_obj_t *button);
