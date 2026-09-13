#include "ui_theme.h"

/*
 * Centralizes the reusable visual contract for interactive controls. This
 * helper intentionally only changes styles; allocation, event callbacks, and
 * LVGL object lifetime remain with each calling view.
 */
void ui_theme_apply_button(lv_obj_t *button)
{
    lv_obj_set_style_radius(button, 16, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x1C1C1C), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x303030), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0xFF7A00), 0);
    lv_obj_set_style_shadow_color(button, lv_color_black(), 0);
    lv_obj_set_style_shadow_width(button, 8, 0);
    lv_obj_set_style_shadow_opa(button, LV_OPA_60, 0);
    lv_obj_set_style_shadow_offset_y(button, 4, 0);
}
