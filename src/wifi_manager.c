#include "wifi_manager.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "deep_sleep.h"
#include "main_menu.h"
#include "sd_card.h"
#include "ui_theme.h"
#include "wifi_network.h"

/*
 * Drives Wi-Fi scanning and connection as a state machine. Radio and file I/O
 * run in short-lived core-0 worker tasks; the LVGL timer is the sole consumer
 * of their state/results and performs every screen mutation on the UI side.
 */
#define WIFI_MANAGER_MAX_NETWORKS 20
#define WIFI_MANAGER_REFRESH_MS 100
#define WIFI_MANAGER_TASK_PRIORITY 3
#define WIFI_MANAGER_TASK_CORE 0
#define WIFI_MANAGER_TASK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define WIFI_MANAGER_CONNECT_TIMEOUT_US (15000000LL)
#define WIFI_CREDENTIALS_PATH SD_CARD_MOUNT_PATH "/wifi_credentials.json"

typedef enum {
    WIFI_MANAGER_SCANNING,
    WIFI_MANAGER_LISTING,
    WIFI_MANAGER_CREDENTIALS,
    WIFI_MANAGER_CONNECTING,
    WIFI_MANAGER_CONNECTED,
    WIFI_MANAGER_RETURNING,
    WIFI_MANAGER_FAILED,
} wifi_manager_state_t;

static lv_timer_t *ui_timer;
static lv_obj_t *status_label;
static lv_obj_t *network_list;
static lv_obj_t *password_input;
static lv_obj_t *keyboard;
static wifi_ap_record_t networks[WIFI_MANAGER_MAX_NETWORKS];
static uint16_t network_count;
static char selected_ssid[sizeof(((wifi_config_t *)0)->sta.ssid) + 1];
static char selected_password[sizeof(((wifi_config_t *)0)->sta.password) + 1];
static char saved_ssid[sizeof(((wifi_config_t *)0)->sta.ssid) + 1];
static char saved_password[sizeof(((wifi_config_t *)0)->sta.password) + 1];
static bool saved_credentials_available;
static bool credential_view_visible;
static volatile wifi_manager_state_t state;
static volatile esp_err_t operation_result;
static volatile bool operation_running;
static int64_t connect_started_at_us;
static const char *const TAG = "wifi_manager";

static void encode_hex(const char *source, char *destination, size_t destination_size)
{
    /*
     * Hex encodes arbitrary credentials so the deliberately small JSON parser
     * need not handle quotes, backslashes, or control characters.
     */
    static const char hex[] = "0123456789ABCDEF";
    size_t length = strlen(source);
    if (destination_size < length * 2 + 1) {
        return;
    }
    for (size_t index = 0; index < length; index++) {
        destination[index * 2] = hex[(uint8_t)source[index] >> 4];
        destination[index * 2 + 1] = hex[(uint8_t)source[index] & 0x0F];
    }
    destination[length * 2] = '\0';
}

static bool decode_hex(const char *source, char *destination, size_t destination_size)
{
    size_t length = strlen(source);
    if ((length & 1U) != 0 || length / 2 >= destination_size) {
        return false;
    }
    for (size_t index = 0; index < length; index += 2) {
        char high = source[index];
        char low = source[index + 1];
        if ((high < '0' || high > '9') && (high < 'A' || high > 'F') &&
            (high < 'a' || high > 'f')) {
            return false;
        }
        if ((low < '0' || low > '9') && (low < 'A' || low > 'F') &&
            (low < 'a' || low > 'f')) {
            return false;
        }
        uint8_t high_value = (uint8_t)(high <= '9' ? high - '0'
                                       : (high <= 'F' ? high - 'A' + 10 : high - 'a' + 10));
        uint8_t low_value = (uint8_t)(low <= '9' ? low - '0'
                                      : (low <= 'F' ? low - 'A' + 10 : low - 'a' + 10));
        destination[index / 2] = (char)(high_value << 4 | low_value);
    }
    destination[length / 2] = '\0';
    return true;
}

static void load_saved_credentials(void)
{
    saved_credentials_available = false;
    FILE *file = fopen(WIFI_CREDENTIALS_PATH, "r");
    if (file == NULL) {
        return;
    }

    char contents[256];
    size_t length = fread(contents, 1, sizeof(contents) - 1, file);
    fclose(file);
    contents[length] = '\0';
    char ssid_hex[sizeof(saved_ssid) * 2] = {0};
    char password_hex[sizeof(saved_password) * 2] = {0};
    /* Fixed field widths bound parsing even if the SD-card file is corrupted. */
    if (sscanf(contents, "{\"ssid_hex\":\"%64[0123456789ABCDEFabcdef]\","
                 "\"password_hex\":\"%128[0123456789ABCDEFabcdef]\"}",
               ssid_hex, password_hex) == 2 &&
        decode_hex(ssid_hex, saved_ssid, sizeof(saved_ssid)) &&
        decode_hex(password_hex, saved_password, sizeof(saved_password)) && saved_ssid[0] != '\0') {
        saved_credentials_available = true;
    } else {
        ESP_LOGW(TAG, "Ignoring malformed saved Wi-Fi credentials");
    }
}

static esp_err_t save_credentials(void)
{
    char ssid_hex[sizeof(selected_ssid) * 2];
    char password_hex[sizeof(selected_password) * 2];
    encode_hex(selected_ssid, ssid_hex, sizeof(ssid_hex));
    encode_hex(selected_password, password_hex, sizeof(password_hex));

    FILE *file = fopen(WIFI_CREDENTIALS_PATH, "w");
    if (file == NULL) {
        return ESP_FAIL;
    }
    bool written = fprintf(file, "{\"ssid_hex\":\"%s\",\"password_hex\":\"%s\"}\n", ssid_hex,
                           password_hex) > 0;
    if (fclose(file) != 0 || !written) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool saved_network_is_visible(void)
{
    if (!saved_credentials_available) {
        return false;
    }
    for (uint16_t index = 0; index < network_count; index++) {
        if (strcmp((const char *)networks[index].ssid, saved_ssid) == 0) {
            return true;
        }
    }
    return false;
}

static void return_to_main_menu(void *user_data)
{
    (void)user_data;

    /* Delete the LVGL consumer before cleaning labels it may otherwise update. */
    if (ui_timer != NULL) {
        lv_timer_delete(ui_timer);
        ui_timer = NULL;
    }
    lv_obj_clean(lv_screen_active());
    main_menu_create();
    sleep_timer_resume();
}

static void request_main_menu(lv_event_t *event)
{
    (void)event;
    lv_async_call(return_to_main_menu, NULL);
}

static void create_back_button(lv_obj_t *screen)
{
    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_size(button, 110, 44);
    ui_theme_apply_button(button);
    lv_obj_align(button, LV_ALIGN_BOTTOM_MID, -65, -22);
    lv_obj_add_event_cb(button, request_main_menu, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
}

static void scan_task(void *argument)
{
    (void)argument;

    /* Blocking scan/connect operations run away from the display task. */
    operation_result = wifi_network_start_station();
    if (operation_result == ESP_OK) {
        esp_err_t mount_result = sd_card_mount();
        if (mount_result == ESP_OK) {
            load_saved_credentials();
            sd_card_unmount();
        } else {
            ESP_LOGW(TAG, "Cannot load saved Wi-Fi credentials: %s",
                     esp_err_to_name(mount_result));
        }
        network_count = WIFI_MANAGER_MAX_NETWORKS;
        operation_result = wifi_network_scan(networks, &network_count);
    }
    if (operation_result == ESP_OK && saved_network_is_visible()) {
        snprintf(selected_ssid, sizeof(selected_ssid), "%s", saved_ssid);
        snprintf(selected_password, sizeof(selected_password), "%s", saved_password);
        operation_result = wifi_network_connect_station(selected_ssid, selected_password);
        if (operation_result == ESP_OK) {
            connect_started_at_us = esp_timer_get_time();
            while (!wifi_network_station_has_valid_ip() &&
                   esp_timer_get_time() - connect_started_at_us < WIFI_MANAGER_CONNECT_TIMEOUT_US) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (!wifi_network_station_has_valid_ip()) {
                operation_result = ESP_ERR_TIMEOUT;
            }
        }
        state = operation_result == ESP_OK ? WIFI_MANAGER_CONNECTED : WIFI_MANAGER_CREDENTIALS;
    } else {
        state = operation_result == ESP_OK ? WIFI_MANAGER_LISTING : WIFI_MANAGER_FAILED;
    }
    operation_running = false;
    vTaskDelete(NULL);
}

static void connect_task(void *argument)
{
    (void)argument;

    operation_result = wifi_network_connect_station(selected_ssid, selected_password);
    if (operation_result != ESP_OK) {
        state = WIFI_MANAGER_FAILED;
        operation_running = false;
        vTaskDelete(NULL);
        return;
    }

    /* Polling permits a finite UI-visible timeout without registering netif events. */
    connect_started_at_us = esp_timer_get_time();
    while (!wifi_network_station_has_valid_ip()) {
        if (esp_timer_get_time() - connect_started_at_us >= WIFI_MANAGER_CONNECT_TIMEOUT_US) {
            operation_result = ESP_ERR_TIMEOUT;
            state = WIFI_MANAGER_FAILED;
            operation_running = false;
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }

    state = WIFI_MANAGER_CONNECTED;
    esp_err_t mount_result = sd_card_mount();
    if (mount_result == ESP_OK) {
        esp_err_t save_result = save_credentials();
        sd_card_unmount();
        if (save_result != ESP_OK) {
            ESP_LOGW(TAG, "Failed to save Wi-Fi credentials: %s", esp_err_to_name(save_result));
        }
    } else {
        ESP_LOGW(TAG, "Cannot save Wi-Fi credentials: %s", esp_err_to_name(mount_result));
    }
    operation_running = false;
    vTaskDelete(NULL);
}

static void begin_scan(lv_event_t *event)
{
    (void)event;

    if (operation_running) {
        return;
    }
    operation_running = true;
    state = WIFI_MANAGER_SCANNING;
    operation_result = ESP_OK;
    network_list = NULL;
    lv_label_set_text(status_label, "Scanning nearby Wi-Fi networks...");
    if (xTaskCreatePinnedToCoreWithCaps(scan_task, "wifi_scan", 6144, NULL,
                                        WIFI_MANAGER_TASK_PRIORITY, NULL, WIFI_MANAGER_TASK_CORE,
                                        WIFI_MANAGER_TASK_CAPS) != pdPASS) {
        operation_running = false;
        operation_result = ESP_ERR_NO_MEM;
        state = WIFI_MANAGER_FAILED;
    }
}

static void show_credentials(void *user_data);

static void select_network(lv_event_t *event)
{
    const wifi_ap_record_t *network = lv_event_get_user_data(event);
    snprintf(selected_ssid, sizeof(selected_ssid), "%s", (const char *)network->ssid);
    state = WIFI_MANAGER_CREDENTIALS;
    network_list = NULL;
    credential_view_visible = true;
    lv_async_call(show_credentials, NULL);
}

static void add_network_button(const wifi_ap_record_t *network)
{
    lv_obj_t *button = lv_button_create(network_list);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_height(button, 48);
    ui_theme_apply_button(button);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text_fmt(label, "%s %s  %d dBm",
                          network->authmode == WIFI_AUTH_OPEN ? LV_SYMBOL_WIFI : "WPA",
                          (const char *)network->ssid, network->rssi);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, select_network, LV_EVENT_CLICKED, (void *)network);
}

static void show_network_list(void *user_data)
{
    (void)user_data;

    lv_obj_clean(lv_screen_active());
    credential_view_visible = false;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Wi-Fi networks");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);

    status_label = lv_label_create(screen);
    lv_obj_set_width(status_label, 360);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFB35C), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 56);

    network_list = lv_obj_create(screen);
    lv_obj_set_size(network_list, 370, 335);
    lv_obj_align(network_list, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_set_flex_flow(network_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(network_list, 8, 0);
    lv_obj_set_style_bg_opa(network_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(network_list, 0, 0);

    if (network_count == 0) {
        lv_obj_t *empty = lv_label_create(network_list);
        lv_label_set_text(empty, "No networks found");
        lv_obj_set_style_text_color(empty, lv_color_white(), 0);
    } else {
        for (uint16_t index = 0; index < network_count; index++) {
            add_network_button(&networks[index]);
        }
    }
    lv_label_set_text(status_label, "Select a network");

    lv_obj_t *rescan_button = lv_button_create(screen);
    lv_obj_set_size(rescan_button, 110, 44);
    ui_theme_apply_button(rescan_button);
    lv_obj_align(rescan_button, LV_ALIGN_BOTTOM_MID, 65, -22);
    lv_obj_add_event_cb(rescan_button, begin_scan, LV_EVENT_CLICKED, NULL);
    lv_obj_t *rescan_label = lv_label_create(rescan_button);
    lv_label_set_text(rescan_label, LV_SYMBOL_REFRESH " Scan");
    lv_obj_set_style_text_color(rescan_label, lv_color_white(), 0);
    lv_obj_center(rescan_label);
    create_back_button(screen);
}

static void focus_input(lv_event_t *event)
{
    lv_keyboard_set_textarea(keyboard, lv_event_get_target(event));
}

static void request_network_list(lv_event_t *event)
{
    (void)event;
    state = WIFI_MANAGER_LISTING;
    network_list = NULL;
    lv_async_call(show_network_list, NULL);
}

static void begin_connect(lv_event_t *event)
{
    (void)event;

    if (operation_running) {
        return;
    }
    snprintf(selected_password, sizeof(selected_password), "%s", lv_textarea_get_text(password_input));
    operation_running = true;
    state = WIFI_MANAGER_CONNECTING;
    lv_label_set_text(status_label, "Connecting...");
    if (xTaskCreatePinnedToCoreWithCaps(connect_task, "wifi_connect", 4096, NULL,
                                        WIFI_MANAGER_TASK_PRIORITY, NULL, WIFI_MANAGER_TASK_CORE,
                                        WIFI_MANAGER_TASK_CAPS) != pdPASS) {
        operation_running = false;
        operation_result = ESP_ERR_NO_MEM;
        state = WIFI_MANAGER_FAILED;
    }
}

static void show_credentials(void *user_data)
{
    (void)user_data;

    lv_obj_clean(lv_screen_active());
    credential_view_visible = true;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, selected_ssid);
    lv_obj_set_width(title, 360);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "Enter the Wi-Fi password if required");
    lv_obj_set_width(status_label, 360);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFB35C), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 52);

    password_input = lv_textarea_create(screen);
    lv_obj_set_size(password_input, 350, 40);
    lv_textarea_set_one_line(password_input, true);
    lv_textarea_set_password_mode(password_input, true);
    lv_textarea_set_placeholder_text(password_input, "Password");
    lv_obj_align(password_input, LV_ALIGN_TOP_MID, 0, 104);
    lv_obj_add_event_cb(password_input, focus_input, LV_EVENT_FOCUSED, NULL);

    lv_obj_t *connect_button = lv_button_create(screen);
    lv_obj_set_size(connect_button, 110, 44);
    ui_theme_apply_button(connect_button);
    lv_obj_align(connect_button, LV_ALIGN_TOP_MID, 0, 158);
    lv_obj_add_event_cb(connect_button, begin_connect, LV_EVENT_CLICKED, NULL);
    lv_obj_t *connect_label = lv_label_create(connect_button);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_set_style_text_color(connect_label, lv_color_white(), 0);
    lv_obj_center(connect_label);

    keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(keyboard, LV_PCT(100), 210);
    lv_obj_align(keyboard, LV_ALIGN_TOP_MID, 0, 220);
    lv_keyboard_set_textarea(keyboard, password_input);

    lv_obj_t *back_button = lv_button_create(screen);
    lv_obj_set_size(back_button, 80, 36);
    ui_theme_apply_button(back_button);
    lv_obj_align(back_button, LV_ALIGN_TOP_LEFT, 12, 14);
    lv_obj_add_event_cb(back_button, request_network_list, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);
}

static void refresh_ui(lv_timer_t *timer)
{
    (void)timer;

    /* Workers publish state only; this timer owns all LVGL transitions. */
    if (state == WIFI_MANAGER_LISTING && network_list == NULL) {
        lv_async_call(show_network_list, NULL);
    } else if (state == WIFI_MANAGER_CREDENTIALS && !credential_view_visible) {
        credential_view_visible = true;
        lv_async_call(show_credentials, NULL);
    } else if (state == WIFI_MANAGER_CONNECTED) {
        state = WIFI_MANAGER_RETURNING;
        lv_async_call(return_to_main_menu, NULL);
    } else if (state == WIFI_MANAGER_FAILED) {
        lv_label_set_text_fmt(status_label, "Wi-Fi failed: %s", esp_err_to_name(operation_result));
    }
}

void wifi_manager_create(void)
{
    sleep_timer_pause();
    state = WIFI_MANAGER_SCANNING;
    operation_result = ESP_OK;
    operation_running = false;
    network_count = 0;
    selected_ssid[0] = '\0';
    selected_password[0] = '\0';
    saved_ssid[0] = '\0';
    saved_password[0] = '\0';
    saved_credentials_available = false;
    credential_view_visible = false;
    status_label = NULL;
    network_list = NULL;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "Preparing Wi-Fi...");
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_obj_center(status_label);

    ui_timer = lv_timer_create(refresh_ui, WIFI_MANAGER_REFRESH_MS, NULL);
    begin_scan(NULL);
}
