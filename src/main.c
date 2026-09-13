#include <assert.h>

#include "esp_check.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "deep_sleep.h"
#include "main_menu.h"
#include "power_button.h"
#include "time_mgmt.h"
#include "ui_dispatch.h"

/*
 * Application composition root. It establishes the BSP display and initial
 * clock screen, then hands screen mutations to LVGL events/async callbacks.
 * The display task runs on core 1 while radio/network work uses core 0.
 */
/*
 * The display service runs LVGL independently of app_main. Keep its task on
 * core 1: Wi-Fi, the Settings portal, and their event work are pinned to core 0.
 */
static bool main_menu_open;

static void open_main_menu(void *user_data)
{
    (void)user_data;

    sleep_timer_reset();

    if (main_menu_open) {
        return;
    }

    main_menu_open = true;
    time_mgmt_stop();
    lv_obj_clean(lv_screen_active());
    main_menu_create();
}

static void request_main_menu(lv_event_t *event)
{
    (void)event;

    /* Input callbacks defer destruction of the current screen until LVGL's safe queue. */
    ui_async_call(open_main_menu, NULL);
}

void app_main(void)
{
    /* Start the board display with the same BSP defaults, except for core affinity. */
    bsp_display_cfg_t display_config = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_DRAW_BUFF_SIZE,
        .double_buffer = BSP_LCD_DRAW_BUFF_DOUBLE,
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
        },
    };
    display_config.lvgl_port_cfg.task_affinity = 1;

    bsp_display_start_with_config(&display_config);
    bsp_display_lock(0);

    ESP_ERROR_CHECK(time_mgmt_start());
    sleep_after_timeout();

    /* The first touch replaces the clock screen with the main menu. */
    lv_indev_t *touch_input = bsp_display_get_input_dev();
    assert(touch_input != NULL);
    lv_indev_add_event_cb(touch_input, request_main_menu, LV_EVENT_PRESSED, NULL);

    bsp_display_unlock();

    ESP_ERROR_CHECK(power_button_start());
}