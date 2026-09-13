#include "multimedia_player.h"

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "deep_sleep.h"
#include "main_menu.h"
#include "sd_card.h"
#include "ui_theme.h"

/*
 * Browses SD-card directories and plays a constrained PCM WAV format through
 * the BSP speaker. Core-0 workers own blocking codec/filesystem operations;
 * the LVGL timer serializes UI transitions once those workers stop, preventing
 * deleted screens or an unmounted card from being used by an active task.
 */
#define PLAYER_PATH_SIZE 192
#define PLAYER_NAME_SIZE 64
#define PLAYER_BUFFER_SIZE 2048
#define PLAYER_OUTPUT_VOLUME 100
#define PLAYER_MAX_BROWSER_ENTRIES 48
#define PLAYER_UI_REFRESH_MS 100
#define PLAYER_TASK_PRIORITY 4
#define PLAYER_TASK_CORE 0
#define PLAYER_TASK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define RECORDINGS_DIRECTORY SD_CARD_MOUNT_PATH "/recordings"

typedef enum {
    PLAYER_STOPPED,
    PLAYER_PLAYING,
    PLAYER_PAUSED,
    PLAYER_FINISHED,
    PLAYER_FAILED,
} player_state_t;

typedef struct {
    uint16_t format;
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    uint32_t data_offset;
    uint32_t data_size;
} wav_format_t;

typedef struct {
    char name[PLAYER_NAME_SIZE];
    bool directory;
} browser_entry_t;

static const char *const TAG = "multimedia_player";
static lv_timer_t *player_ui_timer;
static lv_obj_t *title_label;
static lv_obj_t *path_label;
static lv_obj_t *content;
static lv_obj_t *status_label;
static lv_obj_t *operation_status_label;
static lv_obj_t *elapsed_label;
static lv_obj_t *progress_bar;
static lv_obj_t *volume_label;
static lv_obj_t *play_label;
static lv_obj_t *play_button;
static lv_obj_t *clear_button;
static char current_directory[PLAYER_PATH_SIZE];
static char selected_path[PLAYER_PATH_SIZE];
static char selected_name[PLAYER_NAME_SIZE];
static volatile player_state_t player_state;
static volatile bool playback_requested;
static volatile bool playback_paused;
static volatile bool playback_task_running;
static volatile esp_err_t playback_result;
static volatile uint32_t playback_position_bytes;
static volatile uint32_t playback_total_bytes;
static volatile uint8_t playback_volume;
static volatile bool playback_volume_changed;
static volatile bool clear_task_running;
static volatile bool clear_requested;
static volatile bool clear_result_ready;
static volatile esp_err_t clear_result;
static volatile int clear_errno;
static bool clear_completed;
static bool player_visible;
static bool leave_requested;
static bool player_screen_active;
static bool browser_requested;
static bool play_after_stop;
static browser_entry_t browser_entries[PLAYER_MAX_BROWSER_ENTRIES];
static size_t browser_entry_count;

static void return_to_main_menu(void *user_data);

static bool is_wav_filename(const char *name)
{
    size_t length = strlen(name);
    return length > 4 && strcasecmp(name + length - 4, ".wav") == 0;
}

static void request_return_to_main_menu(lv_event_t *event)
{
    (void)event;
    lv_async_call(return_to_main_menu, NULL);
}

static bool is_root_directory(void)
{
    return strcmp(current_directory, SD_CARD_MOUNT_PATH) == 0;
}

static void join_path(char *path, size_t path_size, const char *directory, const char *name)
{
    snprintf(path, path_size, "%s/%s", directory, name);
}

static void return_to_main_menu(void *user_data)
{
    (void)user_data;

    /* Keep the screen and SD mount alive until the playback task has released them. */
    if (playback_task_running) {
        return;
    }

    player_visible = false;
    leave_requested = false;
    if (player_ui_timer != NULL) {
        lv_timer_delete(player_ui_timer);
        player_ui_timer = NULL;
    }
    sd_card_unmount();
    lv_obj_clean(lv_screen_active());
    main_menu_create();
    sleep_timer_resume();
}

static bool read_wav_format(FILE *file, wav_format_t *format)
{
    /*
     * RIFF chunks are extensible and word-aligned. Walk them rather than
     * assuming a fixed 44-byte header, then retain the data chunk's offset.
     */
    uint8_t header[12];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        return false;
    }

    bool format_found = false;
    while (true) {
        uint8_t chunk_header[8];
        if (fread(chunk_header, 1, sizeof(chunk_header), file) != sizeof(chunk_header)) {
            return false;
        }
        uint32_t chunk_size = (uint32_t)chunk_header[4] |
                              (uint32_t)chunk_header[5] << 8 |
                              (uint32_t)chunk_header[6] << 16 |
                              (uint32_t)chunk_header[7] << 24;
        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            uint8_t fmt_data[16];
            if (chunk_size < sizeof(fmt_data) ||
                fread(fmt_data, 1, sizeof(fmt_data), file) != sizeof(fmt_data)) {
                return false;
            }
            format->format = (uint16_t)fmt_data[0] | (uint16_t)fmt_data[1] << 8;
            format->channels = (uint16_t)fmt_data[2] | (uint16_t)fmt_data[3] << 8;
            format->sample_rate = (uint32_t)fmt_data[4] | (uint32_t)fmt_data[5] << 8 |
                                  (uint32_t)fmt_data[6] << 16 | (uint32_t)fmt_data[7] << 24;
            format->bits_per_sample = (uint16_t)fmt_data[14] | (uint16_t)fmt_data[15] << 8;
            if (fseek(file, chunk_size - sizeof(fmt_data) + (chunk_size & 1U), SEEK_CUR) != 0) {
                return false;
            }
            format_found = true;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            if (!format_found) {
                return false;
            }
            format->data_offset = (uint32_t)ftell(file);
            format->data_size = chunk_size;
            return true;
        } else if (fseek(file, chunk_size + (chunk_size & 1U), SEEK_CUR) != 0) {
            return false;
        }
    }
}

static void playback_task(void *argument)
{
    (void)argument;

    FILE *file = fopen(selected_path, "rb");
    uint8_t *buffer = NULL;
    esp_codec_dev_handle_t speaker = NULL;
    esp_err_t result = ESP_OK;
    wav_format_t format;
    /* The hardware path accepts the recorder's 22.05 kHz, mono, 16-bit PCM only. */
    if (file == NULL || !read_wav_format(file, &format) || format.format != 1 ||
        format.channels != 1 || format.sample_rate != 22050 || format.bits_per_sample != 16) {
        result = ESP_ERR_NOT_SUPPORTED;
        goto finish;
    }

    result = bsp_audio_init(NULL);
    if (result != ESP_OK) {
        goto finish;
    }
    speaker = bsp_audio_codec_speaker_init();
    if (speaker == NULL) {
        result = ESP_FAIL;
        goto finish;
    }

    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = format.bits_per_sample,
        .channel = format.channels,
        .channel_mask = 0,
        .sample_rate = format.sample_rate,
        .mclk_multiple = 0,
    };
    result = esp_codec_dev_open(speaker, &sample_info);
    if (result != ESP_OK) {
        goto finish;
    }
    result = esp_codec_dev_set_out_vol(speaker, PLAYER_OUTPUT_VOLUME);
    if (result != ESP_OK) {
        goto finish;
    }
    buffer = heap_caps_malloc(PLAYER_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        result = ESP_ERR_NO_MEM;
        goto finish;
    }

    uint32_t remaining = format.data_size;
    playback_position_bytes = 0;
    playback_total_bytes = format.data_size;
    while (playback_requested && remaining > 0) {
        while (playback_requested && playback_paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!playback_requested) {
            break;
        }
        if (playback_volume_changed) {
            result = esp_codec_dev_set_out_vol(speaker, playback_volume);
            if (result != ESP_OK) {
                goto finish;
            }
            playback_volume_changed = false;
        }
        size_t wanted = remaining < PLAYER_BUFFER_SIZE ? remaining : PLAYER_BUFFER_SIZE;
        size_t bytes_read = fread(buffer, 1, wanted, file);
        if (bytes_read != wanted) {
            result = ESP_FAIL;
            goto finish;
        }
        result = esp_codec_dev_write(speaker, buffer, bytes_read);
        if (result != ESP_OK) {
            goto finish;
        }
        remaining -= bytes_read;
        playback_position_bytes += bytes_read;
    }

finish:
    /* The worker owns codec/file/buffer cleanup and publishes completion last. */
    if (buffer != NULL) {
        heap_caps_free(buffer);
    }
    if (speaker != NULL) {
        esp_codec_dev_close(speaker);
        esp_codec_dev_delete(speaker);
    }
    if (file != NULL) {
        fclose(file);
    }
    playback_result = result;
    playback_task_running = false;
    if (result != ESP_OK) {
        player_state = PLAYER_FAILED;
        ESP_LOGE(TAG, "Playback failed: %s", esp_err_to_name(result));
    } else if (playback_requested) {
        player_state = PLAYER_FINISHED;
    } else {
        player_state = PLAYER_STOPPED;
    }
    vTaskDelete(NULL);
}

static void start_playback(void)
{
    if (playback_task_running || selected_path[0] == '\0') {
        return;
    }

    playback_result = ESP_OK;
    playback_requested = true;
    playback_paused = false;
    playback_task_running = true;
    player_state = PLAYER_PLAYING;
    if (xTaskCreatePinnedToCoreWithCaps(playback_task, "wav_playback", 6144, NULL,
                                        PLAYER_TASK_PRIORITY, NULL, PLAYER_TASK_CORE,
                                        PLAYER_TASK_CAPS) != pdPASS) {
        playback_task_running = false;
        playback_requested = false;
        playback_result = ESP_ERR_NO_MEM;
        player_state = PLAYER_FAILED;
    }
}

static void stop_playback(void)
{
    playback_requested = false;
    playback_paused = false;
}

static void show_browser(void *user_data);

static void select_entry(lv_event_t *event)
{
    const browser_entry_t *entry = lv_event_get_user_data(event);
    char path[PLAYER_PATH_SIZE];
    join_path(path, sizeof(path), current_directory, entry->name);

    struct stat entry_info;
    if (stat(path, &entry_info) != 0) {
        lv_label_set_text(path_label, "Cannot read SD card entry");
        return;
    }
    if (entry->directory) {
        if (strcmp(entry->name, "..") == 0) {
            char *last_separator = strrchr(current_directory, '/');
            if (last_separator != NULL && last_separator != current_directory) {
                *last_separator = '\0';
            }
        } else {
            snprintf(current_directory, sizeof(current_directory), "%s", path);
        }
        lv_async_call(show_browser, NULL);
        return;
    }

    snprintf(selected_path, sizeof(selected_path), "%s", path);
    snprintf(selected_name, sizeof(selected_name), "%s", entry->name);
    lv_async_call(show_browser, (void *)true);
}

static void add_browser_entry(const browser_entry_t *entry)
{
    lv_obj_t *button = lv_button_create(content);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_height(button, 48);
    ui_theme_apply_button(button);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text_fmt(label, "%s %s",
                          entry->directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_AUDIO, entry->name);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    lv_obj_add_event_cb(button, select_entry, LV_EVENT_CLICKED, (void *)entry);
}

static void request_leave(lv_event_t *event)
{
    (void)event;
    leave_requested = true;
    stop_playback();
}

static void browser_back(lv_event_t *event)
{
    (void)event;
    request_leave(NULL);
}

static void request_stop(lv_event_t *event)
{
    (void)event;
    stop_playback();
}

static void request_toggle_playback(lv_event_t *event)
{
    (void)event;

    if (playback_task_running) {
        playback_paused = !playback_paused;
        player_state = playback_paused ? PLAYER_PAUSED : PLAYER_PLAYING;
    } else {
        clear_completed = false;
        start_playback();
    }
}

static void request_show_browser(lv_event_t *event)
{
    (void)event;
    browser_requested = true;
    stop_playback();
}

static bool clear_directory_contents(const char *directory_path)
{
    DIR *directory = opendir(directory_path);
    if (directory == NULL) {
        if (errno == ENOENT) {
            return true;
        }
        clear_errno = errno;
        return false;
    }

    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        char path[PLAYER_PATH_SIZE];
        int path_length = snprintf(path, sizeof(path), "%s/%s", directory_path, entry->d_name);
        if (path_length < 0 || (size_t)path_length >= sizeof(path)) {
            clear_errno = ENAMETOOLONG;
            closedir(directory);
            return false;
        }

        struct stat entry_info;
        if (stat(path, &entry_info) != 0) {
            clear_errno = errno;
            closedir(directory);
            return false;
        }
        if (S_ISDIR(entry_info.st_mode)) {
            if (!clear_directory_contents(path) || rmdir(path) != 0) {
                if (clear_errno == 0) {
                    clear_errno = errno;
                }
                closedir(directory);
                return false;
            }
        } else if (unlink(path) != 0) {
            clear_errno = errno;
            closedir(directory);
            return false;
        }
    }

    if (closedir(directory) != 0) {
        clear_errno = errno;
        return false;
    }
    return true;
}

static void clear_recordings_task(void *argument)
{
    (void)argument;

    clear_errno = 0;
    /* Runs after playback stops, so recursive deletion cannot race an open WAV file. */
    clear_result = clear_directory_contents(RECORDINGS_DIRECTORY) ? ESP_OK : ESP_FAIL;
    clear_result_ready = true;
    clear_task_running = false;
    vTaskDelete(NULL);
}

static void request_clear_recordings(lv_event_t *event)
{
    (void)event;

    if (clear_task_running || clear_requested) {
        return;
    }
    clear_requested = true;
    clear_result_ready = false;
    clear_completed = false;
    stop_playback();
    lv_obj_add_state(clear_button, LV_STATE_DISABLED);
    lv_label_set_text(operation_status_label, "Clearing recordings...");
}

static void update_volume(lv_event_t *event)
{
    playback_volume = lv_slider_get_value(lv_event_get_target(event));
    playback_volume_changed = true;
    lv_label_set_text_fmt(volume_label, "Volume %u%%", playback_volume);
}

static void select_adjacent_track(int direction)
{
    if (browser_entry_count == 0) {
        return;
    }

    size_t selected_index = browser_entry_count;
    for (size_t index = 0; index < browser_entry_count; index++) {
        if (!browser_entries[index].directory &&
            strcmp(browser_entries[index].name, selected_name) == 0) {
            selected_index = index;
            break;
        }
    }
    for (size_t offset = 1; offset <= browser_entry_count; offset++) {
        int index = (int)selected_index + direction * (int)offset;
        if (index < 0) {
            index += browser_entry_count;
        } else if (index >= (int)browser_entry_count) {
            index -= browser_entry_count;
        }
        if (!browser_entries[index].directory) {
            join_path(selected_path, sizeof(selected_path), current_directory,
                      browser_entries[index].name);
            snprintf(selected_name, sizeof(selected_name), "%s", browser_entries[index].name);
            play_after_stop = true;
            stop_playback();
            return;
        }
    }
}

static void request_previous_track(lv_event_t *event)
{
    (void)event;
    select_adjacent_track(-1);
}

static void request_next_track(lv_event_t *event)
{
    (void)event;
    select_adjacent_track(1);
}

static void show_player(void *user_data)
{
    (void)user_data;

    lv_obj_clean(lv_screen_active());
    player_screen_active = true;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    title_label = lv_label_create(screen);
    lv_label_set_text(title_label, "Media player");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 42);

    lv_obj_t *track_label = lv_label_create(screen);
    lv_label_set_text(track_label, selected_name);
    lv_label_set_long_mode(track_label, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(track_label, 340);
    lv_obj_set_style_text_color(track_label, lv_color_hex(0xFFB35C), 0);
    lv_obj_align(track_label, LV_ALIGN_TOP_MID, 0, 92);

    status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "Ready");
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 156);
    operation_status_label = status_label;

    elapsed_label = lv_label_create(screen);
    lv_label_set_text(elapsed_label, "00:00 / 00:00");
    lv_obj_set_style_text_color(elapsed_label, lv_color_hex(0xFFB35C), 0);
    lv_obj_align(elapsed_label, LV_ALIGN_TOP_MID, 0, 202);

    progress_bar = lv_bar_create(screen);
    lv_obj_set_size(progress_bar, 300, 12);
    lv_bar_set_range(progress_bar, 0, 100);
    lv_bar_set_value(progress_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0x1C1C1C), 0);
    lv_obj_set_style_bg_color(progress_bar, lv_color_hex(0xFF7A00), LV_PART_INDICATOR);
    lv_obj_align(progress_bar, LV_ALIGN_TOP_MID, 0, 234);

    volume_label = lv_label_create(screen);
    lv_label_set_text_fmt(volume_label, "Volume %u%%", playback_volume);
    lv_obj_set_style_text_color(volume_label, lv_color_hex(0xFFB35C), 0);
    lv_obj_align(volume_label, LV_ALIGN_TOP_MID, 0, 262);

    lv_obj_t *volume_slider = lv_slider_create(screen);
    lv_obj_set_size(volume_slider, 250, 14);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, playback_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0x1C1C1C), 0);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0xFF7A00), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0xFFB35C), LV_PART_KNOB);
    lv_obj_align(volume_slider, LV_ALIGN_TOP_MID, 0, 290);
    lv_obj_add_event_cb(volume_slider, update_volume, LV_EVENT_VALUE_CHANGED, NULL);

    const char *const symbols[] = {LV_SYMBOL_LEFT, LV_SYMBOL_STOP, LV_SYMBOL_PLAY,
                                   LV_SYMBOL_RIGHT, LV_SYMBOL_DIRECTORY};
    for (uint32_t index = 0; index < 5; index++) {
        lv_obj_t *button = lv_button_create(screen);
        lv_obj_set_size(button, index == 2 ? 78 : 58, index == 2 ? 78 : 58);
        ui_theme_apply_button(button);
        if (index == 0) {
            lv_obj_align(button, LV_ALIGN_CENTER, -142, 120);
        } else if (index == 1) {
            lv_obj_align(button, LV_ALIGN_CENTER, -72, 120);
        } else if (index == 2) {
            lv_obj_align(button, LV_ALIGN_CENTER, 0, 120);
            play_button = button;
        } else if (index == 3) {
            lv_obj_align(button, LV_ALIGN_CENTER, 72, 120);
        } else {
            lv_obj_align(button, LV_ALIGN_CENTER, 142, 120);
        }
        if (index == 0) {
            lv_obj_add_event_cb(button, request_previous_track, LV_EVENT_CLICKED, NULL);
        } else if (index == 1) {
            lv_obj_add_event_cb(button, request_stop, LV_EVENT_CLICKED, NULL);
        } else if (index == 2) {
            lv_obj_add_event_cb(button, request_toggle_playback, LV_EVENT_CLICKED, NULL);
        } else if (index == 3) {
            lv_obj_add_event_cb(button, request_next_track, LV_EVENT_CLICKED, NULL);
        } else if (index == 4) {
            lv_obj_add_event_cb(button, request_show_browser, LV_EVENT_CLICKED, NULL);
        }
        lv_obj_t *label = lv_label_create(button);
        lv_label_set_text(label, symbols[index]);
        lv_obj_set_style_text_font(label, &lv_font_montserrat_24, 0);
        lv_obj_set_style_text_color(label, lv_color_white(), 0);
        lv_obj_center(label);
        if (index == 2) {
            play_label = label;
        }
    }

    lv_obj_t *menu_button = lv_button_create(screen);
    lv_obj_set_size(menu_button, 120, 52);
    ui_theme_apply_button(menu_button);
    lv_obj_align(menu_button, LV_ALIGN_BOTTOM_MID, -70, -22);
    lv_obj_add_event_cb(menu_button, request_leave, LV_EVENT_CLICKED, NULL);
    lv_obj_t *menu_label = lv_label_create(menu_button);
    lv_label_set_text(menu_label, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(menu_label, lv_color_white(), 0);
    lv_obj_center(menu_label);

}

static void show_browser(void *user_data)
{
    if (user_data != NULL) {
        show_player(NULL);
        return;
    }

    lv_obj_clean(lv_screen_active());
    player_screen_active = false;
    status_label = NULL;
    elapsed_label = NULL;
    progress_bar = NULL;
    volume_label = NULL;
    play_label = NULL;
    play_button = NULL;
    clear_button = NULL;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    title_label = lv_label_create(screen);
    lv_label_set_text(title_label, "Media library");
    lv_obj_set_style_text_font(title_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title_label, lv_color_white(), 0);
    lv_obj_align(title_label, LV_ALIGN_TOP_MID, 0, 24);

    path_label = lv_label_create(screen);
    lv_label_set_text(path_label, current_directory);
    lv_obj_set_width(path_label, 370);
    lv_label_set_long_mode(path_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_obj_set_style_text_color(path_label, lv_color_hex(0xFFB35C), 0);
    lv_obj_align(path_label, LV_ALIGN_TOP_MID, 0, 58);
    operation_status_label = path_label;

    content = lv_obj_create(screen);
    lv_obj_set_size(content, 374, 310);
    lv_obj_align(content, LV_ALIGN_TOP_MID, 0, 94);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(content, 8, 0);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);

    browser_entry_count = 0;
    if (!is_root_directory()) {
        snprintf(browser_entries[browser_entry_count].name,
                 sizeof(browser_entries[browser_entry_count].name), "..");
        browser_entries[browser_entry_count].directory = true;
        add_browser_entry(&browser_entries[browser_entry_count++]);
    }
    DIR *directory = opendir(current_directory);
    if (directory == NULL) {
        lv_label_set_text(path_label, "Cannot open SD-card folder");
    } else {
        struct dirent *entry;
        bool found = false;
        while ((entry = readdir(directory)) != NULL) {
            if (entry->d_name[0] == '.') {
                continue;
            }
            char path[PLAYER_PATH_SIZE];
            struct stat entry_info;
            join_path(path, sizeof(path), current_directory, entry->d_name);
            if (stat(path, &entry_info) == 0 &&
                (S_ISDIR(entry_info.st_mode) || is_wav_filename(entry->d_name))) {
                if (browser_entry_count == PLAYER_MAX_BROWSER_ENTRIES) {
                    break;
                }
                snprintf(browser_entries[browser_entry_count].name,
                         sizeof(browser_entries[browser_entry_count].name), "%s", entry->d_name);
                browser_entries[browser_entry_count].directory = S_ISDIR(entry_info.st_mode);
                add_browser_entry(&browser_entries[browser_entry_count++]);
                found = true;
            }
        }
        closedir(directory);
        if (!found && is_root_directory()) {
            lv_obj_t *empty = lv_label_create(content);
            lv_label_set_text(empty, "No WAV files or folders");
            lv_obj_set_style_text_color(empty, lv_color_white(), 0);
        }
    }

    lv_obj_t *back_button = lv_button_create(screen);
    lv_obj_set_size(back_button, 120, 52);
    ui_theme_apply_button(back_button);
    lv_obj_align(back_button, LV_ALIGN_BOTTOM_MID, -70, -22);
    lv_obj_add_event_cb(back_button, browser_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_center(back_label);

    clear_button = lv_button_create(screen);
    lv_obj_set_size(clear_button, 150, 52);
    ui_theme_apply_button(clear_button);
    lv_obj_align(clear_button, LV_ALIGN_BOTTOM_MID, 75, -22);
    lv_obj_add_event_cb(clear_button, request_clear_recordings, LV_EVENT_CLICKED, NULL);
    lv_obj_t *clear_label = lv_label_create(clear_button);
    lv_label_set_text(clear_label, "Clear recordings");
    lv_obj_set_style_text_color(clear_label, lv_color_white(), 0);
    lv_obj_center(clear_label);
}

static void refresh_player_ui(lv_timer_t *timer)
{
    (void)timer;

    /* Defer navigation and destructive work until all workers have quiesced. */
    if (leave_requested && !playback_task_running && !clear_task_running) {
        leave_requested = false;
        lv_async_call(return_to_main_menu, NULL);
        return;
    }
    if (browser_requested && !playback_task_running) {
        browser_requested = false;
        lv_async_call(show_browser, NULL);
        return;
    }
    if (play_after_stop && !playback_task_running) {
        play_after_stop = false;
        start_playback();
    }
    if (clear_requested && !playback_task_running && !clear_task_running) {
        clear_requested = false;
        clear_task_running = true;
        if (xTaskCreatePinnedToCoreWithCaps(clear_recordings_task, "clear_recordings", 4096,
                                            NULL, PLAYER_TASK_PRIORITY, NULL, PLAYER_TASK_CORE,
                                            PLAYER_TASK_CAPS) != pdPASS) {
            clear_task_running = false;
            clear_result = ESP_ERR_NO_MEM;
            clear_result_ready = true;
        }
    }
    if (clear_task_running) {
        lv_label_set_text(operation_status_label, "Clearing recordings...");
    } else if (clear_result_ready) {
        clear_result_ready = false;
        if (clear_result == ESP_OK) {
            lv_label_set_text(operation_status_label, "Recordings cleared");
        } else {
            lv_label_set_text_fmt(operation_status_label, "Clear failed: %d", clear_errno);
        }
        clear_completed = true;
        lv_obj_remove_state(clear_button, LV_STATE_DISABLED);
    }
    if (!player_visible || !player_screen_active || status_label == NULL) {
        return;
    }
    if (clear_task_running || clear_completed) {
        return;
    }
    if (player_state == PLAYER_PLAYING) {
        lv_label_set_text(status_label, "Playing");
        lv_label_set_text(play_label, LV_SYMBOL_PAUSE);
    } else if (player_state == PLAYER_PAUSED) {
        lv_label_set_text(status_label, "Paused");
        lv_label_set_text(play_label, LV_SYMBOL_PLAY);
    } else if (player_state == PLAYER_FINISHED) {
        lv_label_set_text(status_label, "Finished");
        lv_label_set_text(play_label, LV_SYMBOL_PLAY);
    } else if (player_state == PLAYER_FAILED) {
        lv_label_set_text_fmt(status_label, "Playback failed: %s",
                              esp_err_to_name(playback_result));
        lv_label_set_text(play_label, LV_SYMBOL_PLAY);
    } else {
        lv_label_set_text(status_label, "Stopped");
        lv_label_set_text(play_label, LV_SYMBOL_PLAY);
    }
    if (playback_total_bytes > 0) {
        uint32_t elapsed_seconds = playback_position_bytes / (22050 * 2);
        uint32_t total_seconds = playback_total_bytes / (22050 * 2);
        lv_label_set_text_fmt(elapsed_label, "%02u:%02u / %02u:%02u",
                              elapsed_seconds / 60, elapsed_seconds % 60,
                              total_seconds / 60, total_seconds % 60);
        lv_bar_set_value(progress_bar, playback_position_bytes * 100 / playback_total_bytes,
                         LV_ANIM_OFF);
    }
}

void multimedia_player_create(void)
{
    sleep_timer_pause();
    player_visible = true;
    leave_requested = false;
    player_screen_active = false;
    browser_requested = false;
    play_after_stop = false;
    selected_path[0] = '\0';
    selected_name[0] = '\0';
    playback_requested = false;
    playback_paused = false;
    playback_task_running = false;
    player_state = PLAYER_STOPPED;
    playback_position_bytes = 0;
    playback_total_bytes = 0;
    playback_volume = PLAYER_OUTPUT_VOLUME;
    playback_volume_changed = false;
    clear_task_running = false;
    clear_requested = false;
    clear_result_ready = false;
    clear_result = ESP_OK;
    clear_errno = 0;
    clear_completed = false;
    snprintf(current_directory, sizeof(current_directory), "%s", SD_CARD_MOUNT_PATH);

    esp_err_t result = sd_card_mount();
    if (result != ESP_OK) {
        lv_obj_t *screen = lv_screen_active();
        lv_obj_t *message = lv_label_create(screen);
        lv_label_set_text_fmt(message, "SD card unavailable: %s", esp_err_to_name(result));
        lv_obj_align(message, LV_ALIGN_CENTER, 0, -30);
        lv_obj_t *back_button = lv_button_create(screen);
        lv_obj_set_size(back_button, 110, 52);
        ui_theme_apply_button(back_button);
        lv_obj_align(back_button, LV_ALIGN_CENTER, 0, 36);
        lv_obj_add_event_cb(back_button, request_return_to_main_menu, LV_EVENT_CLICKED, NULL);
        lv_obj_t *back_label = lv_label_create(back_button);
        lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
        lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
        lv_obj_center(back_label);
        return;
    }

    show_browser(NULL);
    player_ui_timer = lv_timer_create(refresh_player_ui, PLAYER_UI_REFRESH_MS, NULL);
}
