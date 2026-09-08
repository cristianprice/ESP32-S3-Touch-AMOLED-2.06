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

esp_err_t wifi_network_start_ap(const char *ssid, const char *captive_portal_uri)
{
    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > sizeof(((wifi_config_t *)0)->ap.ssid)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (wifi_started) {
        return ESP_ERR_INVALID_STATE;
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
    return wifi_started && access_point_netif != NULL &&
           esp_netif_get_ip_info(access_point_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0;
}

esp_err_t wifi_network_get_ip(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_netif_ip_info_t ip_info;
    if (!wifi_started || access_point_netif == NULL ||
        esp_netif_get_ip_info(access_point_netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_ip4addr_ntoa(&ip_info.ip, buffer, buffer_size) == NULL ? ESP_FAIL : ESP_OK;
}
