#include "ui_dispatch.h"

#include "esp_log.h"
#include "bsp/esp-bsp.h"

static const char *const TAG = "ui_dispatch";

lv_result_t ui_async_call(lv_async_cb_t callback, void *user_data)
{
    /*
     * bsp_display_lock() and LVGL's display task share a recursive mutex.
     * Recursive ownership keeps event callbacks safe while worker tasks gain
     * the same protection before they append to LVGL's async callback list.
     */
    if (!bsp_display_lock(0)) {
        ESP_LOGE(TAG, "Unable to acquire the LVGL mutex");
        return LV_RESULT_INVALID;
    }

    lv_result_t result = lv_async_call(callback, user_data);
    bsp_display_unlock();
    return result;
}
