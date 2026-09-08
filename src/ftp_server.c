#include "ftp_server.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "assets/abstract_timekeeper.h"
#include "deep_sleep.h"
#include "main_menu.h"
#include "sd_card.h"
#include "wifi_network.h"

#define FTP_AP_SSID "WatchFTP"
#define FTP_PORT 21
#define FTP_TASK_PRIORITY 3
#define FTP_TASK_CORE 0
/*
 * Wi-Fi socket calls may run while caches are frozen, so FTP task stacks must
 * remain internal. File payloads are allocated separately from PSRAM.
 */
#define FTP_TASK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define FTP_TRANSFER_BUFFER_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define FTP_COMMAND_BUFFER_SIZE 256
#define FTP_PATH_SIZE 192
#define FTP_TRANSFER_BUFFER_SIZE 1024

static const char *const TAG = "ftp_server";
static volatile bool ftp_running;
static volatile bool ftp_view_active;
static int listen_socket = -1;
static int control_socket = -1;
static bool ftp_started_network;

static void send_response(int socket, const char *format, ...)
{
    char response[192];
    va_list arguments;
    va_start(arguments, format);
    int length = vsnprintf(response, sizeof(response), format, arguments);
    va_end(arguments);
    if (length > 0) {
        send(socket, response, length < (int)sizeof(response) ? length : sizeof(response) - 1, 0);
    }
}

static void close_socket(int *socket)
{
    if (*socket >= 0) {
        shutdown(*socket, SHUT_RDWR);
        close(*socket);
        *socket = -1;
    }
}

static bool send_data(int socket, const char *buffer, size_t length)
{
    size_t sent = 0;
    while (sent < length) {
        ssize_t count = send(socket, buffer + sent, length - sent, 0);
        if (count <= 0) {
            return false;
        }
        sent += count;
    }
    return true;
}

static bool make_sd_path(const char *working_directory, const char *argument, char *path,
                         size_t path_size)
{
    const char *virtual_path = argument[0] == '/' ? argument : working_directory;
    int length = argument[0] == '/'
                     ? snprintf(path, path_size, SD_CARD_MOUNT_PATH "%s", virtual_path)
                     : snprintf(path, path_size, SD_CARD_MOUNT_PATH "%s/%s", virtual_path, argument);
    return length >= 0 && (size_t)length < path_size && strstr(virtual_path, "..") == NULL;
}

static int open_passive_listener(int control)
{
    int socket = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (socket < 0) {
        send_response(control, "425 Cannot open data connection.\r\n");
        return -1;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = 0,
    };
    if (bind(socket, (struct sockaddr *)&address, sizeof(address)) < 0 || listen(socket, 1) < 0) {
        close_socket(&socket);
        send_response(control, "425 Cannot open data connection.\r\n");
        return -1;
    }

    socklen_t address_size = sizeof(address);
    getsockname(socket, (struct sockaddr *)&address, &address_size);
    char ip[16];
    if (wifi_network_get_ip(ip, sizeof(ip)) != ESP_OK) {
        close_socket(&socket);
        send_response(control, "425 Network is unavailable.\r\n");
        return -1;
    }

    unsigned int first, second, third, fourth;
    sscanf(ip, "%u.%u.%u.%u", &first, &second, &third, &fourth);
    uint16_t port = ntohs(address.sin_port);
    send_response(control, "227 Entering Passive Mode (%u,%u,%u,%u,%u,%u).\r\n", first, second,
                  third, fourth, port >> 8, port & 0xff);
    return socket;
}

static int accept_data_connection(int *passive_socket, int control)
{
    if (*passive_socket < 0) {
        send_response(control, "425 Use PASV before requesting a transfer.\r\n");
        return -1;
    }

    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(*passive_socket, &read_set);
    struct timeval timeout = {.tv_sec = 10};
    if (select(*passive_socket + 1, &read_set, NULL, NULL, &timeout) <= 0) {
        close_socket(passive_socket);
        send_response(control, "425 Data connection timed out.\r\n");
        return -1;
    }

    int data_socket = accept(*passive_socket, NULL, NULL);
    close_socket(passive_socket);
    return data_socket;
}

static void list_directory(int control, int *passive_socket, const char *path)
{
    DIR *directory = opendir(path);
    if (directory == NULL) {
        send_response(control, "550 Directory unavailable.\r\n");
        return;
    }

    send_response(control, "150 Opening data connection.\r\n");
    int data_socket = accept_data_connection(passive_socket, control);
    if (data_socket < 0) {
        closedir(directory);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child_path[FTP_PATH_SIZE];
        struct stat info;
        snprintf(child_path, sizeof(child_path), "%s/%s", path, entry->d_name);
        if (stat(child_path, &info) == 0) {
            char entry_line[FTP_PATH_SIZE + 64];
            int length = snprintf(entry_line, sizeof(entry_line),
                                  "%crw-r--r-- 1 0 0 %ld Jan 01 00:00 %s\r\n",
                                  S_ISDIR(info.st_mode) ? 'd' : '-', (long)info.st_size,
                                  entry->d_name);
            if (length < 0 || (size_t)length >= sizeof(entry_line) ||
                !send_data(data_socket, entry_line, length)) {
                break;
            }
        }
    }
    close_socket(&data_socket);
    closedir(directory);
    send_response(control, "226 Transfer complete.\r\n");
}

static void retrieve_file(int control, int *passive_socket, const char *path)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        send_response(control, "550 File unavailable.\r\n");
        return;
    }

    send_response(control, "150 Opening data connection.\r\n");
    int data_socket = accept_data_connection(passive_socket, control);
    if (data_socket < 0) {
        fclose(file);
        return;
    }

    char *buffer = heap_caps_malloc(FTP_TRANSFER_BUFFER_SIZE, FTP_TRANSFER_BUFFER_CAPS);
    if (buffer == NULL) {
        close_socket(&data_socket);
        fclose(file);
        send_response(control, "451 Insufficient memory for transfer.\r\n");
        return;
    }

    size_t count;
    while (ftp_running && (count = fread(buffer, 1, FTP_TRANSFER_BUFFER_SIZE, file)) > 0) {
        if (!send_data(data_socket, buffer, count)) {
            break;
        }
    }
    heap_caps_free(buffer);
    close_socket(&data_socket);
    fclose(file);
    send_response(control, "226 Transfer complete.\r\n");
}

static void process_command(int control, int *passive_socket, char *working_directory, char *line)
{
    char *argument = strchr(line, ' ');
    if (argument != NULL) {
        *argument++ = '\0';
    } else {
        argument = "";
    }

    if (strcasecmp(line, "USER") == 0) {
        send_response(control, "331 Anonymous login accepted.\r\n");
    } else if (strcasecmp(line, "PASS") == 0) {
        send_response(control, "230 Logged in.\r\n");
    } else if (strcasecmp(line, "SYST") == 0) {
        send_response(control, "215 UNIX Type: L8\r\n");
    } else if (strcasecmp(line, "FEAT") == 0) {
        send_response(control, "211-Features\r\n PASV\r\n SIZE\r\n211 End\r\n");
    } else if (strcasecmp(line, "PWD") == 0) {
        send_response(control, "257 \"%s\"\r\n", working_directory);
    } else if (strcasecmp(line, "TYPE") == 0) {
        send_response(control, "200 Type set.\r\n");
    } else if (strcasecmp(line, "PASV") == 0) {
        close_socket(passive_socket);
        *passive_socket = open_passive_listener(control);
    } else if (strcasecmp(line, "CWD") == 0) {
        char path[FTP_PATH_SIZE];
        struct stat info;
        if (!make_sd_path(working_directory, argument, path, sizeof(path)) || stat(path, &info) != 0 ||
            !S_ISDIR(info.st_mode)) {
            send_response(control, "550 Directory unavailable.\r\n");
        } else {
            snprintf(working_directory, FTP_PATH_SIZE - strlen(SD_CARD_MOUNT_PATH), "%s",
                     argument[0] == '/' ? argument : path + strlen(SD_CARD_MOUNT_PATH));
            send_response(control, "250 Directory changed.\r\n");
        }
    } else if (strcasecmp(line, "LIST") == 0 || strcasecmp(line, "NLST") == 0) {
        char path[FTP_PATH_SIZE];
        if (make_sd_path(working_directory, argument[0] == '\0' ? "." : argument, path, sizeof(path))) {
            list_directory(control, passive_socket, path);
        } else {
            send_response(control, "550 Invalid path.\r\n");
        }
    } else if (strcasecmp(line, "RETR") == 0) {
        char path[FTP_PATH_SIZE];
        if (make_sd_path(working_directory, argument, path, sizeof(path))) {
            retrieve_file(control, passive_socket, path);
        } else {
            send_response(control, "550 Invalid path.\r\n");
        }
    } else if (strcasecmp(line, "SIZE") == 0) {
        char path[FTP_PATH_SIZE];
        struct stat info;
        if (make_sd_path(working_directory, argument, path, sizeof(path)) && stat(path, &info) == 0) {
            send_response(control, "213 %ld\r\n", (long)info.st_size);
        } else {
            send_response(control, "550 File unavailable.\r\n");
        }
    } else if (strcasecmp(line, "QUIT") == 0) {
        send_response(control, "221 Goodbye.\r\n");
        close_socket(&control);
    } else {
        send_response(control, "502 Read-only FTP command unsupported.\r\n");
    }
}

static void serve_client(int socket)
{
    int passive_socket = -1;
    char working_directory[FTP_PATH_SIZE] = "/";
    char buffer[FTP_COMMAND_BUFFER_SIZE];
    size_t used = 0;
    send_response(socket, "220 WatchFTP ready.\r\n");

    while (ftp_running) {
        ssize_t count = recv(socket, buffer + used, sizeof(buffer) - used - 1, 0);
        if (count <= 0) {
            break;
        }
        used += count;
        buffer[used] = '\0';
        char *line;
        while ((line = strstr(buffer, "\r\n")) != NULL) {
            *line = '\0';
            process_command(socket, &passive_socket, working_directory, buffer);
            size_t remaining = used - ((line + 2) - buffer);
            memmove(buffer, line + 2, remaining);
            used = remaining;
            buffer[used] = '\0';
        }
        if (used == sizeof(buffer) - 1) {
            used = 0;
            send_response(socket, "500 Command too long.\r\n");
        }
    }
    close_socket(&passive_socket);
}

static void ftp_task(void *argument)
{
    (void)argument;
    while (ftp_running) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(listen_socket, &read_set);
        struct timeval timeout = {.tv_sec = 1};
        if (select(listen_socket + 1, &read_set, NULL, NULL, &timeout) <= 0) {
            continue;
        }
        control_socket = accept(listen_socket, NULL, NULL);
        if (control_socket >= 0) {
            serve_client(control_socket);
            close_socket(&control_socket);
        }
    }
    vTaskDelete(NULL);
}

static esp_err_t start_ftp_service(void)
{
    esp_err_t result = sd_card_mount();
    if (result != ESP_OK) {
        return result;
    }

    listen_socket = lwip_socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_socket < 0) {
        sd_card_unmount();
        return ESP_FAIL;
    }
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(FTP_PORT),
    };
    if (bind(listen_socket, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        listen(listen_socket, 1) < 0) {
        close_socket(&listen_socket);
        sd_card_unmount();
        return ESP_FAIL;
    }
    ftp_running = true;
    return xTaskCreatePinnedToCoreWithCaps(ftp_task, "ftp_server", 6144, NULL,
                                           FTP_TASK_PRIORITY, NULL, FTP_TASK_CORE,
                                           FTP_TASK_CAPS) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

static void stop_ftp_service(void)
{
    ftp_running = false;
    close_socket(&control_socket);
    close_socket(&listen_socket);
    sd_card_unmount();
    if (ftp_started_network) {
        wifi_network_stop();
        ftp_started_network = false;
    }
}

static void start_ftp_task(void *argument)
{
    (void)argument;
    if (!wifi_network_has_valid_ip()) {
        esp_err_t result = wifi_network_start_ap(FTP_AP_SSID, NULL);
        if (result != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start WatchFTP AP: %s", esp_err_to_name(result));
            vTaskDelete(NULL);
            return;
        }
        ftp_started_network = true;
    }
    esp_err_t result = start_ftp_service();
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start FTP service: %s", esp_err_to_name(result));
        stop_ftp_service();
    }
    if (!ftp_view_active) {
        stop_ftp_service();
    }
    vTaskDelete(NULL);
}

static void return_to_menu(lv_event_t *event)
{
    (void)event;
    ftp_view_active = false;
    stop_ftp_service();
    lv_obj_clean(lv_screen_active());
    main_menu_create();
    sleep_timer_resume();
}

static void begin_ftp(lv_event_t *event)
{
    lv_obj_t *status = lv_event_get_user_data(event);
    lv_label_set_text(status, "Starting WatchFTP at 192.168.4.1...");
    if (xTaskCreatePinnedToCoreWithCaps(start_ftp_task, "ftp_setup", 6144, NULL,
                                        FTP_TASK_PRIORITY, NULL, FTP_TASK_CORE,
                                        FTP_TASK_CAPS) != pdPASS) {
        lv_label_set_text(status, "FTP server unavailable: insufficient memory");
    }
}

void ftp_server_create(void)
{
    sleep_timer_pause();
    ftp_view_active = true;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x061826), 0);
    lv_obj_t *background = lv_image_create(screen);
    lv_image_set_src(background, &abstract_timekeeper);
    lv_obj_center(background);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "FTP server");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 36);

    lv_obj_t *status = lv_label_create(screen);
    lv_obj_set_width(status, 350);
    lv_label_set_long_mode(status, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status, lv_color_white(), 0);
    lv_obj_align(status, LV_ALIGN_CENTER, 0, -20);
    lv_label_set_text(status, wifi_network_has_valid_ip()
                                  ? "Network ready. Start read-only FTP access to /sdcard?"
                                  : "Wi-Fi is unavailable. Start the WatchFTP access point?");

    lv_obj_t *start_button = lv_button_create(screen);
    lv_obj_set_size(start_button, 110, 44);
    lv_obj_align(start_button, LV_ALIGN_CENTER, -65, 72);
    lv_obj_add_event_cb(start_button, begin_ftp, LV_EVENT_CLICKED, status);
    lv_obj_t *start_label = lv_label_create(start_button);
    lv_label_set_text(start_label, "Yes");
    lv_obj_center(start_label);

    lv_obj_t *back_button = lv_button_create(screen);
    lv_obj_set_size(back_button, 110, 44);
    lv_obj_align(back_button, LV_ALIGN_CENTER, 65, 72);
    lv_obj_add_event_cb(back_button, return_to_menu, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, "No");
    lv_obj_center(back_label);
}
