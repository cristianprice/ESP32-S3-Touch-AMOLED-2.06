#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"

esp_err_t wifi_network_start_ap(const char *ssid, const char *captive_portal_uri);
esp_err_t wifi_network_start_station(void);
esp_err_t wifi_network_scan(wifi_ap_record_t *records, uint16_t *record_count);
esp_err_t wifi_network_connect_station(const char *ssid, const char *password);
esp_err_t wifi_network_set_power_save(wifi_ps_type_t mode);
void wifi_network_stop(void);
bool wifi_network_has_valid_ip(void);
bool wifi_network_station_is_started(void);
bool wifi_network_station_has_valid_ip(void);
esp_err_t wifi_network_get_ip(char *buffer, size_t buffer_size);
