#pragma once

#include <stdbool.h>

#include "esp_err.h"

#define SD_CARD_MOUNT_PATH "/sdcard"

esp_err_t sd_card_mount(void);
void sd_card_unmount(void);
bool sd_card_is_mounted(void);
