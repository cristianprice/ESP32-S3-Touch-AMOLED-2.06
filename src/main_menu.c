#include "main_menu.h"

#include "esp_log.h"
#include "lvgl.h"
#include "assets/chart_icon.h"
#include "assets/abstract_timekeeper.h"
#include "assets/settings_icon.h"
#include "motion_detection.h"
#include "power_button.h"
#include "settings_ui.h"

/* The 3-by-3 grid is sized for the 410-by-502 AMOLED display. */
#define MENU_COLUMN_COUNT 3
#define MENU_PADDING 12
#define MENU_COLUMN_GAP 10
#define MENU_ROW_GAP 20
#define MENU_TOOLBAR_HEIGHT 38
#define MENU_CONTENT_HEIGHT (502 - MENU_TOOLBAR_HEIGHT)
#define MENU_BUTTON_WIDTH 110
#define MENU_BUTTON_HEIGHT 95

static const char *const TAG = "main_menu";

static const lv_coord_t menu_columns[] = {
    LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST
};

static const lv_coord_t menu_rows[] = {
    MENU_BUTTON_HEIGHT, MENU_BUTTON_HEIGHT, MENU_BUTTON_HEIGHT,
    LV_GRID_TEMPLATE_LAST
};

static const char *const menu_icons[] = {
    LV_SYMBOL_WIFI,
    NULL,
    NULL,
    LV_SYMBOL_PLAY,
    LV_SYMBOL_LOOP,
    LV_SYMBOL_UP,
    LV_SYMBOL_LIST,
    LV_SYMBOL_EYE_OPEN,
    LV_SYMBOL_BELL,
};

#define MENU_BUTTON_COUNT (sizeof(menu_icons) / sizeof(menu_icons[0]))

static void open_motion_detection(lv_event_t *event)
{
    (void)event;

    /* Discard the previous screen before creating the tool's full-screen UI. */
    lv_obj_clean(lv_screen_active());
    motion_detection_create();
}

static void open_settings_ui(lv_event_t *event)
{
    (void)event;

    lv_obj_clean(lv_screen_active());
    settings_ui_create();
}

static const char *battery_icon(uint8_t percent)
{
    if (percent <= 10) {
        return LV_SYMBOL_BATTERY_EMPTY;
    }
    if (percent <= 35) {
        return LV_SYMBOL_BATTERY_1;
    }
    if (percent <= 65) {
        return LV_SYMBOL_BATTERY_2;
    }
    if (percent <= 90) {
        return LV_SYMBOL_BATTERY_3;
    }

    return LV_SYMBOL_BATTERY_FULL;
}

static void create_toolbar(lv_obj_t *parent)
{
    lv_obj_t *toolbar = lv_obj_create(parent);
    lv_obj_add_flag(toolbar, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(toolbar, LV_PCT(100), MENU_TOOLBAR_HEIGHT);
    lv_obj_align(toolbar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_border_width(toolbar, 0, 0);
    lv_obj_set_style_radius(toolbar, 0, 0);
    lv_obj_set_style_bg_color(toolbar, lv_color_hex(0x102C42), 0);
    lv_obj_set_style_pad_left(toolbar, MENU_PADDING, 0);

    lv_obj_t *battery_label = lv_label_create(toolbar);
    uint8_t percent;
    esp_err_t result = power_button_get_battery_percent(&percent);
    if (result == ESP_OK) {
        lv_label_set_text_fmt(battery_label, "%s %u%%", battery_icon(percent), percent);
    } else {
        ESP_LOGW(TAG, "Failed to read battery percentage: %s", esp_err_to_name(result));
        lv_label_set_text(battery_label, LV_SYMBOL_BATTERY_EMPTY " --");
    }
    lv_obj_set_style_text_color(battery_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(battery_label, &lv_font_montserrat_24, 0);
    lv_obj_align(battery_label, LV_ALIGN_RIGHT_MID, -32, 0);
}

static void create_menu_button(lv_obj_t *parent, uint32_t index)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, MENU_BUTTON_WIDTH, MENU_BUTTON_HEIGHT);
    lv_obj_set_grid_cell(button, LV_GRID_ALIGN_CENTER, index % MENU_COLUMN_COUNT, 1,
                         LV_GRID_ALIGN_CENTER, index / MENU_COLUMN_COUNT, 1);
    lv_obj_set_style_radius(button, 16, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x202020), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x404040), LV_STATE_PRESSED);
    /* Tiles 1 and 2 use generated RGB565 image descriptors, not font glyphs. */
    if (index == 1) {
        lv_obj_add_event_cb(button, open_settings_ui, LV_EVENT_CLICKED, NULL);
    } else if (index == 2) {
        lv_obj_add_event_cb(button, open_motion_detection, LV_EVENT_CLICKED, NULL);
    }

    lv_obj_t *icon;
    if (index == 1) {
        icon = lv_image_create(button);
        lv_image_set_src(icon, &settings_icon);
    } else if (index == 2) {
        icon = lv_image_create(button);
        lv_image_set_src(icon, &chart_icon);
    } else {
        icon = lv_label_create(button);
        lv_label_set_text(icon, menu_icons[index]);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
        lv_obj_set_style_text_color(icon, lv_color_white(), 0);
    }
    lv_obj_center(icon);
}

void main_menu_create(void)
{
    lv_obj_t *menu = lv_obj_create(lv_screen_active());
    lv_obj_remove_flag(menu, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(menu, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_border_width(menu, 0, 0);
    lv_obj_set_style_radius(menu, 0, 0);
    lv_obj_set_style_bg_color(menu, lv_color_hex(0x000000), 0);

    lv_obj_t *background = lv_image_create(menu);
    lv_image_set_src(background, &abstract_timekeeper);
    lv_obj_center(background);

    lv_obj_t *button_container = lv_obj_create(menu);
    lv_obj_set_pos(button_container, 0, MENU_TOOLBAR_HEIGHT);
    lv_obj_set_size(button_container, LV_PCT(100), MENU_CONTENT_HEIGHT);
    lv_obj_set_style_bg_opa(button_container, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(button_container, 0, 0);
    lv_obj_set_style_radius(button_container, 0, 0);
    lv_obj_set_style_pad_all(button_container, MENU_PADDING, 0);
    lv_obj_set_style_pad_row(button_container, MENU_ROW_GAP, 0);
    lv_obj_set_style_pad_column(button_container, MENU_COLUMN_GAP, 0);
    lv_obj_set_scroll_dir(button_container, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(button_container, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_grid_dsc_array(button_container, menu_columns, menu_rows);
    lv_obj_set_grid_align(button_container, LV_GRID_ALIGN_CENTER, LV_GRID_ALIGN_CENTER);
    lv_obj_set_layout(button_container, LV_LAYOUT_GRID);

    for (uint32_t index = 0; index < MENU_BUTTON_COUNT; index++) {
        create_menu_button(button_container, index);
    }

    create_toolbar(menu);
}
