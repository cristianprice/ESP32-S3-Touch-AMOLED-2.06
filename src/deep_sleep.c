#include "deep_sleep.h"

#include "driver/rtc_io.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "lvgl.h"
#include "bsp/display.h"
#include "time_mgmt.h"

/*
 * Owns the global inactivity timer and the irreversible transition to deep
 * sleep. It coordinates clock persistence, panel shutdown, and GPIO wake
 * configuration; none of these APIs return after esp_deep_sleep_start().
 */
#define DEEP_SLEEP_DELAY_MS 10000
#define BOOT_BUTTON_GPIO GPIO_NUM_0
#define SH8601_SLEEP_IN_COMMAND 0x10
#define SH8601_COMMAND_PREFIX (0x02U << 24)
#define SH8601_SLEEP_IN_DELAY_MS 120

/*
 * esp_lvgl_port stores this context as the LVGL display driver data. The BSP
 * does not expose its SH8601 panel handle, so retain only the stable leading
 * fields needed to issue display-off and sleep-in commands before deep sleep.
 */
typedef struct {
    int display_type;
    esp_lcd_panel_io_handle_t io_handle;
    esp_lcd_panel_handle_t panel_handle;
} lvgl_port_display_context_prefix_t;

/* A one-shot timer makes any touch or short power-button press restart inactivity. */
static TimerHandle_t sleep_timer;
static bool sleep_timer_paused;

static esp_err_t enter_display_sleep(void)
{
    lv_display_t *display = lv_display_get_default();
    if (display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    lvgl_port_display_context_prefix_t *context = lv_display_get_driver_data(display);
    if (context == NULL || context->io_handle == NULL || context->panel_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = esp_lcd_panel_disp_on_off(context->panel_handle, false);
    if (result != ESP_OK) {
        return result;
    }

    /*
     * SH8601 uses a QSPI command prefix. This is the same wire encoding the
     * board BSP uses for brightness commands; 0x10 is the standard DCS sleep-in
     * command and requires 120 ms before power-down.
     */
    uint32_t command = SH8601_SLEEP_IN_COMMAND << 8 | SH8601_COMMAND_PREFIX;
    result = esp_lcd_panel_io_tx_param(context->io_handle, command, NULL, 0);
    if (result != ESP_OK) {
        return result;
    }
    vTaskDelay(pdMS_TO_TICKS(SH8601_SLEEP_IN_DELAY_MS));
    return ESP_OK;
}

static void sleep_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    /* FreeRTOS executes this in its timer-service task, not in LVGL context. */
    deep_sleep_immediately();
}

void deep_sleep_immediately(void)
{
    if (time_mgmt_is_initialized()) {
        ESP_ERROR_CHECK(time_mgmt_save());
    }
    ESP_ERROR_CHECK(bsp_display_backlight_off());
    ESP_ERROR_CHECK(enter_display_sleep());
    /* GPIO0 is the boot button and the wake source after deep sleep begins. */
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(BOOT_BUTTON_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BOOT_BUTTON_GPIO));
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0));

    esp_deep_sleep_start();
}

void sleep_after_timeout(void)
{
    /* Auto-reload is disabled: each user action explicitly begins a new interval. */
    sleep_timer = xTimerCreate(
        "sleep_timer", pdMS_TO_TICKS(DEEP_SLEEP_DELAY_MS), pdFALSE, NULL, sleep_timer_callback);
    configASSERT(sleep_timer != NULL);
    configASSERT(xTimerStart(sleep_timer, 0) == pdPASS);
}

void sleep_timer_reset(void)
{
    configASSERT(sleep_timer != NULL);
    if (!sleep_timer_paused) {
        configASSERT(xTimerReset(sleep_timer, 0) == pdPASS);
    }
}

void sleep_timer_pause(void)
{
    /* Full-screen tools own their lifetime and must not sleep while active. */
    configASSERT(sleep_timer != NULL);
    configASSERT(xTimerStop(sleep_timer, 0) == pdPASS);
    sleep_timer_paused = true;
}

void sleep_timer_resume(void)
{
    configASSERT(sleep_timer != NULL);
    sleep_timer_paused = false;
    configASSERT(xTimerReset(sleep_timer, 0) == pdPASS);
}
