#include "steps_meter.h"

#include <stdbool.h>
#include <inttypes.h>
#include <math.h>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "assets/running_icon.h"
#include "deep_sleep.h"
#include "main_menu.h"
#include "ui_theme.h"

#define QMI8658_ADDRESS 0x6B
#define QMI8658_I2C_CLOCK_HZ 100000
#define QMI8658_WHO_AM_I_REGISTER 0x00
#define QMI8658_WHO_AM_I_VALUE 0x05
#define QMI8658_CTRL1_REGISTER 0x02
#define QMI8658_CTRL2_REGISTER 0x03
#define QMI8658_CTRL5_REGISTER 0x06
#define QMI8658_CTRL7_REGISTER 0x08
#define QMI8658_ACCELERATION_REGISTER 0x35
#define QMI8658_RESET_REGISTER 0x60
#define QMI8658_RESET_VALUE 0xB0
#define QMI8658_CTRL1_AUTO_INCREMENT 0x40
#define QMI8658_CTRL2_ACCEL_4G_125HZ 0x16
#define QMI8658_CTRL5_ACCEL_LPF_ENABLE 0x01
#define QMI8658_CTRL7_ACCEL_ENABLE 0x01
#define QMI8658_ACCEL_SCALE_G (4.0f / 32768.0f)
#define SAMPLE_PERIOD_MS 50
#define QMI8658_TRANSACTION_TIMEOUT_MS 25
#define STEP_MINIMUM_INTERVAL_US 250000
#define STEP_MAXIMUM_INTERVAL_US 2000000
#define STEP_HIGH_THRESHOLD_G 0.12f
#define STEP_LOW_THRESHOLD_G 0.06f
#define ESTIMATED_STEP_LENGTH_METERS 0.70f

static const char *const TAG = "steps_meter";
static lv_timer_t *elapsed_timer;
static lv_obj_t *elapsed_label;
static lv_obj_t *speed_label;
static lv_obj_t *steps_label;
static lv_obj_t *status_label;
static lv_obj_t *step_pulse_label;
static int64_t session_started_at_us;
static int64_t last_step_at_us;
static uint32_t total_steps;
static float gravity_magnitude_g;
static float current_speed_kmh;
static bool step_peak_detected;
static i2c_master_dev_handle_t qmi8658_device;

static void set_pulse_opacity(void *object, int32_t value)
{
    lv_obj_set_style_opa(object, value, 0);
}

static void set_pulse_zoom(void *object, int32_t value)
{
    lv_obj_set_style_transform_zoom(object, value, 0);
}

static void animate_step_registration(void)
{
    lv_anim_delete(step_pulse_label, set_pulse_opacity);
    lv_anim_delete(step_pulse_label, set_pulse_zoom);
    lv_obj_set_style_opa(step_pulse_label, LV_OPA_COVER, 0);
    lv_obj_set_style_transform_zoom(step_pulse_label, 256, 0);

    lv_anim_t fade_animation;
    lv_anim_init(&fade_animation);
    lv_anim_set_var(&fade_animation, step_pulse_label);
    lv_anim_set_exec_cb(&fade_animation, set_pulse_opacity);
    lv_anim_set_values(&fade_animation, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&fade_animation, 500);
    lv_anim_set_path_cb(&fade_animation, lv_anim_path_ease_out);
    lv_anim_start(&fade_animation);

    lv_anim_t zoom_animation;
    lv_anim_init(&zoom_animation);
    lv_anim_set_var(&zoom_animation, step_pulse_label);
    lv_anim_set_exec_cb(&zoom_animation, set_pulse_zoom);
    lv_anim_set_values(&zoom_animation, 256, 360);
    lv_anim_set_duration(&zoom_animation, 180);
    lv_anim_set_playback_duration(&zoom_animation, 320);
    lv_anim_set_path_cb(&zoom_animation, lv_anim_path_ease_out);
    lv_anim_start(&zoom_animation);
}

static esp_err_t qmi8658_write_register(uint8_t register_address, uint8_t value)
{
    const uint8_t payload[] = {register_address, value};
    return i2c_master_transmit(qmi8658_device, payload, sizeof(payload),
                               QMI8658_TRANSACTION_TIMEOUT_MS);
}

static esp_err_t qmi8658_read_acceleration(float *x, float *y, float *z)
{
    if (x == NULL || y == NULL || z == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t values[6];
    esp_err_t result = i2c_master_transmit_receive(
        qmi8658_device, &(uint8_t){QMI8658_ACCELERATION_REGISTER}, 1, values, sizeof(values),
        QMI8658_TRANSACTION_TIMEOUT_MS);
    if (result != ESP_OK) {
        return result;
    }

    *x = (float)(int16_t)((uint16_t)values[1] << 8 | values[0]) * QMI8658_ACCEL_SCALE_G;
    *y = (float)(int16_t)((uint16_t)values[3] << 8 | values[2]) * QMI8658_ACCEL_SCALE_G;
    *z = (float)(int16_t)((uint16_t)values[5] << 8 | values[4]) * QMI8658_ACCEL_SCALE_G;
    return ESP_OK;
}

static void qmi8658_stop(void)
{
    if (qmi8658_device == NULL) {
        return;
    }

    esp_err_t result = qmi8658_write_register(QMI8658_CTRL7_REGISTER, 0);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to disable QMI8658: %s", esp_err_to_name(result));
    }
    result = i2c_master_bus_rm_device(qmi8658_device);
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Failed to remove QMI8658 from I2C bus: %s", esp_err_to_name(result));
    }
    qmi8658_device = NULL;
}

static esp_err_t qmi8658_start(void)
{
    esp_err_t result = bsp_i2c_init();
    if (result != ESP_OK) {
        return result;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = QMI8658_ADDRESS,
        .scl_speed_hz = QMI8658_I2C_CLOCK_HZ,
    };
    result = i2c_master_bus_add_device(bsp_i2c_get_handle(), &device_config, &qmi8658_device);
    if (result != ESP_OK) {
        return result;
    }

    result = qmi8658_write_register(QMI8658_RESET_REGISTER, QMI8658_RESET_VALUE);
    if (result != ESP_OK) {
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t who_am_i;
    result = i2c_master_transmit_receive(
        qmi8658_device, &(uint8_t){QMI8658_WHO_AM_I_REGISTER}, 1, &who_am_i, sizeof(who_am_i),
        QMI8658_TRANSACTION_TIMEOUT_MS);
    if (result != ESP_OK) {
        goto fail;
    }
    if (who_am_i != QMI8658_WHO_AM_I_VALUE) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto fail;
    }

    result = qmi8658_write_register(QMI8658_CTRL1_REGISTER, QMI8658_CTRL1_AUTO_INCREMENT);
    if (result != ESP_OK) {
        goto fail;
    }
    result = qmi8658_write_register(QMI8658_CTRL2_REGISTER, QMI8658_CTRL2_ACCEL_4G_125HZ);
    if (result != ESP_OK) {
        goto fail;
    }
    result = qmi8658_write_register(QMI8658_CTRL5_REGISTER, QMI8658_CTRL5_ACCEL_LPF_ENABLE);
    if (result != ESP_OK) {
        goto fail;
    }
    result = qmi8658_write_register(QMI8658_CTRL7_REGISTER, QMI8658_CTRL7_ACCEL_ENABLE);
    if (result != ESP_OK) {
        goto fail;
    }

    return ESP_OK;

fail:
    qmi8658_stop();
    return result;
}

static void update_elapsed_time(lv_timer_t *timer)
{
    (void)timer;

    float x;
    float y;
    float z;
    esp_err_t result = qmi8658_read_acceleration(&x, &y, &z);
    if (result != ESP_OK) {
        lv_label_set_text_fmt(status_label, "Sensor read failed: %s", esp_err_to_name(result));
        return;
    }

    float magnitude_g = sqrtf(x * x + y * y + z * z);
    if (gravity_magnitude_g == 0.0f) {
        gravity_magnitude_g = magnitude_g;
    }
    gravity_magnitude_g = gravity_magnitude_g * 0.9f + magnitude_g * 0.1f;
    float linear_acceleration_g = fabsf(magnitude_g - gravity_magnitude_g);
    int64_t now_us = esp_timer_get_time();
    bool step_registered = false;

    if (!step_peak_detected && linear_acceleration_g >= STEP_HIGH_THRESHOLD_G) {
        int64_t step_interval_us = now_us - last_step_at_us;
        if (last_step_at_us == 0 || step_interval_us > STEP_MAXIMUM_INTERVAL_US) {
            total_steps++;
            last_step_at_us = now_us;
            current_speed_kmh = 0.0f;
            step_registered = true;
        } else if (step_interval_us >= STEP_MINIMUM_INTERVAL_US) {
            total_steps++;
            last_step_at_us = now_us;
            current_speed_kmh =
                ESTIMATED_STEP_LENGTH_METERS * 3600000.0f / (float)step_interval_us;
            step_registered = true;
        }
        step_peak_detected = true;
    } else if (step_peak_detected && linear_acceleration_g <= STEP_LOW_THRESHOLD_G) {
        step_peak_detected = false;
    }

    if (step_registered) {
        animate_step_registration();
    }

    if (last_step_at_us != 0 && now_us - last_step_at_us > STEP_MAXIMUM_INTERVAL_US) {
        current_speed_kmh = 0.0f;
    }
    uint32_t speed_tenths = (uint32_t)(current_speed_kmh * 10.0f + 0.5f);
    lv_label_set_text_fmt(speed_label, "Speed: %" PRIu32 ".%" PRIu32 " km/h",
                          speed_tenths / 10, speed_tenths % 10);
    lv_label_set_text_fmt(steps_label, "Steps: %" PRIu32, total_steps);

    int64_t elapsed_seconds = (esp_timer_get_time() - session_started_at_us) / 1000000;
    int64_t hours = elapsed_seconds / 3600;
    int64_t minutes = (elapsed_seconds % 3600) / 60;
    int64_t seconds = elapsed_seconds % 60;
    lv_label_set_text_fmt(elapsed_label, "Time: %02" PRId64 ":%02" PRId64 ":%02" PRId64,
                          hours, minutes, seconds);
}

static void back_to_main_menu(void *user_data)
{
    (void)user_data;

    if (elapsed_timer != NULL) {
        lv_timer_delete(elapsed_timer);
        elapsed_timer = NULL;
    }
    lv_anim_delete(step_pulse_label, set_pulse_opacity);
    lv_anim_delete(step_pulse_label, set_pulse_zoom);
    qmi8658_stop();

    lv_obj_clean(lv_screen_active());
    main_menu_create();
    sleep_timer_resume();
}

static void request_main_menu(lv_event_t *event)
{
    (void)event;

    lv_async_call(back_to_main_menu, NULL);
}

static lv_obj_t *create_measurement_label(lv_obj_t *parent, const char *text, lv_coord_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
    lv_obj_align(label, LV_ALIGN_TOP_MID, 0, y);
    return label;
}

void steps_meter_create(void)
{
    /* Keep the live session visible until the user explicitly returns to the menu. */
    sleep_timer_pause();
    session_started_at_us = esp_timer_get_time();

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Steps meter");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

    speed_label = create_measurement_label(screen, "Speed: 0.0 km/h", 116);
    steps_label = create_measurement_label(screen, "Steps: 0", 170);
    elapsed_label = create_measurement_label(screen, "Time: 00:00:00", 224);

    status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "Starting QMI8658...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFB35C), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 278);

    step_pulse_label = lv_image_create(screen);
    lv_image_set_src(step_pulse_label, &running_icon);
    lv_obj_set_style_opa(step_pulse_label, LV_OPA_TRANSP, 0);
    lv_obj_align(step_pulse_label, LV_ALIGN_TOP_MID, 0, 318);

    lv_obj_t *back_button = lv_button_create(screen);
    lv_obj_set_size(back_button, 110, 44);
    ui_theme_apply_button(back_button);
    lv_obj_align(back_button, LV_ALIGN_BOTTOM_LEFT, 52, -32);
    lv_obj_add_event_cb(back_button, request_main_menu, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);

    esp_err_t result = qmi8658_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start QMI8658: %s", esp_err_to_name(result));
        lv_label_set_text_fmt(status_label, "IMU unavailable: %s", esp_err_to_name(result));
        return;
    }

    total_steps = 0;
    last_step_at_us = 0;
    gravity_magnitude_g = 0.0f;
    current_speed_kmh = 0.0f;
    step_peak_detected = false;
    lv_label_set_text(status_label, "QMI8658 active");
    elapsed_timer = lv_timer_create(update_elapsed_time, SAMPLE_PERIOD_MS, NULL);
}
