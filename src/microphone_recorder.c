#include "microphone_recorder.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "deep_sleep.h"
#include "main_menu.h"
#include "sd_card.h"
#include "ui_theme.h"

/*
 * Captures mono PCM from the BSP microphone to timestamped WAV files. The
 * core-0 recording task owns audio, file, and capture-buffer resources; it
 * publishes scalar state and spectrum levels for an LVGL timer to render.
 * Generation checks prevent a completed worker from updating a replaced view.
 */
#define RECORDING_DIRECTORY SD_CARD_MOUNT_PATH "/recordings"
#define RECORDING_PATH_SIZE 128
#define RECORDING_SAMPLE_RATE 22050
#define RECORDING_BITS_PER_SAMPLE 16
#define RECORDING_CHANNELS 1
#define RECORDING_MICROPHONE_GAIN_DB 30.0f
#define RECORDING_BUFFER_SIZE 2048
#define RECORDING_UI_REFRESH_MS 100
#define EQUALIZER_BAND_COUNT 8
#define EQUALIZER_ANALYSIS_SAMPLE_COUNT 256
#define EQUALIZER_TWO_PI 6.28318530718f
#define EQUALIZER_MIN_AMPLITUDE 0.0005f
#define EQUALIZER_MAX_AMPLITUDE 0.25f
#define RECORDING_TASK_PRIORITY 4
#define RECORDING_TASK_CORE 0
#define RECORDING_TASK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

typedef enum {
    RECORDING_STARTING,
    RECORDING_ACTIVE,
    RECORDING_STOPPING,
    RECORDING_SAVED,
    RECORDING_FAILED,
} recording_state_t;

typedef enum {
    RECORDING_FAILURE_NONE,
    RECORDING_FAILURE_SD_MOUNT,
    RECORDING_FAILURE_DIRECTORY_CREATE,
    RECORDING_FAILURE_FILE_CREATE,
    RECORDING_FAILURE_HEADER_WRITE,
    RECORDING_FAILURE_AUDIO_INIT,
    RECORDING_FAILURE_MICROPHONE_INIT,
    RECORDING_FAILURE_AUDIO_OPEN,
    RECORDING_FAILURE_MICROPHONE_GAIN,
    RECORDING_FAILURE_BUFFER_ALLOCATE,
    RECORDING_FAILURE_AUDIO_READ,
    RECORDING_FAILURE_DATA_WRITE,
    RECORDING_FAILURE_HEADER_FINALIZE,
} recording_failure_stage_t;

typedef struct __attribute__((packed)) {
    char riff[4];
    uint32_t file_size;
    char wave[4];
    char format[4];
    uint32_t format_size;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data[4];
    uint32_t data_size;
} wav_header_t;

static const char *const TAG = "microphone_recorder";
static lv_timer_t *recording_ui_timer;
static lv_obj_t *elapsed_label;
static lv_obj_t *status_label;
static lv_obj_t *stop_button;
static lv_obj_t *stop_label;
static lv_obj_t *equalizer_chart;
static lv_chart_series_t *equalizer_series;
static volatile recording_state_t recording_state;
static volatile bool recording_requested;
static volatile int64_t recording_started_at_us;
static volatile esp_err_t recording_result;
static volatile recording_failure_stage_t recording_failure_stage;
static volatile int recording_failure_errno;
static uint32_t view_generation;
static bool view_active;
static char recording_path[RECORDING_PATH_SIZE];
static volatile uint8_t equalizer_levels[EQUALIZER_BAND_COUNT];
static const uint16_t equalizer_frequencies_hz[EQUALIZER_BAND_COUNT] = {
    150, 300, 600, 1200, 2400, 4800, 7200, 9600,
};

static const char *recording_failure_stage_name(recording_failure_stage_t stage)
{
    switch (stage) {
    case RECORDING_FAILURE_SD_MOUNT:
        return "SD card mount";
    case RECORDING_FAILURE_DIRECTORY_CREATE:
        return "recordings folder";
    case RECORDING_FAILURE_FILE_CREATE:
        return "WAV file create";
    case RECORDING_FAILURE_HEADER_WRITE:
        return "WAV header write";
    case RECORDING_FAILURE_AUDIO_INIT:
        return "audio initialization";
    case RECORDING_FAILURE_MICROPHONE_INIT:
        return "microphone initialization";
    case RECORDING_FAILURE_AUDIO_OPEN:
        return "microphone open";
    case RECORDING_FAILURE_MICROPHONE_GAIN:
        return "microphone gain";
    case RECORDING_FAILURE_BUFFER_ALLOCATE:
        return "capture buffer";
    case RECORDING_FAILURE_AUDIO_READ:
        return "microphone capture";
    case RECORDING_FAILURE_DATA_WRITE:
        return "audio data write";
    case RECORDING_FAILURE_HEADER_FINALIZE:
        return "WAV finalization";
    case RECORDING_FAILURE_NONE:
    default:
        return "unknown stage";
    }
}

static wav_header_t make_wav_header(uint32_t data_size)
{
    /* RIFF chunk sizes exclude their own 8-byte ID/size prefix. */
    wav_header_t header = {
        .riff = {'R', 'I', 'F', 'F'},
        .file_size = data_size + sizeof(wav_header_t) - 8,
        .wave = {'W', 'A', 'V', 'E'},
        .format = {'f', 'm', 't', ' '},
        .format_size = 16,
        .audio_format = 1,
        .channels = RECORDING_CHANNELS,
        .sample_rate = RECORDING_SAMPLE_RATE,
        .byte_rate = RECORDING_SAMPLE_RATE * RECORDING_CHANNELS * RECORDING_BITS_PER_SAMPLE / 8,
        .block_align = RECORDING_CHANNELS * RECORDING_BITS_PER_SAMPLE / 8,
        .bits_per_sample = RECORDING_BITS_PER_SAMPLE,
        .data = {'d', 'a', 't', 'a'},
        .data_size = data_size,
    };
    return header;
}

static void update_equalizer_levels(const int16_t *samples, size_t sample_count)
{
    if (sample_count < EQUALIZER_ANALYSIS_SAMPLE_COUNT) {
        return;
    }

    float mean = 0.0f;
    for (size_t sample = 0; sample < EQUALIZER_ANALYSIS_SAMPLE_COUNT; sample++) {
        mean += (float)samples[sample] / 32768.0f;
    }
    mean /= EQUALIZER_ANALYSIS_SAMPLE_COUNT;

    /* Goertzel-style single-bin analysis is compact enough for the capture task. */
    for (size_t band = 0; band < EQUALIZER_BAND_COUNT; band++) {
        float normalized_frequency =
            (float)equalizer_frequencies_hz[band] / RECORDING_SAMPLE_RATE;
        float coefficient = 2.0f * cosf(EQUALIZER_TWO_PI * normalized_frequency);
        float previous = 0.0f;
        float current = 0.0f;
        for (size_t sample = 0; sample < EQUALIZER_ANALYSIS_SAMPLE_COUNT; sample++) {
            float next = (float)samples[sample] / 32768.0f - mean +
                         coefficient * current - previous;
            previous = current;
            current = next;
        }

        float magnitude = sqrtf(current * current + previous * previous -
                                coefficient * current * previous) *
                          2.0f / EQUALIZER_ANALYSIS_SAMPLE_COUNT;
        float level_ratio = log10f(fmaxf(magnitude, EQUALIZER_MIN_AMPLITUDE) /
                                   EQUALIZER_MIN_AMPLITUDE) /
                            log10f(EQUALIZER_MAX_AMPLITUDE / EQUALIZER_MIN_AMPLITUDE);
        uint32_t level = (uint32_t)(fminf(fmaxf(level_ratio, 0.0f), 1.0f) * 100.0f);
        equalizer_levels[band] = (equalizer_levels[band] * 3 + level) / 4;
    }
}

static esp_err_t create_recording_path(char *path, size_t path_size)
{
    struct stat directory_info;
    if (stat(RECORDING_DIRECTORY, &directory_info) != 0) {
        mkdir(RECORDING_DIRECTORY, 0775);
        if (stat(RECORDING_DIRECTORY, &directory_info) != 0) {
            recording_failure_errno = errno;
            ESP_LOGE(TAG, "Failed to create %s: errno %d (%s)", RECORDING_DIRECTORY,
                     recording_failure_errno, strerror(recording_failure_errno));
            return ESP_FAIL;
        }
    }
    if (!S_ISDIR(directory_info.st_mode)) {
        ESP_LOGE(TAG, "%s exists but is not a directory", RECORDING_DIRECTORY);
        return ESP_ERR_INVALID_STATE;
    }

    time_t now = time(NULL);
    struct tm timestamp;
    if (localtime_r(&now, &timestamp) == NULL) {
        return ESP_FAIL;
    }

    int length = snprintf(path, path_size, RECORDING_DIRECTORY
                          "/recording-%04d-%02d-%02d-%02d-%02d-%02d.wav",
                          timestamp.tm_year + 1900, timestamp.tm_mon + 1, timestamp.tm_mday,
                          timestamp.tm_hour, timestamp.tm_min, timestamp.tm_sec);
    return length >= 0 && (size_t)length < path_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void recording_task(void *argument)
{
    uint32_t task_generation = (uint32_t)(uintptr_t)argument;
    esp_codec_dev_handle_t microphone = NULL;
    uint8_t *buffer = NULL;
    FILE *file = NULL;
    uint32_t data_size = 0;
    esp_err_t result = sd_card_mount();
    if (result != ESP_OK) {
        recording_failure_stage = RECORDING_FAILURE_SD_MOUNT;
        goto finish;
    }
    result = create_recording_path(recording_path, sizeof(recording_path));
    if (result != ESP_OK) {
        recording_failure_stage = RECORDING_FAILURE_DIRECTORY_CREATE;
        goto finish;
    }
    file = fopen(recording_path, "wb");
    if (file == NULL) {
        result = ESP_FAIL;
        recording_failure_stage = RECORDING_FAILURE_FILE_CREATE;
        goto finish;
    }

    /* Write a placeholder header, then seek back with the final byte count on success. */
    wav_header_t header = make_wav_header(0);
    if (fwrite(&header, 1, sizeof(header), file) != sizeof(header)) {
        result = ESP_FAIL;
        recording_failure_stage = RECORDING_FAILURE_HEADER_WRITE;
        goto finish;
    }

    result = bsp_audio_init(NULL);
    if (result != ESP_OK) {
        recording_failure_stage = RECORDING_FAILURE_AUDIO_INIT;
        goto finish;
    }
    microphone = bsp_audio_codec_microphone_init();
    if (microphone == NULL) {
        result = ESP_FAIL;
        recording_failure_stage = RECORDING_FAILURE_MICROPHONE_INIT;
        goto finish;
    }

    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = RECORDING_BITS_PER_SAMPLE,
        .channel = RECORDING_CHANNELS,
        .channel_mask = 0,
        .sample_rate = RECORDING_SAMPLE_RATE,
        .mclk_multiple = 0,
    };
    result = esp_codec_dev_open(microphone, &sample_info);
    if (result != ESP_OK) {
        recording_failure_stage = RECORDING_FAILURE_AUDIO_OPEN;
        goto finish;
    }
    result = esp_codec_dev_set_in_gain(microphone, RECORDING_MICROPHONE_GAIN_DB);
    if (result != ESP_OK) {
        recording_failure_stage = RECORDING_FAILURE_MICROPHONE_GAIN;
        goto finish;
    }
    buffer = heap_caps_malloc(RECORDING_BUFFER_SIZE, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        result = ESP_ERR_NO_MEM;
        recording_failure_stage = RECORDING_FAILURE_BUFFER_ALLOCATE;
        goto finish;
    }

    recording_started_at_us = esp_timer_get_time();
    recording_state = RECORDING_ACTIVE;
    while (recording_requested) {
        result = esp_codec_dev_read(microphone, buffer, RECORDING_BUFFER_SIZE);
        if (result != ESP_OK) {
            recording_failure_stage = RECORDING_FAILURE_AUDIO_READ;
            goto finish;
        }
        if (fwrite(buffer, 1, RECORDING_BUFFER_SIZE, file) != RECORDING_BUFFER_SIZE) {
            result = ESP_FAIL;
            recording_failure_stage = RECORDING_FAILURE_DATA_WRITE;
            goto finish;
        }
        update_equalizer_levels((const int16_t *)buffer,
                                RECORDING_BUFFER_SIZE / sizeof(int16_t));
        data_size += RECORDING_BUFFER_SIZE;
    }

    header = make_wav_header(data_size);
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fwrite(&header, 1, sizeof(header), file) != sizeof(header) || fflush(file) != 0) {
        result = ESP_FAIL;
        recording_failure_stage = RECORDING_FAILURE_HEADER_FINALIZE;
        goto finish;
    }
    result = ESP_OK;

finish:
    /*
     * Release in reverse acquisition order. A failed partial capture is
     * removed so the player never presents a WAV with a stale header.
     */
    if (buffer != NULL) {
        heap_caps_free(buffer);
    }
    if (microphone != NULL) {
        esp_codec_dev_close(microphone);
        esp_codec_dev_delete(microphone);
    }
    if (file != NULL) {
        fclose(file);
    }
    if (result != ESP_OK && recording_path[0] != '\0') {
        remove(recording_path);
    }
    sd_card_unmount();

    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Recording failed at %s: %s",
                 recording_failure_stage_name(recording_failure_stage), esp_err_to_name(result));
    }
    /* The UI may have been destroyed while capture unwound; guard its state update. */
    if (view_active && task_generation == view_generation) {
        recording_result = result;
        recording_state = result == ESP_OK ? RECORDING_SAVED : RECORDING_FAILED;
    }
    vTaskDelete(NULL);
}

static void return_to_main_menu(void *user_data)
{
    (void)user_data;

    view_active = false;
    view_generation++;
    if (recording_ui_timer != NULL) {
        lv_timer_delete(recording_ui_timer);
        recording_ui_timer = NULL;
    }
    lv_obj_clean(lv_screen_active());
    main_menu_create();
    sleep_timer_resume();
}

static void refresh_recording_ui(lv_timer_t *timer)
{
    (void)timer;

    /* LVGL objects are touched only by this UI timer, never by recording_task. */
    if (recording_state == RECORDING_ACTIVE) {
        int64_t elapsed_seconds = (esp_timer_get_time() - recording_started_at_us) / 1000000;
        lv_label_set_text_fmt(elapsed_label, "Recording %02" PRId64 ":%02" PRId64,
                              elapsed_seconds / 60, elapsed_seconds % 60);
        lv_label_set_text(status_label, "Recording...");
        for (uint32_t band = 0; band < EQUALIZER_BAND_COUNT; band++) {
            lv_chart_set_series_value_by_id(equalizer_chart, equalizer_series, band,
                                            equalizer_levels[band]);
        }
        lv_chart_refresh(equalizer_chart);
    }
    if (recording_state == RECORDING_SAVED) {
        lv_label_set_text(status_label, "Recording saved");
        lv_label_set_text(stop_label, LV_SYMBOL_LEFT);
        lv_obj_remove_state(stop_button, LV_STATE_DISABLED);
    } else if (recording_state == RECORDING_FAILED) {
        lv_label_set_text_fmt(status_label, "%s failed: %s",
                              recording_failure_stage_name(recording_failure_stage),
                              esp_err_to_name(recording_result));
        if (recording_failure_stage == RECORDING_FAILURE_DIRECTORY_CREATE) {
            lv_label_set_text_fmt(status_label, "Folder failed: %d (%s)",
                                  recording_failure_errno, strerror(recording_failure_errno));
        }
        lv_label_set_text(stop_label, LV_SYMBOL_LEFT);
        lv_obj_remove_state(stop_button, LV_STATE_DISABLED);
    }
}

static void request_stop_recording(lv_event_t *event)
{
    (void)event;

    if (recording_state == RECORDING_SAVED || recording_state == RECORDING_FAILED) {
        lv_async_call(return_to_main_menu, NULL);
        return;
    }
    if (recording_state != RECORDING_STARTING && recording_state != RECORDING_ACTIVE) {
        return;
    }
    recording_requested = false;
    recording_state = RECORDING_STOPPING;
    lv_obj_add_state(stop_button, LV_STATE_DISABLED);
    lv_label_set_text(status_label, "Saving recording...");
}

void microphone_recorder_create(void)
{
    sleep_timer_pause();
    view_active = true;
    view_generation++;
    recording_path[0] = '\0';
    recording_requested = true;
    recording_state = RECORDING_STARTING;
    recording_result = ESP_OK;
    recording_failure_stage = RECORDING_FAILURE_NONE;
    recording_failure_errno = 0;
    for (size_t band = 0; band < EQUALIZER_BAND_COUNT; band++) {
        equalizer_levels[band] = 0;
    }

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Voice recorder");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

    elapsed_label = lv_label_create(screen);
    lv_label_set_text(elapsed_label, "Recording 00:00");
    lv_obj_set_style_text_font(elapsed_label, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(elapsed_label, lv_color_white(), 0);
    lv_obj_align(elapsed_label, LV_ALIGN_TOP_MID, 0, 96);

    equalizer_chart = lv_chart_create(screen);
    lv_obj_set_size(equalizer_chart, 330, 90);
    lv_obj_align(equalizer_chart, LV_ALIGN_TOP_MID, 0, 162);
    lv_chart_set_type(equalizer_chart, LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(equalizer_chart, EQUALIZER_BAND_COUNT);
    lv_chart_set_range(equalizer_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(equalizer_chart, 3, 0);
    equalizer_series = lv_chart_add_series(equalizer_chart, lv_color_hex(0xFF7A00),
                                           LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_value(equalizer_chart, equalizer_series, 0);

    status_label = lv_label_create(screen);
    lv_label_set_text(status_label, "Starting microphone...");
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xFFB35C), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 276);

    stop_button = lv_button_create(screen);
    lv_obj_set_size(stop_button, 84, 84);
    ui_theme_apply_button(stop_button);
    lv_obj_set_style_radius(stop_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(stop_button, LV_ALIGN_BOTTOM_MID, 0, -48);
    lv_obj_add_event_cb(stop_button, request_stop_recording, LV_EVENT_CLICKED, NULL);

    stop_label = lv_label_create(stop_button);
    lv_label_set_text(stop_label, LV_SYMBOL_STOP);
    lv_obj_set_style_text_font(stop_label, &lv_font_montserrat_48, 0);
    lv_obj_center(stop_label);

    recording_ui_timer = lv_timer_create(refresh_recording_ui, RECORDING_UI_REFRESH_MS, NULL);
    if (xTaskCreatePinnedToCoreWithCaps(recording_task, "microphone_record", 6144,
                                        (void *)(uintptr_t)view_generation, RECORDING_TASK_PRIORITY,
                                        NULL, RECORDING_TASK_CORE, RECORDING_TASK_CAPS) != pdPASS) {
        recording_requested = false;
        recording_result = ESP_ERR_NO_MEM;
        recording_state = RECORDING_FAILED;
    }
}
