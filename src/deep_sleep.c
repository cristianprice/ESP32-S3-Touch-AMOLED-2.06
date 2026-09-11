#include "deep_sleep.h"

#include "driver/rtc_io.h"
#include "esp_check.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "bsp/display.h"
#include "time_mgmt.h"

#define DEEP_SLEEP_DELAY_MS 10000
#define BOOT_BUTTON_GPIO GPIO_NUM_0

/* A one-shot timer makes any touch or short power-button press restart inactivity. */
static TimerHandle_t sleep_timer;
static bool sleep_timer_paused;

static void sleep_timer_callback(TimerHandle_t timer)
{
    (void)timer;
    deep_sleep_immediately();
}

void deep_sleep_immediately(void)
{
    if (time_mgmt_is_initialized()) {
        ESP_ERROR_CHECK(time_mgmt_save());
    }
    ESP_ERROR_CHECK(bsp_display_backlight_off());
    /* GPIO0 is the boot button and the wake source after deep sleep begins. */
    ESP_ERROR_CHECK(rtc_gpio_pullup_en(BOOT_BUTTON_GPIO));
    ESP_ERROR_CHECK(rtc_gpio_pulldown_dis(BOOT_BUTTON_GPIO));
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_BUTTON_GPIO, 0));

    esp_deep_sleep_start();
}

void sleep_after_timeout(void)
{
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
