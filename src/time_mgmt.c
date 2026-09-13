#include "time_mgmt.h"

#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "assets/abstract_timekeeper.h"

/*
 * Owns the PCF85063 device on the shared BSP I2C bus and the initial clock
 * screen's LVGL timer. The RTC stores calendar fields in BCD; conversion and
 * validation stay here so other modules exchange ordinary UTC timestamps.
 */
/* PCF85063 RTC register layout and I2C bus parameters. */
#define PCF85063_ADDRESS 0x51
#define PCF85063_I2C_CLOCK_HZ 100000
#define PCF85063_REG_SECONDS 0x04
#define PCF85063_DATETIME_SIZE 7

typedef struct
{
    uint8_t seconds;
    uint8_t minutes;
    uint8_t hours;
    uint8_t day;
    uint8_t weekday;
    uint8_t month;
    uint16_t year;
} rtc_datetime_t;

static const char *const TAG = "time_mgmt";
static const char *const weekday_names[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static i2c_master_dev_handle_t rtc_device;
static lv_obj_t *time_label;
static lv_obj_t *date_label;
static lv_timer_t *clock_timer;

bool time_mgmt_is_initialized(void)
{
    return rtc_device != NULL;
}

static uint8_t bcd_to_decimal(uint8_t value)
{
    return ((value >> 4) * 10) + (value & 0x0F);
}

static uint8_t decimal_to_bcd(uint8_t value)
{
    return ((value / 10) << 4) | (value % 10);
}

static uint8_t weekday_from_date(uint16_t year, uint8_t month, uint8_t day)
{
    /* Sakamoto's Gregorian-calendar offsets yield Sunday == 0, matching the RTC/UI. */
    static const uint8_t month_offsets[] = {
        0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4
    };

    if (month < 3) {
        year--;
    }

    return (year + year / 4 - year / 100 + year / 400 + month_offsets[month - 1] + day) % 7;
}

static bool datetime_is_valid(const rtc_datetime_t *datetime)
{
    return datetime->seconds < 60 && datetime->minutes < 60 && datetime->hours < 24 &&
           datetime->day >= 1 && datetime->day <= 31 && datetime->month >= 1 &&
           datetime->month <= 12 && datetime->weekday <= 6;
}

static uint8_t build_month(void)
{
    static const char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";

    for (uint8_t month = 0; month < 12; month++)
    {
        const char *month_name = &months[month * 3];
        if (__DATE__[0] == month_name[0] && __DATE__[1] == month_name[1] &&
            __DATE__[2] == month_name[2])
        {
            return month + 1;
        }
    }

    return 1;
}

static rtc_datetime_t build_datetime(void)
{
    /* Seed a stopped or invalid RTC from the firmware build timestamp. */
    rtc_datetime_t datetime = {
        .seconds = (__TIME__[6] - '0') * 10 + (__TIME__[7] - '0'),
        .minutes = (__TIME__[3] - '0') * 10 + (__TIME__[4] - '0'),
        .hours = (__TIME__[0] - '0') * 10 + (__TIME__[1] - '0'),
        .day = (__DATE__[4] == ' ' ? 0 : (__DATE__[4] - '0') * 10) + (__DATE__[5] - '0'),
        .month = build_month(),
        .year = (__DATE__[7] - '0') * 1000 + (__DATE__[8] - '0') * 100 +
                (__DATE__[9] - '0') * 10 + (__DATE__[10] - '0'),
    };

    datetime.weekday = weekday_from_date(datetime.year, datetime.month, datetime.day);
    return datetime;
}

static esp_err_t write_datetime(const rtc_datetime_t *datetime)
{
    /* The RTC auto-increments from seconds, so one write updates the full calendar. */
    const uint8_t payload[PCF85063_DATETIME_SIZE + 1] = {
        PCF85063_REG_SECONDS,
        decimal_to_bcd(datetime->seconds),
        decimal_to_bcd(datetime->minutes),
        decimal_to_bcd(datetime->hours),
        decimal_to_bcd(datetime->day),
        datetime->weekday,
        decimal_to_bcd(datetime->month),
        decimal_to_bcd(datetime->year % 100),
    };

    return i2c_master_transmit(rtc_device, payload, sizeof(payload), -1);
}

static esp_err_t read_datetime(rtc_datetime_t *datetime, uint8_t *raw_datetime)
{
    uint8_t register_address = PCF85063_REG_SECONDS;
    uint8_t values[PCF85063_DATETIME_SIZE];
    esp_err_t result = i2c_master_transmit_receive(
        rtc_device, &register_address, sizeof(register_address), values, sizeof(values), -1);
    if (result != ESP_OK)
    {
        return result;
    }

    /* The oscillator-stop bit means time is unreliable until reseeded. */
    if ((values[0] & 0x80) != 0)
    {
        return ESP_ERR_INVALID_STATE;
    }

    datetime->seconds = bcd_to_decimal(values[0] & 0x7F);
    datetime->minutes = bcd_to_decimal(values[1] & 0x7F);
    datetime->hours = bcd_to_decimal(values[2] & 0x3F);
    datetime->day = bcd_to_decimal(values[3] & 0x3F);
    datetime->weekday = values[4] & 0x07;
    datetime->month = bcd_to_decimal(values[5] & 0x1F);
    datetime->year = 2000 + bcd_to_decimal(values[6]);

    if (!datetime_is_valid(datetime))
    {
        return ESP_ERR_INVALID_STATE;
    }

    if (raw_datetime != NULL)
    {
        for (size_t index = 0; index < sizeof(values); index++)
        {
            raw_datetime[index] = values[index];
        }
    }

    return ESP_OK;
}

static void update_clock_labels(void)
{
    /* Called only by initialization and the LVGL timer, never from an I2C callback. */
    rtc_datetime_t datetime;
    esp_err_t result = read_datetime(&datetime, NULL);
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to read PCF85063: %s", esp_err_to_name(result));
        lv_label_set_text(time_label, "--:--");
        lv_label_set_text(date_label, "Date unavailable");
        return;
    }

    lv_label_set_text_fmt(
        time_label, "%02u:%02u:%02u", datetime.hours, datetime.minutes, datetime.seconds);
    lv_label_set_text_fmt(
        date_label, "%s %02u-%02u-%04u", weekday_names[datetime.weekday], datetime.day,
        datetime.month, datetime.year);
}

static void clock_timer_callback(lv_timer_t *timer)
{
    (void)timer;
    update_clock_labels();
}

esp_err_t time_mgmt_start(void)
{
    esp_err_t result = bsp_i2c_init();
    if (result != ESP_OK)
    {
        return result;
    }

    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_ADDRESS,
        .scl_speed_hz = PCF85063_I2C_CLOCK_HZ,
    };
    result = i2c_master_bus_add_device(bsp_i2c_get_handle(), &device_config, &rtc_device);
    if (result != ESP_OK)
    {
        return result;
    }

    rtc_datetime_t datetime;
    result = read_datetime(&datetime, NULL);
    if (result == ESP_ERR_INVALID_STATE)
    {
        /* A stopped oscillator or malformed calendar is replaced with the build time. */
        datetime = build_datetime();
        result = write_datetime(&datetime);
    }
    if (result != ESP_OK)
    {
        return result;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x061826), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x102C42), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);

    lv_obj_t *background = lv_image_create(screen);
    lv_image_set_src(background, &abstract_timekeeper);
    lv_obj_center(background);

    time_label = lv_label_create(screen);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_align(time_label, LV_ALIGN_CENTER, 0, -50);

    date_label = lv_label_create(screen);
    lv_obj_set_style_text_font(date_label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(date_label, lv_color_white(), 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 50);

    update_clock_labels();
    /* LVGL owns UI updates; refreshing once per second keeps I2C use minimal. */
    clock_timer = lv_timer_create(clock_timer_callback, 1000, NULL);

    return ESP_OK;
}

esp_err_t time_mgmt_save(void)
{
    if (!time_mgmt_is_initialized())
    {
        return ESP_ERR_INVALID_STATE;
    }

    rtc_datetime_t datetime;
    esp_err_t result = read_datetime(&datetime, NULL);
    if (result != ESP_OK)
    {
        return result;
    }

    return write_datetime(&datetime);
}

esp_err_t time_mgmt_set_utc(time_t utc_time)
{
    if (!time_mgmt_is_initialized()) {
        return ESP_ERR_INVALID_STATE;
    }

    struct tm utc_datetime;
    if (gmtime_r(&utc_time, &utc_datetime) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    rtc_datetime_t datetime = {
        .seconds = utc_datetime.tm_sec,
        .minutes = utc_datetime.tm_min,
        .hours = utc_datetime.tm_hour,
        .day = utc_datetime.tm_mday,
        .weekday = utc_datetime.tm_wday,
        .month = utc_datetime.tm_mon + 1,
        .year = utc_datetime.tm_year + 1900,
    };
    return datetime_is_valid(&datetime) ? write_datetime(&datetime) : ESP_ERR_INVALID_ARG;
}

void time_mgmt_stop(void)
{
    if (clock_timer != NULL) {
        lv_timer_delete(clock_timer);
        clock_timer = NULL;
    }

    time_label = NULL;
    date_label = NULL;
}
