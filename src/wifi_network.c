#include "wifi_network.h"

#include <string.h>

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "nvs_flash.h"

#define NETWORK_AP_CHANNEL 1
#define NETWORK_AP_MAX_CONNECTIONS 4

static esp_netif_t *access_point_netif;
static esp_netif_t *station_netif;
static bool nvs_initialized;
static bool network_initialized;
static bool event_loop_initialized;
static bool wifi_initialized;
static bool wifi_started;

void wifi_network_stop(void)
{
    if (wifi_started) {
        esp_wifi_stop();
        wifi_started = false;
    }
    if (wifi_initialized) {
        esp_wifi_deinit();
        wifi_initialized = false;
    }
    if (access_point_netif != NULL) {
        esp_netif_destroy_default_wifi(access_point_netif);
        access_point_netif = NULL;
    }
    if (station_netif != NULL) {
        esp_netif_destroy_default_wifi(station_netif);
        station_netif = NULL;
    }
    if (event_loop_initialized) {
        esp_event_loop_delete_default();
        event_loop_initialized = false;
    }
    if (network_initialized) {
        esp_netif_deinit();
        network_initialized = false;
    }
    if (nvs_initialized) {
        nvs_flash_deinit();
        nvs_initialized = false;
    }
}

esp_err_t wifi_network_start_station(void)
{
    if (wifi_started) {
        return station_netif != NULL ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) {
        goto fail;
    }
    nvs_initialized = true;

    result = esp_netif_init();
    if (result != ESP_OK) {
        goto fail;
    }
    network_initialized = true;

    result = esp_event_loop_create_default();
    if (result != ESP_OK) {
        goto fail;
    }
    event_loop_initialized = true;

    station_netif = esp_netif_create_default_wifi_sta();
    if (station_netif == NULL) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&init_config);
    if (result != ESP_OK) {
        goto fail;
    }
    wifi_initialized = true;

    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK) {
        goto fail;
    }
    result = esp_wifi_start();
    if (result != ESP_OK) {
        goto fail;
    }
    wifi_started = true;
    return ESP_OK;

fail:
    wifi_network_stop();
    return result;
}

esp_err_t wifi_network_scan(wifi_ap_record_t *records, uint16_t *record_count)
{
    if (records == NULL || record_count == NULL || *record_count == 0 || station_netif == NULL ||
        !wifi_started) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t result = esp_wifi_scan_start(NULL, true);
    if (result != ESP_OK) {
        return result;
    }
    return esp_wifi_scan_get_ap_records(record_count, records);
}

esp_err_t wifi_network_connect_station(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > sizeof(((wifi_config_t *)0)->sta.ssid) ||
        password == NULL || strlen(password) > sizeof(((wifi_config_t *)0)->sta.password) ||
        station_netif == NULL || !wifi_started) {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t config = {0};
    memcpy(config.sta.ssid, ssid, strlen(ssid));
    memcpy(config.sta.password, password, strlen(password));
    config.sta.scan_method = WIFI_FAST_SCAN;
    config.sta.failure_retry_cnt = 3;
    esp_err_t result = esp_wifi_disconnect();
    if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_CONNECT) {
        return result;
    }
    result = esp_wifi_set_config(WIFI_IF_STA, &config);
    if (result != ESP_OK) {
        return result;
    }
    return esp_wifi_connect();
}

esp_err_t wifi_network_set_power_save(wifi_ps_type_t mode)
{
    if (!wifi_started || station_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_wifi_set_ps(mode);
}

esp_err_t wifi_network_start_ap(const char *ssid, const char *captive_portal_uri)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > sizeof(((wifi_config_t *)0)->ap.ssid)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (wifi_network_station_has_valid_ip()) {
        return ESP_OK;
    }
    if (wifi_started) {
        wifi_network_stop();
    }

    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) {
        goto fail;
    }
    nvs_initialized = true;

    result = esp_netif_init();
    if (result != ESP_OK) {
        goto fail;
    }
    network_initialized = true;

    result = esp_event_loop_create_default();
    if (result != ESP_OK) {
        goto fail;
    }
    event_loop_initialized = true;

    access_point_netif = esp_netif_create_default_wifi_ap();
    if (access_point_netif == NULL) {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }
    if (captive_portal_uri != NULL) {
        result = esp_netif_dhcps_option(access_point_netif, ESP_NETIF_OP_SET,
                                        ESP_NETIF_CAPTIVEPORTAL_URI, (void *)captive_portal_uri,
                                        strlen(captive_portal_uri));
        if (result != ESP_OK) {
            goto fail;
        }
    }

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&init_config);
    if (result != ESP_OK) {
        goto fail;
    }
    wifi_initialized = true;

    wifi_config_t config = {
        .ap = {
            .ssid_len = strlen(ssid),
            .channel = NETWORK_AP_CHANNEL,
            .max_connection = NETWORK_AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    memcpy(config.ap.ssid, ssid, config.ap.ssid_len);

    /* APSTA permits a future STA connection while preserving the local AP address. */
    result = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (result != ESP_OK) {
        goto fail;
    }
    result = esp_wifi_set_config(WIFI_IF_AP, &config);
    if (result != ESP_OK) {
        goto fail;
    }
    result = esp_wifi_start();
    if (result != ESP_OK) {
        goto fail;
    }
    wifi_started = true;
    return ESP_OK;

fail:
    wifi_network_stop();
    return result;
}

bool wifi_network_has_valid_ip(void)
{
    esp_netif_ip_info_t ip_info;
    return wifi_started && ((access_point_netif != NULL &&
                             esp_netif_get_ip_info(access_point_netif, &ip_info) == ESP_OK &&
                             ip_info.ip.addr != 0) ||
                            (station_netif != NULL &&
                             esp_netif_get_ip_info(station_netif, &ip_info) == ESP_OK &&
                             ip_info.ip.addr != 0));
}

bool wifi_network_station_has_valid_ip(void)
{
    esp_netif_ip_info_t ip_info;
    return wifi_started && station_netif != NULL &&
           esp_netif_get_ip_info(station_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0;
}

bool wifi_network_station_is_started(void)
{
    return wifi_started && station_netif != NULL;
}

esp_err_t wifi_network_get_ip(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = station_netif != NULL ? station_netif : access_point_netif;
    if (!wifi_started || netif == NULL || esp_netif_get_ip_info(netif, &ip_info) != ESP_OK ||
        ip_info.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_ip4addr_ntoa(&ip_info.ip, buffer, buffer_size) == NULL ? ESP_FAIL : ESP_OK;
}
