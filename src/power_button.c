#include "power_button.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "deep_sleep.h"

/*
 * Owns the AXP2101 handle attached to the BSP's shared I2C bus. The PMIC
 * reports power-key events through registers, so a dedicated polling task
 * acknowledges the latched bit before requesting the global sleep path.
 */
#define AXP2101_ADDRESS 0x34
#define AXP2101_I2C_CLOCK_HZ 100000
#define AXP2101_REG_INTEN2 0x41
#define AXP2101_REG_INTSTS2 0x49
#define AXP2101_REG_BATTERY_PERCENT 0xA4
#define AXP2101_PKEY_SHORT_PRESS_BIT (1U << 3)
#define POWER_BUTTON_POLL_PERIOD_MS 100

static const char *const TAG = "power_button";
static i2c_master_dev_handle_t axp2101_device;

static esp_err_t read_register(uint8_t register_address, uint8_t *value)
{
    /* Register-address write followed by read is the PMIC's I2C transaction format. */
    return i2c_master_transmit_receive(
        axp2101_device, &register_address, sizeof(register_address), value, sizeof(*value), -1);
}

static esp_err_t write_register(uint8_t register_address, uint8_t value)
{
    const uint8_t payload[] = {register_address, value};
    return i2c_master_transmit(axp2101_device, payload, sizeof(payload), -1);
}

esp_err_t power_button_get_battery_percent(uint8_t *percent)
{
    if (percent == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (axp2101_device == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = read_register(AXP2101_REG_BATTERY_PERCENT, percent);
    if (result != ESP_OK) {
        return result;
    }
    if (*percent > 100) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    return ESP_OK;
}

static void power_button_task(void *argument)
{
    (void)argument;

    while (true) {
        uint8_t interrupt_status;
        esp_err_t result = read_register(AXP2101_REG_INTSTS2, &interrupt_status);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to read AXP2101 button status: %s", esp_err_to_name(result));
            vTaskDelay(pdMS_TO_TICKS(POWER_BUTTON_POLL_PERIOD_MS));
            continue;
        }

        if ((interrupt_status & AXP2101_PKEY_SHORT_PRESS_BIT) != 0) {
            /* The physical power button sleeps immediately, independent of inactivity state. */
            result = write_register(AXP2101_REG_INTSTS2, AXP2101_PKEY_SHORT_PRESS_BIT);
            if (result != ESP_OK) {
                ESP_LOGE(TAG, "Failed to clear AXP2101 button status: %s", esp_err_to_name(result));
            } else {
                deep_sleep_immediately();
            }
        }

        vTaskDelay(pdMS_TO_TICKS(POWER_BUTTON_POLL_PERIOD_MS));
    }
}

esp_err_t power_button_start(void)
{
    esp_err_t result = bsp_i2c_init();
    if (result != ESP_OK) {
        return result;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDRESS,
        .scl_speed_hz = AXP2101_I2C_CLOCK_HZ,
    };
    result = i2c_master_bus_add_device(bsp_i2c_get_handle(), &device_config, &axp2101_device);
    if (result != ESP_OK) {
        return result;
    }

    uint8_t interrupt_enable;
    result = read_register(AXP2101_REG_INTEN2, &interrupt_enable);
    if (result != ESP_OK) {
        return result;
    }

    result = write_register(
        AXP2101_REG_INTEN2, interrupt_enable | AXP2101_PKEY_SHORT_PRESS_BIT);
    if (result != ESP_OK) {
        return result;
    }

    result = write_register(AXP2101_REG_INTSTS2, AXP2101_PKEY_SHORT_PRESS_BIT);
    if (result != ESP_OK) {
        return result;
    }

    /*
     * Poll the PMIC because its power-key interrupt is exposed through I2C.
     * The task owns no LVGL objects and remains valid for the whole boot.
     */
    return xTaskCreate(
               power_button_task, "power_button", 2048, NULL, 5, NULL) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}
