#pragma once

/**
 * @brief Build the 3-by-3 application launcher on the active LVGL screen.
 *
 * Does not clear a prior screen or manage display locking; callers must do
 * both as appropriate before creating the menu.
 */
void main_menu_create(void);
