#include "ntp_client.h"

#include <stdbool.h>
#include <stdint.h>
#include <sys/time.h>
#include <time.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lvgl.h"
#include "deep_sleep.h"
#include "main_menu.h"
#include "time_mgmt.h"
#include "ui_theme.h"

#define NTP_PORT 123
#define NTP_PACKET_SIZE 48
#define NTP_TRANSMIT_TIMESTAMP_OFFSET 40
#define NTP_UNIX_EPOCH_OFFSET 2208988800UL
#define NTP_REQUEST_TIMEOUT_SECONDS 2
#define NTP_TASK_PRIORITY 3
#define NTP_TASK_CORE 0
#define NTP_TASK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define NTP_STATUS_REFRESH_MS 100

typedef enum {
    NTP_SYNC_IDLE,
    NTP_SYNC_IN_PROGRESS,
    NTP_SYNC_SUCCEEDED,
    NTP_SYNC_FAILED,
} ntp_sync_state_t;

static const char *const TAG = "ntp_client";
static const char *const ntp_servers[] = {
    "pool.ntp.org",
    "time.google.com",
    "time.cloudflare.com",
};
static lv_timer_t *status_refresh_timer;
static lv_obj_t *status_label;
static volatile ntp_sync_state_t sync_state;
static volatile esp_err_t sync_result;
static volatile uint32_t view_generation;
static volatile bool view_active;

static bool station_has_network_connection(void)
{
    wifi_ap_record_t access_point;
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
        return false;
    }

    esp_netif_t *station_netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (station_netif == NULL) {
        return false;
    }

    esp_netif_ip_info_t ip_info;
    return esp_netif_get_ip_info(station_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0;
}

static esp_err_t request_ntp_time(const char *server, time_t *utc_time)
{
    if (server == NULL || utc_time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_DGRAM,
    };
    struct addrinfo *address_info = NULL;
    if (getaddrinfo(server, "123", &hints, &address_info) != 0) {
        return ESP_ERR_NOT_FOUND;
    }

    int socket = lwip_socket(address_info->ai_family, address_info->ai_socktype,
                             address_info->ai_protocol);
    if (socket < 0) {
        freeaddrinfo(address_info);
        return ESP_FAIL;
    }

    struct timeval timeout = {.tv_sec = NTP_REQUEST_TIMEOUT_SECONDS};
    if (setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0) {
        close(socket);
        freeaddrinfo(address_info);
        return ESP_FAIL;
    }

    uint8_t request[NTP_PACKET_SIZE] = {0};
    request[0] = 0x23;
    ssize_t sent = sendto(socket, request, sizeof(request), 0, address_info->ai_addr,
                          address_info->ai_addrlen);
    freeaddrinfo(address_info);
    if (sent != sizeof(request)) {
        close(socket);
        return ESP_FAIL;
    }

    uint8_t response[NTP_PACKET_SIZE];
    ssize_t received = recv(socket, response, sizeof(response), 0);
    close(socket);
    if (received < NTP_PACKET_SIZE) {
        return ESP_ERR_TIMEOUT;
    }

    uint32_t ntp_seconds = ((uint32_t)response[NTP_TRANSMIT_TIMESTAMP_OFFSET] << 24) |
                           ((uint32_t)response[NTP_TRANSMIT_TIMESTAMP_OFFSET + 1] << 16) |
                           ((uint32_t)response[NTP_TRANSMIT_TIMESTAMP_OFFSET + 2] << 8) |
                           response[NTP_TRANSMIT_TIMESTAMP_OFFSET + 3];
    if (ntp_seconds <= NTP_UNIX_EPOCH_OFFSET) {
        return ESP_ERR_INVALID_RESPONSE;
    }

    *utc_time = (time_t)(ntp_seconds - NTP_UNIX_EPOCH_OFFSET);
    return ESP_OK;
}

static void synchronize_time_task(void *argument)
{
    uint32_t request_generation = (uint32_t)(uintptr_t)argument;
    esp_err_t result = ESP_ERR_TIMEOUT;
    time_t utc_time = 0;

    for (size_t index = 0; index < sizeof(ntp_servers) / sizeof(ntp_servers[0]); index++) {
        result = request_ntp_time(ntp_servers[index], &utc_time);
        if (result == ESP_OK) {
            break;
        }
    }

    if (result == ESP_OK && view_active && request_generation == view_generation) {
        struct timeval system_time = {.tv_sec = utc_time};
        if (settimeofday(&system_time, NULL) != 0) {
            result = ESP_FAIL;
        } else {
            result = time_mgmt_set_utc(utc_time);
        }
    }

    if (view_active && request_generation == view_generation) {
        sync_result = result;
        sync_state = result == ESP_OK ? NTP_SYNC_SUCCEEDED : NTP_SYNC_FAILED;
    }
    vTaskDelete(NULL);
}

static void refresh_sync_status(lv_timer_t *timer)
{
    (void)timer;

    if (sync_state == NTP_SYNC_SUCCEEDED) {
        lv_label_set_text(status_label, "Time synchronized successfully");
    } else if (sync_state == NTP_SYNC_FAILED) {
        lv_label_set_text_fmt(status_label, "Synchronization failed: %s",
                              esp_err_to_name(sync_result));
    }
}

static void start_synchronization(lv_event_t *event)
{
    (void)event;

    if (!station_has_network_connection()) {
        sync_result = ESP_ERR_INVALID_STATE;
        sync_state = NTP_SYNC_FAILED;
        lv_label_set_text(status_label, "No Wi-Fi network connection.");
        return;
    }
    if (sync_state == NTP_SYNC_IN_PROGRESS) {
        return;
    }

    sync_state = NTP_SYNC_IN_PROGRESS;
    lv_label_set_text(status_label, "Contacting time servers...");
    if (xTaskCreatePinnedToCoreWithCaps(synchronize_time_task, "ntp_sync", 4096,
                                        (void *)(uintptr_t)view_generation, NTP_TASK_PRIORITY, NULL,
                                        NTP_TASK_CORE, NTP_TASK_CAPS) != pdPASS) {
        sync_result = ESP_ERR_NO_MEM;
        sync_state = NTP_SYNC_FAILED;
    }
}

static void return_to_main_menu(void *user_data)
{
    (void)user_data;

    view_active = false;
    view_generation++;
    if (status_refresh_timer != NULL) {
        lv_timer_delete(status_refresh_timer);
        status_refresh_timer = NULL;
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

void ntp_client_create(void)
{
    sleep_timer_pause();
    view_active = true;
    view_generation++;
    sync_state = NTP_SYNC_IDLE;
    sync_result = ESP_OK;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "NTP client");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

    status_label = lv_label_create(screen);
    lv_obj_set_width(status_label, 350);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_obj_align(status_label, LV_ALIGN_CENTER, 0, -20);

    bool connected = station_has_network_connection();
    lv_label_set_text(status_label, connected
                                      ? "Network connected. Synchronize the watch time?"
                                      : "No Wi-Fi network connection. Connect to Wi-Fi first.");

    lv_obj_t *back_button = lv_button_create(screen);
    lv_obj_set_size(back_button, 110, 44);
    ui_theme_apply_button(back_button);
    lv_obj_align(back_button, LV_ALIGN_CENTER, 65, 72);
    lv_obj_add_event_cb(back_button, request_main_menu, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_center(back_label);

    lv_obj_t *synchronize_button = lv_button_create(screen);
    lv_obj_set_size(synchronize_button, 130, 44);
    ui_theme_apply_button(synchronize_button);
    lv_obj_align(synchronize_button, LV_ALIGN_CENTER, -65, 72);
    lv_obj_add_event_cb(synchronize_button, start_synchronization, LV_EVENT_CLICKED, NULL);
    if (!connected) {
        lv_obj_add_state(synchronize_button, LV_STATE_DISABLED);
    }

    lv_obj_t *synchronize_label = lv_label_create(synchronize_button);
    lv_label_set_text(synchronize_label, "Synchronize");
    lv_obj_center(synchronize_label);

    if (connected) {
        status_refresh_timer = lv_timer_create(refresh_sync_status, NTP_STATUS_REFRESH_MS, NULL);
    }
}
