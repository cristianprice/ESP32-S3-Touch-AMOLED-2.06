#include "settings_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "lvgl.h"
#include "deep_sleep.h"
#include "main_menu.h"
#include "assets/abstract_timekeeper.h"

#define CONFIG_AP_SSID "WatchConfig"
#define CONFIG_AP_CHANNEL 1
#define CONFIG_AP_MAX_CONNECTIONS 4
#define CAPTIVE_PORTAL_URI "http://192.168.4.1/"
#define SETTINGS_TASK_PRIORITY 3
#define SETTINGS_TASK_CORE 0

/*
 * The Wi-Fi driver is pinned to core 0. Keep portal setup and HTTP handling
 * there as well; main.c pins LVGL to core 1 to keep the display responsive.
 */
static const char *const TAG = "settings_ui";
static httpd_handle_t http_server;
static esp_netif_t *access_point_netif;
static bool nvs_initialized;
static bool network_initialized;
static bool event_loop_initialized;
static bool wifi_initialized;
static bool wifi_started;
static bool spiffs_initialized;
static bool spiffs_mount_failed;
static volatile bool settings_view_active;

static const char *content_type_for_path(const char *path)
{
    if (strstr(path, ".js") != NULL) {
        return "text/javascript";
    }
    if (strstr(path, ".css") != NULL) {
        return "text/css";
    }
    if (strstr(path, ".ico") != NULL) {
        return "image/x-icon";
    }
    return "text/html";
}

static esp_err_t static_file_handler(httpd_req_t *request)
{
    const char *path = strcmp(request->uri, "/") == 0 ? "/index.html" : request->uri;
    /* Do not let request paths escape the SPIFFS asset directory. */
    if (strstr(path, "..") != NULL) {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    char file_path[128];
    int length = snprintf(file_path, sizeof(file_path), "/spiffs%s", path);
    if (length < 0 || length >= sizeof(file_path)) {
        return httpd_resp_send_err(request, HTTPD_414_URI_TOO_LONG, "Path too long");
    }

    FILE *file = fopen(file_path, "r");
    if (file == NULL) {
        /* Captive portal probes and Angular client routes return to the app shell. */
        ESP_LOGI(TAG, "Redirecting unknown path: %s", path);
        httpd_resp_set_status(request, "302 Found");
        httpd_resp_set_hdr(request, "Location", "/");
        return httpd_resp_send(request, NULL, 0);
    }

    ESP_LOGI(TAG, "Serving static file: %s", file_path);
    httpd_resp_set_type(request, content_type_for_path(path));
    char buffer[1024];
    size_t bytes_read;
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        esp_err_t result = httpd_resp_send_chunk(request, buffer, bytes_read);
        if (result != ESP_OK) {
            fclose(file);
            return result;
        }
    }
    fclose(file);

    return httpd_resp_send_chunk(request, NULL, 0);
}

static const httpd_uri_t static_file_uri = {
    .uri = "/*",
    .method = HTTP_GET,
    .handler = static_file_handler,
};

static void log_cleanup_error(const char *operation, esp_err_t result)
{
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "%s: %s", operation, esp_err_to_name(result));
    }
}

static void settings_ui_stop(void)
{
    /* Release all portal resources before returning from the Settings screen. */
    if (http_server != NULL)
    {
        log_cleanup_error("Failed to stop HTTP server", httpd_stop(http_server));
        http_server = NULL;
    }
    if (spiffs_initialized)
    {
        log_cleanup_error("Failed to unmount SPIFFS", esp_vfs_spiffs_unregister("storage"));
        spiffs_initialized = false;
    }
    if (wifi_started)
    {
        log_cleanup_error("Failed to stop Wi-Fi", esp_wifi_stop());
        wifi_started = false;
    }
    if (wifi_initialized)
    {
        log_cleanup_error("Failed to deinitialize Wi-Fi", esp_wifi_deinit());
        wifi_initialized = false;
    }
    if (access_point_netif != NULL)
    {
        esp_netif_destroy_default_wifi(access_point_netif);
        access_point_netif = NULL;
    }
    if (event_loop_initialized)
    {
        log_cleanup_error("Failed to delete event loop", esp_event_loop_delete_default());
        event_loop_initialized = false;
    }
    if (network_initialized)
    {
        log_cleanup_error("Failed to deinitialize networking", esp_netif_deinit());
        network_initialized = false;
    }
    if (nvs_initialized)
    {
        log_cleanup_error("Failed to deinitialize NVS", nvs_flash_deinit());
        nvs_initialized = false;
    }
}

static esp_err_t settings_ui_start(void)
{
    spiffs_mount_failed = false;

    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK)
    {
        goto fail;
    }
    nvs_initialized = true;

    result = esp_netif_init();
    if (result != ESP_OK)
    {
        goto fail;
    }
    network_initialized = true;

    /* esp_netif's default SoftAP helpers require ESP-IDF's default event loop. */
    result = esp_event_loop_create_default();
    if (result != ESP_OK)
    {
        goto fail;
    }
    event_loop_initialized = true;

    access_point_netif = esp_netif_create_default_wifi_ap();
    if (access_point_netif == NULL)
    {
        result = ESP_ERR_NO_MEM;
        goto fail;
    }
    result = esp_netif_dhcps_option(
        access_point_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
        (void *)CAPTIVE_PORTAL_URI, strlen(CAPTIVE_PORTAL_URI));
    if (result != ESP_OK)
    {
        goto fail;
    }

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&wifi_init_config);
    if (result != ESP_OK)
    {
        goto fail;
    }
    wifi_initialized = true;

    wifi_config_t access_point_config = {
        .ap = {
            .ssid = CONFIG_AP_SSID,
            .ssid_len = sizeof(CONFIG_AP_SSID) - 1,
            .channel = CONFIG_AP_CHANNEL,
            .max_connection = CONFIG_AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_OPEN,
        },
    };
    result = esp_wifi_set_mode(WIFI_MODE_AP);
    if (result != ESP_OK)
    {
        goto fail;
    }
    result = esp_wifi_set_config(WIFI_IF_AP, &access_point_config);
    if (result != ESP_OK)
    {
        goto fail;
    }
    result = esp_wifi_start();
    if (result != ESP_OK)
    {
        goto fail;
    }
    wifi_started = true;

    /* Static Angular output is uploaded to the `storage` SPIFFS partition. */
    esp_vfs_spiffs_conf_t spiffs_config = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = false,
    };
    result = esp_vfs_spiffs_register(&spiffs_config);
    if (result != ESP_OK)
    {
        spiffs_mount_failed = true;
        ESP_LOGE(TAG, "Failed to mount static filesystem: %s", esp_err_to_name(result));
        goto fail;
    }
    spiffs_initialized = true;

    httpd_config_t http_config = HTTPD_DEFAULT_CONFIG();
    http_config.task_priority = SETTINGS_TASK_PRIORITY;
    http_config.core_id = SETTINGS_TASK_CORE;
    http_config.uri_match_fn = httpd_uri_match_wildcard;
    result = httpd_start(&http_server, &http_config);
    if (result != ESP_OK)
    {
        goto fail;
    }
    result = httpd_register_uri_handler(http_server, &static_file_uri);
    if (result != ESP_OK)
    {
        goto fail;
    }
    ESP_LOGI(TAG, "Configuration server started at " CAPTIVE_PORTAL_URI);

    return ESP_OK;

fail:
    settings_ui_stop();
    return result;
}

static void settings_ui_start_task(void *argument)
{
    (void)argument;

    esp_err_t result = settings_ui_start();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start configuration server: %s", esp_err_to_name(result));
    }
    if (!settings_view_active) {
        settings_ui_stop();
    }

    vTaskDelete(NULL);
}

static void back_to_main_menu(lv_event_t *event)
{
    (void)event;

    settings_view_active = false;
    settings_ui_stop();
    lv_obj_clean(lv_screen_active());
    main_menu_create();
    sleep_timer_resume();
}

void settings_ui_create(void)
{
    /* Do not enter inactivity sleep while the user configures the device. */
    sleep_timer_pause();
    settings_view_active = true;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x061826), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_hex(0x102C42), 0);
    lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);

    lv_obj_t *background = lv_image_create(screen);
    lv_image_set_src(background, &abstract_timekeeper);
    lv_obj_center(background);

    lv_obj_t *message = lv_label_create(screen);
    lv_label_set_text(message, "Web app waiting for configuration");
    lv_obj_set_style_text_font(message, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(message, lv_color_white(), 0);
    lv_obj_set_width(message, 360);
    lv_label_set_long_mode(message, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(message, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(message, LV_ALIGN_CENTER, 0, -24);

    lv_obj_t *connection = lv_label_create(screen);
    lv_label_set_text(connection, "Connect to WatchConfig and open 192.168.4.1");
    lv_obj_set_width(connection, 360);
    lv_label_set_long_mode(connection, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(connection, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(connection, lv_color_white(), 0);
    lv_obj_align(connection, LV_ALIGN_CENTER, 0, 36);

    lv_obj_t *back_button = lv_button_create(screen);
    lv_obj_set_size(back_button, 110, 44);
    lv_obj_align(back_button, LV_ALIGN_BOTTOM_LEFT, 52, -32);
    lv_obj_add_event_cb(back_button, back_to_main_menu, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);

    /*
     * Network initialization is not performed in the LVGL event callback.
     * It is isolated on core 0 so Wi-Fi setup cannot stall screen rendering.
     */
    if (xTaskCreatePinnedToCore(settings_ui_start_task, "settings_server", 6144, NULL,
                                SETTINGS_TASK_PRIORITY, NULL, SETTINGS_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create configuration server task");
        lv_label_set_text(connection, "Server unavailable: insufficient memory");
    }
}
