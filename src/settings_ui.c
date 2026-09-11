#include "settings_ui.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "deep_sleep.h"
#include "main_menu.h"
#include "sd_card.h"
#include "ui_theme.h"
#include "wifi_network.h"

#define CONFIG_AP_SSID "WatchConfig"
#define CONFIG_AP_CHANNEL 1
#define CONFIG_AP_MAX_CONNECTIONS 4
#define CAPTIVE_PORTAL_URI "http://192.168.4.1/"
#define SETTINGS_TASK_PRIORITY 3
#define SETTINGS_TASK_CORE 0
/*
 * Wi-Fi can freeze caches while changing radio state. Its callers therefore
 * require internal-RAM stacks; HTTP payloads can still use PSRAM elsewhere.
 */
#define SETTINGS_TASK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define SETTINGS_UPLOAD_BUFFER_SIZE 4096
#define SETTINGS_PATH_SIZE 256

/*
 * The Wi-Fi driver is pinned to core 0. Keep portal setup and HTTP handling
 * there as well; main.c pins LVGL to core 1 to keep the display responsive.
 */
static const char *const TAG = "settings_ui";
static httpd_handle_t http_server;
static bool spiffs_initialized;
static bool spiffs_mount_failed;
static volatile bool settings_view_active;

static esp_err_t send_status(httpd_req_t *request, const char *status, const char *message)
{
    httpd_resp_set_status(request, status);
    return httpd_resp_sendstr(request, message);
}

static esp_err_t send_sdcard_unavailable(httpd_req_t *request)
{
    return send_status(request, "503 Service Unavailable", "SD card unavailable");
}

static bool hex_value(char character, uint8_t *value)
{
    if (character >= '0' && character <= '9')
    {
        *value = character - '0';
    }
    else if (character >= 'a' && character <= 'f')
    {
        *value = character - 'a' + 10;
    }
    else if (character >= 'A' && character <= 'F')
    {
        *value = character - 'A' + 10;
    }
    else
    {
        return false;
    }
    return true;
}

static bool request_sd_path(httpd_req_t *request, char *path, size_t path_size)
{
    size_t query_length = httpd_req_get_url_query_len(request);
    if (query_length == 0 || query_length >= SETTINGS_PATH_SIZE)
    {
        return false;
    }

    char query[SETTINGS_PATH_SIZE];
    char encoded_path[SETTINGS_PATH_SIZE];
    char decoded_path[SETTINGS_PATH_SIZE];
    if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "path", encoded_path, sizeof(encoded_path)) != ESP_OK)
    {
        return false;
    }

    size_t output_length = 0;
    for (size_t index = 0; encoded_path[index] != '\0'; index++)
    {
        char character = encoded_path[index];
        if (character == '%')
        {
            uint8_t high;
            uint8_t low;
            if (!hex_value(encoded_path[++index], &high) || !hex_value(encoded_path[++index], &low))
            {
                return false;
            }
            character = (char)((high << 4) | low);
        }
        if (character == '\0' || output_length + 1 >= sizeof(decoded_path))
        {
            return false;
        }
        decoded_path[output_length++] = character == '+' ? ' ' : character;
    }
    decoded_path[output_length] = '\0';

    if (decoded_path[0] != '/' || strstr(decoded_path, "..") != NULL)
    {
        return false;
    }
    int length = snprintf(path, path_size, SD_CARD_MOUNT_PATH "%s", decoded_path);
    return length >= 0 && (size_t)length < path_size;
}

static esp_err_t mount_sdcard_for_request(httpd_req_t *request)
{
    esp_err_t result = sd_card_mount();
    if (result != ESP_OK)
    {
        send_sdcard_unavailable(request);
    }
    return result;
}

static esp_err_t send_json_string(httpd_req_t *request, const char *value)
{
    esp_err_t result = httpd_resp_send_chunk(request, "\"", 1);
    for (const char *character = value; result == ESP_OK && *character != '\0'; character++)
    {
        if (*character == '"' || *character == '\\')
        {
            result = httpd_resp_send_chunk(request, "\\", 1);
        }
        if (result == ESP_OK)
        {
            result = httpd_resp_send_chunk(request, character, 1);
        }
    }
    return result == ESP_OK ? httpd_resp_send_chunk(request, "\"", 1) : result;
}

static esp_err_t list_sdcard_files(httpd_req_t *request)
{
    if (mount_sdcard_for_request(request) != ESP_OK)
    {
        return ESP_FAIL;
    }

    char path[SETTINGS_PATH_SIZE];
    if (!request_sd_path(request, path, sizeof(path)))
    {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    DIR *directory = opendir(path);
    if (directory == NULL)
    {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "Directory unavailable");
    }

    httpd_resp_set_type(request, "application/json");
    esp_err_t result = httpd_resp_send_chunk(request, "{\"entries\":[", HTTPD_RESP_USE_STRLEN);
    bool first = true;
    struct dirent *entry;
    while (result == ESP_OK && (entry = readdir(directory)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        char entry_path[SETTINGS_PATH_SIZE];
        struct stat info;
        int length = snprintf(entry_path, sizeof(entry_path), "%s/%s", path, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(entry_path) || stat(entry_path, &info) != 0)
        {
            continue;
        }
        result = httpd_resp_send_chunk(request, first ? "{" : ",{", HTTPD_RESP_USE_STRLEN);
        if (result == ESP_OK)
        {
            result = httpd_resp_send_chunk(request, "\"name\":", HTTPD_RESP_USE_STRLEN);
        }
        if (result == ESP_OK)
        {
            result = send_json_string(request, entry->d_name);
        }
        if (result == ESP_OK)
        {
            result = httpd_resp_send_chunk(
                request, S_ISDIR(info.st_mode) ? ",\"type\":\"directory\"" : ",\"type\":\"file\"",
                HTTPD_RESP_USE_STRLEN);
        }
        if (result == ESP_OK)
        {
            char metadata[80];
            length = snprintf(metadata, sizeof(metadata), ",\"size\":%lld,\"modified\":%lld}",
                              (long long)info.st_size, (long long)info.st_mtime);
            if (length < 0 || (size_t)length >= sizeof(metadata))
            {
                result = ESP_FAIL;
            }
            else
            {
                result = httpd_resp_send_chunk(request, metadata, length);
            }
        }
        first = false;
    }
    closedir(directory);
    if (result == ESP_OK)
    {
        result = httpd_resp_send_chunk(request, "]}", HTTPD_RESP_USE_STRLEN);
    }
    return result == ESP_OK ? httpd_resp_send_chunk(request, NULL, 0) : result;
}

static esp_err_t upload_sdcard_file(httpd_req_t *request)
{
    if (mount_sdcard_for_request(request) != ESP_OK)
    {
        return ESP_FAIL;
    }

    char path[SETTINGS_PATH_SIZE];
    if (!request_sd_path(request, path, sizeof(path)))
    {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    FILE *file = fopen(path, "wb");
    if (file == NULL)
    {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to create file");
    }
    char *buffer = heap_caps_malloc(SETTINGS_UPLOAD_BUFFER_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == NULL)
    {
        fclose(file);
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Insufficient memory");
    }

    int remaining = request->content_len;
    while (remaining > 0)
    {
        int received = httpd_req_recv(request, buffer,
                                      remaining < SETTINGS_UPLOAD_BUFFER_SIZE
                                          ? remaining
                                          : SETTINGS_UPLOAD_BUFFER_SIZE);
        if (received <= 0 || fwrite(buffer, 1, received, file) != (size_t)received)
        {
            heap_caps_free(buffer);
            fclose(file);
            unlink(path);
            return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
        }
        remaining -= received;
    }
    heap_caps_free(buffer);
    fclose(file);
    return httpd_resp_sendstr(request, "{\"status\":\"uploaded\"}");
}

static esp_err_t create_sdcard_directory(httpd_req_t *request)
{
    if (mount_sdcard_for_request(request) != ESP_OK)
    {
        return ESP_FAIL;
    }
    char path[SETTINGS_PATH_SIZE];
    if (!request_sd_path(request, path, sizeof(path)))
    {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid path");
    }
    if (mkdir(path, 0755) != 0)
    {
        return send_status(request, "409 Conflict", "Directory already exists or cannot be created");
    }
    return httpd_resp_sendstr(request, "{\"status\":\"created\"}");
}

static esp_err_t delete_sdcard_file(httpd_req_t *request)
{
    if (mount_sdcard_for_request(request) != ESP_OK)
    {
        return ESP_FAIL;
    }
    char path[SETTINGS_PATH_SIZE];
    struct stat info;
    if (!request_sd_path(request, path, sizeof(path)))
    {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid path");
    }
    if (stat(path, &info) != 0 || S_ISDIR(info.st_mode))
    {
        return httpd_resp_send_err(request, HTTPD_404_NOT_FOUND, "File unavailable");
    }
    if (unlink(path) != 0)
    {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete file");
    }
    return httpd_resp_sendstr(request, "{\"status\":\"deleted\"}");
}

static const httpd_uri_t list_sdcard_files_uri = {
    .uri = "/api/files",
    .method = HTTP_GET,
    .handler = list_sdcard_files,
};
static const httpd_uri_t upload_sdcard_file_uri = {
    .uri = "/api/files",
    .method = HTTP_PUT,
    .handler = upload_sdcard_file,
};
static const httpd_uri_t delete_sdcard_file_uri = {
    .uri = "/api/files",
    .method = HTTP_DELETE,
    .handler = delete_sdcard_file,
};
static const httpd_uri_t create_sdcard_directory_uri = {
    .uri = "/api/directories",
    .method = HTTP_POST,
    .handler = create_sdcard_directory,
};

static const char *content_type_for_path(const char *path)
{
    if (strstr(path, ".js") != NULL)
    {
        return "text/javascript";
    }
    if (strstr(path, ".css") != NULL)
    {
        return "text/css";
    }
    if (strstr(path, ".ico") != NULL)
    {
        return "image/x-icon";
    }
    if (strstr(path, ".svg") != NULL)
    {
        return "image/svg+xml";
    }
    return "text/html";
}

static esp_err_t static_file_handler(httpd_req_t *request)
{
    const char *path = strcmp(request->uri, "/") == 0 ? "/index.html" : request->uri;
    /* Do not let request paths escape the SPIFFS asset directory. */
    if (strstr(path, "..") != NULL)
    {
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Invalid path");
    }

    char file_path[128];
    int length = snprintf(file_path, sizeof(file_path), "/spiffs%s", path);
    if (length < 0 || length >= sizeof(file_path))
    {
        return httpd_resp_send_err(request, HTTPD_414_URI_TOO_LONG, "Path too long");
    }

    FILE *file = fopen(file_path, "r");
    if (file == NULL)
    {
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
    while ((bytes_read = fread(buffer, 1, sizeof(buffer), file)) > 0)
    {
        esp_err_t result = httpd_resp_send_chunk(request, buffer, bytes_read);
        if (result != ESP_OK)
        {
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
    sd_card_unmount();
    wifi_network_stop();
}

static esp_err_t settings_ui_start(void)
{
    spiffs_mount_failed = false;

    esp_err_t result = wifi_network_start_ap(CONFIG_AP_SSID, CAPTIVE_PORTAL_URI);
    if (result != ESP_OK)
    {
        goto fail;
    }

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
    http_config.task_caps = SETTINGS_TASK_CAPS;
    http_config.uri_match_fn = httpd_uri_match_wildcard;
    result = httpd_start(&http_server, &http_config);
    if (result != ESP_OK)
    {
        goto fail;
    }
    result = httpd_register_uri_handler(http_server, &list_sdcard_files_uri);
    if (result != ESP_OK)
    {
        goto fail;
    }
    result = httpd_register_uri_handler(http_server, &upload_sdcard_file_uri);
    if (result != ESP_OK)
    {
        goto fail;
    }
    result = httpd_register_uri_handler(http_server, &delete_sdcard_file_uri);
    if (result != ESP_OK)
    {
        goto fail;
    }
    result = httpd_register_uri_handler(http_server, &create_sdcard_directory_uri);
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
    if (result != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to start configuration server: %s", esp_err_to_name(result));
    }
    if (!settings_view_active)
    {
        settings_ui_stop();
    }

    vTaskDelete(NULL);
}

static void back_to_main_menu(void *user_data)
{
    (void)user_data;

    settings_view_active = false;
    settings_ui_stop();
    lv_obj_clean(lv_screen_active());
    main_menu_create();
    sleep_timer_resume();
}

static void request_main_menu(lv_event_t *event)
{
    (void)event;

    lv_async_call(back_to_main_menu, NULL);
}

void settings_ui_create(void)
{
    /* Do not enter inactivity sleep while the user configures the device. */
    sleep_timer_pause();
    settings_view_active = true;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

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
    ui_theme_apply_button(back_button);
    lv_obj_align(back_button, LV_ALIGN_BOTTOM_LEFT, 52, -32);
    lv_obj_add_event_cb(back_button, request_main_menu, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);

    /*
     * Network initialization is not performed in the LVGL event callback.
     * It is isolated on core 0 so Wi-Fi setup cannot stall screen rendering.
     */
    if (xTaskCreatePinnedToCoreWithCaps(settings_ui_start_task, "settings_server", 6144, NULL,
                                        SETTINGS_TASK_PRIORITY, NULL, SETTINGS_TASK_CORE,
                                        SETTINGS_TASK_CAPS) != pdPASS)
    {
        ESP_LOGE(TAG, "Failed to create configuration server task");
        lv_label_set_text(connection, "Server unavailable: insufficient memory");
    }
}
