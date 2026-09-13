#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"

/**
 * @brief Start the shared network stack in SoftAP mode.
 *
 * @param captive_portal_uri Optional URI advertised by DHCP captive-portal options.
 * @return ESP_OK or the first NVS/netif/event-loop/Wi-Fi setup error.
 */
esp_err_t wifi_network_start_ap(const char *ssid, const char *captive_portal_uri);
/** @brief Start the shared network stack in station mode; returns setup errors. */
esp_err_t wifi_network_start_station(void);
/**
 * @brief Scan from a started station interface into caller-provided records.
 *
 * @p record_count is input capacity and output count. Returns argument/state,
 * Wi-Fi, or scan errors; ownership of @p records remains with the caller.
 */
esp_err_t wifi_network_scan(wifi_ap_record_t *records, uint16_t *record_count);
/** @brief Connect the started station using an SSID/password; reports Wi-Fi setup errors. */
esp_err_t wifi_network_connect_station(const char *ssid, const char *password);
/** @brief Set station Wi-Fi power-save behavior for a started Wi-Fi driver. */
esp_err_t wifi_network_set_power_save(wifi_ps_type_t mode);
/**
 * @brief Stop Wi-Fi, unregister network state, and release module-owned netif resources.
 *
 * Safe to call after partial startup; it intentionally has no error return
 * because cleanup continues through best-effort shutdown steps.
 */
void wifi_network_stop(void);
/** @brief Return whether the active network mode currently has a valid IP address. */
bool wifi_network_has_valid_ip(void);
/** @brief Return whether the module has created and started a station interface. */
bool wifi_network_station_is_started(void);
/** @brief Return whether the station interface specifically has a valid IP address. */
bool wifi_network_station_has_valid_ip(void);
/**
 * @brief Format the active interface IPv4 address into @p buffer.
 *
 * @return ESP_OK, ESP_ERR_INVALID_ARG for invalid output storage,
 *         ESP_ERR_INVALID_STATE without an IP, or a formatting error.
 */
esp_err_t wifi_network_get_ip(char *buffer, size_t buffer_size);
