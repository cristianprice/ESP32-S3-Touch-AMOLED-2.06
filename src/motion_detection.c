#include "motion_detection.h"

#include <stdbool.h>
#include <stdlib.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "lvgl.h"
#include "deep_sleep.h"
#include "main_menu.h"
#include "nvs_flash.h"
#include "ui_theme.h"

#define CSI_SAMPLE_QUEUE_LENGTH 32
#define CSI_CHART_POINT_COUNT 60
#define CSI_CHART_MAX_VALUE 100
#define CSI_REFRESH_PERIOD_MS 100
#define WIFI_CHANNEL_COUNT 11
#define WIFI_CHANNEL_SWITCH_TICKS 5

/*
 * CSI arrives in the Wi-Fi driver context. The callback only places scalar
 * samples on a queue; the LVGL timer is the sole consumer that updates the UI.
 */
static const char *const TAG = "motion_detection";
static QueueHandle_t csi_sample_queue;
static lv_timer_t *chart_refresh_timer;
static lv_obj_t *chart;
static lv_chart_series_t *chart_series;
static lv_obj_t *status_label;
static bool nvs_initialized;
static bool wifi_initialized;
static bool wifi_started;
static bool promiscuous_enabled;
static bool csi_callback_registered;
static bool csi_enabled;
static uint16_t previous_csi_magnitude;
static uint8_t wifi_channel = 1;
static uint8_t channel_switch_ticks;

static void log_cleanup_error(const char *operation, esp_err_t result)
{
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", operation, esp_err_to_name(result));
    }
}

static void motion_detection_stop(void)
{
    /* Tear down in reverse order so no callback can outlive its queue or chart. */
    if (chart_refresh_timer != NULL) {
        lv_timer_delete(chart_refresh_timer);
        chart_refresh_timer = NULL;
    }

    if (csi_callback_registered) {
        log_cleanup_error("Failed to unregister CSI callback", esp_wifi_set_csi_rx_cb(NULL, NULL));
        csi_callback_registered = false;
    }
    if (csi_enabled) {
        log_cleanup_error("Failed to disable CSI", esp_wifi_set_csi(false));
        csi_enabled = false;
    }
    if (promiscuous_enabled) {
        log_cleanup_error("Failed to disable promiscuous mode", esp_wifi_set_promiscuous(false));
        promiscuous_enabled = false;
    }
    if (wifi_started) {
        log_cleanup_error("Failed to stop Wi-Fi", esp_wifi_stop());
        wifi_started = false;
    }
    if (wifi_initialized) {
        log_cleanup_error("Failed to deinitialize Wi-Fi", esp_wifi_deinit());
        wifi_initialized = false;
    }
    if (nvs_initialized) {
        log_cleanup_error("Failed to deinitialize NVS", nvs_flash_deinit());
        nvs_initialized = false;
    }
    if (csi_sample_queue != NULL) {
        vQueueDelete(csi_sample_queue);
        csi_sample_queue = NULL;
    }

    previous_csi_magnitude = 0;
    wifi_channel = 1;
    channel_switch_ticks = 0;
}

static void csi_received(void *context, wifi_csi_info_t *data)
{
    (void)context;

    if (data == NULL || data->buf == NULL || data->len < 2 || csi_sample_queue == NULL) {
        return;
    }

    size_t start = data->first_word_invalid && data->len >= 6 ? 4 : 0;
    uint32_t total_magnitude = 0;
    size_t complex_sample_count = 0;
    for (size_t index = start; index + 1 < data->len; index += 2) {
        total_magnitude += abs((int)data->buf[index]) + abs((int)data->buf[index + 1]);
        complex_sample_count++;
    }
    if (complex_sample_count == 0) {
        return;
    }

    /* Average I/Q magnitude gives a lightweight, uncalibrated motion signal. */
    uint16_t magnitude = total_magnitude / (2 * complex_sample_count);
    xQueueSend(csi_sample_queue, &magnitude, 0);
}

static void update_chart(lv_timer_t *timer)
{
    (void)timer;

    channel_switch_ticks++;
    if (channel_switch_ticks == WIFI_CHANNEL_SWITCH_TICKS) {
        channel_switch_ticks = 0;
        esp_err_t result = esp_wifi_set_channel(wifi_channel, WIFI_SECOND_CHAN_NONE);
        if (result != ESP_OK) {
            ESP_LOGW(TAG, "Failed to switch to Wi-Fi channel %u: %s", wifi_channel,
                     esp_err_to_name(result));
        }
        wifi_channel = wifi_channel % WIFI_CHANNEL_COUNT + 1;
    }

    /* Drain queued samples and plot the most recent change between magnitudes. */
    uint16_t magnitude;
    bool received_sample = false;
    uint16_t motion_value = 0;
    while (xQueueReceive(csi_sample_queue, &magnitude, 0) == pdTRUE) {
        motion_value = previous_csi_magnitude > magnitude
                           ? previous_csi_magnitude - magnitude
                           : magnitude - previous_csi_magnitude;
        previous_csi_magnitude = magnitude;
        received_sample = true;
    }

    if (!received_sample) {
        return;
    }

    if (motion_value > CSI_CHART_MAX_VALUE) {
        motion_value = CSI_CHART_MAX_VALUE;
    }
    lv_chart_set_next_value(chart, chart_series, motion_value);
    lv_label_set_text_fmt(status_label, "CSI change: %u", motion_value);
}

static esp_err_t motion_detection_start(void)
{
    csi_sample_queue = xQueueCreate(CSI_SAMPLE_QUEUE_LENGTH, sizeof(uint16_t));
    if (csi_sample_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) {
        goto fail;
    }
    nvs_initialized = true;

    wifi_init_config_t wifi_config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&wifi_config);
    if (result != ESP_OK) {
        goto fail;
    }
    wifi_initialized = true;

    /*
     * Promiscuous station mode receives nearby packets without joining an AP.
     * Channel hopping makes the chart useful in environments with unknown APs.
     */
    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK) {
        goto fail;
    }
    result = esp_wifi_start();
    if (result != ESP_OK) {
        goto fail;
    }
    wifi_started = true;

    result = esp_wifi_set_ps(WIFI_PS_NONE);
    if (result != ESP_OK) {
        goto fail;
    }
    result = esp_wifi_set_promiscuous(true);
    if (result != ESP_OK) {
        goto fail;
    }
    promiscuous_enabled = true;

    const wifi_csi_config_t csi_config = {
        .lltf_en = true,
        .htltf_en = true,
        .stbc_htltf2_en = true,
        .ltf_merge_en = true,
        .channel_filter_en = true,
        .manu_scale = false,
        .shift = 0,
        .dump_ack_en = false,
    };
    result = esp_wifi_set_csi_config(&csi_config);
    if (result != ESP_OK) {
        goto fail;
    }
    result = esp_wifi_set_csi_rx_cb(csi_received, NULL);
    if (result != ESP_OK) {
        goto fail;
    }
    csi_callback_registered = true;
    result = esp_wifi_set_csi(true);
    if (result != ESP_OK) {
        goto fail;
    }
    csi_enabled = true;

    chart_refresh_timer = lv_timer_create(update_chart, CSI_REFRESH_PERIOD_MS, NULL);
    if (chart_refresh_timer == NULL) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    return ESP_OK;

fail:
    motion_detection_stop();
    return result;
}

static void back_to_main_menu(void *user_data)
{
    (void)user_data;

    motion_detection_stop();
    lv_obj_clean(lv_screen_active());
    main_menu_create();
    sleep_timer_resume();
}

static void request_main_menu(lv_event_t *event)
{
    (void)event;

    lv_async_call(back_to_main_menu, NULL);
}

void motion_detection_create(void)
{
    /* Prevent the inactivity timer from powering down during observation. */
    sleep_timer_pause();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_invalidate(screen);

    lv_obj_t *background = lv_obj_create(screen);
    lv_obj_remove_flag(background, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(background, LV_PCT(100), LV_PCT(100));
    lv_obj_align(background, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(background, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(background, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(background, 0, 0);
    lv_obj_set_style_radius(background, 0, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Motion detection");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "Collecting CSI data...");
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 52);

    chart = lv_chart_create(screen);
    lv_obj_set_size(chart, 350, 250);
    lv_obj_align(chart, LV_ALIGN_CENTER, 0, 28);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, CSI_CHART_POINT_COUNT);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, CSI_CHART_MAX_VALUE);
    lv_chart_set_div_line_count(chart, 5, 6);
    chart_series = lv_chart_add_series(chart, lv_palette_main(LV_PALETTE_GREEN),
                                       LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(chart, chart_series, 0);

    lv_obj_t *back_button = lv_button_create(screen);
    lv_obj_set_size(back_button, 110, 44);
    ui_theme_apply_button(back_button);
    lv_obj_align(back_button, LV_ALIGN_BOTTOM_LEFT, 52, -32);
    lv_obj_add_event_cb(back_button, request_main_menu, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);

    /* Flush the complete view before Wi-Fi startup can delay the LVGL task. */
    lv_obj_invalidate(screen);
    lv_refr_now(NULL);

    esp_err_t result = motion_detection_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start CSI collection: %s", esp_err_to_name(result));
        lv_label_set_text_fmt(status_label, "CSI unavailable: %s", esp_err_to_name(result));
    } else {
        lv_label_set_text(status_label, "Listening for CSI data...");
    }
}
