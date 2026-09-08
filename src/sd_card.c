#include "sd_card.h"

#include "bsp/esp-bsp.h"

static bool mounted;

esp_err_t sd_card_mount(void)
{
    if (mounted) {
        return ESP_OK;
    }

    esp_err_t result = bsp_sdcard_mount();
    if (result == ESP_OK) {
        mounted = true;
    }
    return result;
}

void sd_card_unmount(void)
{
    if (mounted) {
        bsp_sdcard_unmount();
        mounted = false;
    }
}

bool sd_card_is_mounted(void)
{
    return mounted;
}
