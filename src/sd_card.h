#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define SD_CARD_MOUNT_PATH "/sdcard"

/**
 * @brief Mount the BSP SD card at SD_CARD_MOUNT_PATH.
 *
 * Idempotent: a retained mount succeeds without remounting.
 * @return ESP_OK or the BSP mount error.
 */
esp_err_t sd_card_mount(void);
/** @brief Unmount the card if this module owns an active mount. */
void sd_card_unmount(void);
/** @brief Return whether sd_card_mount() completed successfully and was not unmounted. */
bool sd_card_is_mounted(void);
