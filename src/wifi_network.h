#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t wifi_network_start_ap(const char *ssid, const char *captive_portal_uri);
void wifi_network_stop(void);
bool wifi_network_has_valid_ip(void);
esp_err_t wifi_network_get_ip(char *buffer, size_t buffer_size);
