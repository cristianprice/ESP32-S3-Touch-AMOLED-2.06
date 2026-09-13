#include "internet_radio.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "deep_sleep.h"
#include "main_menu.h"
#include "mp3dec.h"
#include "ui_theme.h"
#include "wifi_network.h"

#define RADIO_STREAM_URL "http://ice1.somafm.com/groovesalad-128-mp3"
#define RADIO_STATION_NAME "Groove Salad"
#define RADIO_SAMPLE_RATE 22050
#define RADIO_CHANNELS 1
#define RADIO_BITS_PER_SAMPLE 16
#define RADIO_VOLUME 100
#define RADIO_STREAM_BUFFER_SIZE 8192
#define RADIO_NETWORK_BUFFER_SIZE 2048
#define RADIO_NETWORK_BUFFER_COUNT 64
#define RADIO_AUDIO_BUFFER_COUNT 64
#define RADIO_AUDIO_PREBUFFER_COUNT 48
#define RADIO_NETWORK_TASK_STACK_SIZE (16 * 1024)
#define RADIO_AUDIO_TASK_STACK_SIZE 4096
#define RADIO_RESTART_TASK_STACK_SIZE 3072
#define RADIO_TASK_STACK_SIZE (24 * 1024)
#define RADIO_TASK_PRIORITY 4
#define RADIO_AUDIO_TASK_PRIORITY 6
#define RADIO_TASK_CORE 0
#define RADIO_AUDIO_TASK_CORE 1
#define RADIO_TASK_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#define RADIO_UI_REFRESH_MS 100
#define RADIO_MAX_SAMPLES_PER_FRAME (MAX_NGRAN * MAX_NSAMP * MAX_NCHAN)

typedef enum {
    RADIO_STOPPED,
    RADIO_BUFFERING,
    RADIO_PLAYING,
    RADIO_PAUSED,
    RADIO_FAILED,
} radio_state_t;

typedef struct {
    int16_t *samples;
    size_t byte_count;
} radio_audio_buffer_t;

typedef struct {
    uint8_t *data;
    size_t length;
} radio_network_buffer_t;

static const char *const TAG = "internet_radio";
static lv_timer_t *radio_ui_timer;
static lv_obj_t *status_label;
static lv_obj_t *play_label;
static lv_obj_t *volume_label;
static volatile radio_state_t radio_state;
static volatile bool radio_requested;
static volatile bool radio_paused;
static volatile bool radio_restart_requested;
static volatile bool radio_task_running;
static volatile bool radio_audio_task_running;
static volatile bool radio_network_task_running;
static volatile bool radio_restart_task_running;
static volatile bool volume_changed;
static volatile uint8_t radio_volume;
static volatile esp_err_t radio_result;
static bool radio_view_active;
static esp_codec_dev_handle_t radio_speaker;
static QueueHandle_t radio_available_buffers;
static QueueHandle_t radio_ready_buffers;
static int16_t *radio_output_buffers;
static QueueHandle_t radio_available_network_buffers;
static QueueHandle_t radio_ready_network_buffers;
static uint8_t *radio_network_buffers;

static void radio_stream_task(void *argument);
static void start_radio_task(void);

static void radio_restart_task(void *argument)
{
    (void)argument;

    if (radio_restart_requested && radio_view_active && !radio_task_running &&
        wifi_network_station_has_valid_ip()) {
        radio_restart_requested = false;
        start_radio_task();
    }
    radio_restart_task_running = false;
    vTaskDelete(NULL);
}

static void start_radio_task(void)
{
    radio_requested = true;
    radio_paused = false;
    radio_task_running = true;
    radio_state = RADIO_BUFFERING;
    if (xTaskCreatePinnedToCoreWithCaps(radio_stream_task, "internet_radio",
                                        RADIO_TASK_STACK_SIZE, NULL,
                                        RADIO_TASK_PRIORITY, NULL, RADIO_TASK_CORE,
                                        RADIO_TASK_CAPS) != pdPASS) {
        radio_requested = false;
        radio_task_running = false;
        radio_result = ESP_ERR_NO_MEM;
        radio_state = RADIO_FAILED;
    }
}

static void downmix_and_resample(const int16_t *input, int input_frames, int input_channels,
                                 int input_rate, int16_t *output, int *output_frames)
{
    int frames = input_frames * RADIO_SAMPLE_RATE / input_rate;
    if (frames < 1) {
        frames = 1;
    }
    for (int frame = 0; frame < frames; frame++) {
        int source_frame = frame * input_rate / RADIO_SAMPLE_RATE;
        if (source_frame >= input_frames) {
            source_frame = input_frames - 1;
        }
        int32_t sample = input[source_frame * input_channels];
        if (input_channels == 2) {
            sample = (sample + input[source_frame * input_channels + 1]) / 2;
        }
        output[frame] = (int16_t)sample;
    }
    *output_frames = frames;
}

static void radio_audio_task(void *argument)
{
    (void)argument;

    radio_audio_buffer_t buffer;
    while (radio_requested) {
        while (radio_requested && radio_paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!radio_requested) {
            break;
        }
        if (xQueueReceive(radio_ready_buffers, &buffer, pdMS_TO_TICKS(50)) != pdTRUE) {
            continue;
        }
        esp_err_t result = esp_codec_dev_write(radio_speaker, buffer.samples, buffer.byte_count);
        if (result != ESP_OK) {
            radio_result = result;
            radio_state = RADIO_FAILED;
            radio_requested = false;
            break;
        }
        if (xQueueSend(radio_available_buffers, &buffer.samples, 0) != pdTRUE) {
            radio_result = ESP_FAIL;
            radio_state = RADIO_FAILED;
            radio_requested = false;
            break;
        }
        radio_state = RADIO_PLAYING;
    }

    radio_audio_task_running = false;
    vTaskDelete(NULL);
}

static void create_main_menu(void *user_data)
{
    (void)user_data;

    main_menu_create();
    lv_obj_invalidate(lv_screen_active());
}

static void radio_network_task(void *argument)
{
    (void)argument;

    esp_http_client_config_t http_config = {
        .url = RADIO_STREAM_URL,
        .timeout_ms = 10000,
        .buffer_size = RADIO_NETWORK_BUFFER_SIZE,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t client = esp_http_client_init(&http_config);
    bool received_data = false;
    ESP_LOGI(TAG, "Opening stream");
    esp_err_t result = client == NULL ? ESP_ERR_NO_MEM : esp_http_client_open(client, 0);
    if (result == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status_code = esp_http_client_get_status_code(client);
        if (status_code < 200 || status_code >= 300) {
            result = ESP_ERR_INVALID_RESPONSE;
        } else {
            ESP_LOGI(TAG, "HTTP stream connected");
        }
    }

    while (result == ESP_OK && radio_requested) {
        uint8_t *buffer = NULL;
        if (xQueueReceive(radio_available_network_buffers, &buffer, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }

        if (!received_data) {
            ESP_LOGI(TAG, "Waiting for stream data");
        }
        int read = esp_http_client_read(client, (char *)buffer, RADIO_NETWORK_BUFFER_SIZE);
        if (read <= 0) {
            xQueueSend(radio_available_network_buffers, &buffer, 0);
            if (radio_requested) {
                result = ESP_FAIL;
            }
            break;
        }
        if (!received_data) {
            ESP_LOGI(TAG, "Receiving stream data");
            received_data = true;
        }

        radio_network_buffer_t network_buffer = {
            .data = buffer,
            .length = read,
        };
        if (xQueueSend(radio_ready_network_buffers, &network_buffer, pdMS_TO_TICKS(500)) != pdTRUE) {
            xQueueSend(radio_available_network_buffers, &buffer, 0);
            result = ESP_ERR_TIMEOUT;
            break;
        }
    }

    if (client != NULL) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    }
    if (result != ESP_OK && radio_requested) {
        radio_result = result;
        radio_state = RADIO_FAILED;
        radio_requested = false;
    }
    radio_network_task_running = false;
    vTaskDelete(NULL);
}

static void radio_stream_task(void *argument)
{
    (void)argument;

    uint8_t *stream_buffer = NULL;
    HMP3Decoder decoder = NULL;
    int16_t *pcm = NULL;
    size_t queued_buffer_count = 0;
    esp_err_t result = ESP_OK;

    result = bsp_audio_init(NULL);
    if (result != ESP_OK) {
        goto finish;
    }
    radio_speaker = bsp_audio_codec_speaker_init();
    if (radio_speaker == NULL) {
        result = ESP_FAIL;
        goto finish;
    }
    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = RADIO_BITS_PER_SAMPLE,
        .channel = RADIO_CHANNELS,
        .sample_rate = RADIO_SAMPLE_RATE,
    };
    result = esp_codec_dev_open(radio_speaker, &sample_info);
    if (result != ESP_OK) {
        goto finish;
    }
    result = esp_codec_dev_set_out_vol(radio_speaker, radio_volume);
    if (result != ESP_OK) {
        goto finish;
    }
    result = wifi_network_set_power_save(WIFI_PS_NONE);
    if (result != ESP_OK) {
        goto finish;
    }
    ESP_LOGI(TAG, "Audio output ready");

    ESP_LOGI(TAG, "Allocating radio buffers");
    stream_buffer = heap_caps_malloc(RADIO_STREAM_BUFFER_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (stream_buffer == NULL) {
        result = ESP_ERR_NO_MEM;
        goto finish;
    }
    decoder = MP3InitDecoder();
    pcm = heap_caps_malloc(sizeof(*pcm) * RADIO_MAX_SAMPLES_PER_FRAME,
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    radio_output_buffers = heap_caps_malloc(
        sizeof(*radio_output_buffers) * RADIO_MAX_SAMPLES_PER_FRAME * RADIO_AUDIO_BUFFER_COUNT,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    radio_available_buffers = xQueueCreate(RADIO_AUDIO_BUFFER_COUNT, sizeof(int16_t *));
    radio_ready_buffers = xQueueCreate(RADIO_AUDIO_BUFFER_COUNT, sizeof(radio_audio_buffer_t));
    radio_network_buffers =
        heap_caps_malloc(RADIO_NETWORK_BUFFER_SIZE * RADIO_NETWORK_BUFFER_COUNT,
                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    radio_available_network_buffers =
        xQueueCreate(RADIO_NETWORK_BUFFER_COUNT, sizeof(uint8_t *));
    radio_ready_network_buffers =
        xQueueCreate(RADIO_NETWORK_BUFFER_COUNT, sizeof(radio_network_buffer_t));
    if (decoder == NULL || pcm == NULL || radio_output_buffers == NULL ||
        radio_available_buffers == NULL || radio_ready_buffers == NULL || radio_network_buffers == NULL ||
        radio_available_network_buffers == NULL || radio_ready_network_buffers == NULL) {
        result = ESP_ERR_NO_MEM;
        goto finish;
    }
    ESP_LOGI(TAG, "Radio buffers ready");
    for (size_t index = 0; index < RADIO_AUDIO_BUFFER_COUNT; index++) {
        int16_t *buffer = radio_output_buffers + index * RADIO_MAX_SAMPLES_PER_FRAME;
        if (xQueueSend(radio_available_buffers, &buffer, 0) != pdTRUE) {
            result = ESP_FAIL;
            goto finish;
        }
    }
    for (size_t index = 0; index < RADIO_NETWORK_BUFFER_COUNT; index++) {
        uint8_t *buffer = radio_network_buffers + index * RADIO_NETWORK_BUFFER_SIZE;
        if (xQueueSend(radio_available_network_buffers, &buffer, 0) != pdTRUE) {
            result = ESP_FAIL;
            goto finish;
        }
    }
    int buffered = 0;
    radio_state = RADIO_BUFFERING;
    ESP_LOGI(TAG, "Starting network reader");
    radio_network_task_running = true;
    if (xTaskCreatePinnedToCoreWithCaps(radio_network_task, "radio_network",
                                        RADIO_NETWORK_TASK_STACK_SIZE, NULL, RADIO_TASK_PRIORITY, NULL,
                                        RADIO_TASK_CORE, RADIO_TASK_CAPS) != pdPASS) {
        radio_network_task_running = false;
        result = ESP_ERR_NO_MEM;
        goto finish;
    }

    while (radio_requested) {
        while (radio_requested && radio_paused) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (!radio_requested) {
            break;
        }
        if (volume_changed) {
            result = esp_codec_dev_set_out_vol(radio_speaker, radio_volume);
            if (result != ESP_OK) {
                goto finish;
            }
            volume_changed = false;
        }
        radio_network_buffer_t network_buffer;
        if (xQueueReceive(radio_ready_network_buffers, &network_buffer, pdMS_TO_TICKS(100)) != pdTRUE) {
            continue;
        }
        if (network_buffer.length > RADIO_STREAM_BUFFER_SIZE - buffered) {
            xQueueSend(radio_available_network_buffers, &network_buffer.data, 0);
            result = ESP_ERR_INVALID_SIZE;
            goto finish;
        }
        memcpy(stream_buffer + buffered, network_buffer.data, network_buffer.length);
        buffered += network_buffer.length;
        if (xQueueSend(radio_available_network_buffers, &network_buffer.data, 0) != pdTRUE) {
            result = ESP_FAIL;
            goto finish;
        }
        if (buffered < RADIO_STREAM_BUFFER_SIZE / 2) {
            continue;
        }

        while (radio_requested) {
            int sync_offset = MP3FindSyncWord(stream_buffer, buffered);
            if (sync_offset < 0) {
                if (buffered > 3) {
                    memmove(stream_buffer, stream_buffer + buffered - 3, 3);
                    buffered = 3;
                }
                break;
            }
            if (sync_offset > 0) {
                memmove(stream_buffer, stream_buffer + sync_offset, buffered - sync_offset);
                buffered -= sync_offset;
                continue;
            }

            unsigned char *input = stream_buffer;
            int bytes_left = buffered;
            int decode_result = MP3Decode(decoder, &input, &bytes_left, pcm, 0);
            if (decode_result == ERR_MP3_INDATA_UNDERFLOW ||
                decode_result == ERR_MP3_MAINDATA_UNDERFLOW) {
                break;
            }
            if (decode_result != ERR_MP3_NONE) {
                memmove(stream_buffer, stream_buffer + 1, --buffered);
                continue;
            }
            if (input <= stream_buffer || bytes_left >= buffered) {
                result = ESP_ERR_INVALID_RESPONSE;
                goto finish;
            }
            buffered = bytes_left;
            memmove(stream_buffer, input, buffered);

            int output_frames;
            int16_t *output = NULL;
            if (xQueueReceive(radio_available_buffers, &output, pdMS_TO_TICKS(500)) != pdTRUE) {
                result = ESP_ERR_TIMEOUT;
                goto finish;
            }
            MP3FrameInfo frame_info;
            MP3GetLastFrameInfo(decoder, &frame_info);
            if (frame_info.nChans < 1 || frame_info.nChans > 2 ||
                frame_info.outputSamps <= 0 || frame_info.samprate < RADIO_SAMPLE_RATE) {
                xQueueSend(radio_available_buffers, &output, 0);
                continue;
            }
            downmix_and_resample(pcm, frame_info.outputSamps / frame_info.nChans,
                                 frame_info.nChans, frame_info.samprate, output, &output_frames);
            radio_audio_buffer_t audio_buffer = {
                .samples = output,
                .byte_count = output_frames * sizeof(*output),
            };
            if (xQueueSend(radio_ready_buffers, &audio_buffer, pdMS_TO_TICKS(500)) != pdTRUE) {
                result = ESP_ERR_TIMEOUT;
                goto finish;
            }
            queued_buffer_count++;
            if (!radio_audio_task_running && queued_buffer_count >= RADIO_AUDIO_PREBUFFER_COUNT) {
                radio_audio_task_running = true;
                if (xTaskCreatePinnedToCoreWithCaps(radio_audio_task, "radio_audio",
                                                    RADIO_AUDIO_TASK_STACK_SIZE, NULL,
                                                    RADIO_AUDIO_TASK_PRIORITY, NULL, RADIO_AUDIO_TASK_CORE,
                                                    RADIO_TASK_CAPS) != pdPASS) {
                    radio_audio_task_running = false;
                    result = ESP_ERR_NO_MEM;
                    goto finish;
                }
            }
        }
    }

    result = ESP_OK;

finish:
    radio_requested = false;
    while (radio_network_task_running) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    while (radio_audio_task_running) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (radio_ready_buffers != NULL) {
        vQueueDelete(radio_ready_buffers);
        radio_ready_buffers = NULL;
    }
    if (radio_available_buffers != NULL) {
        vQueueDelete(radio_available_buffers);
        radio_available_buffers = NULL;
    }
    if (radio_output_buffers != NULL) {
        heap_caps_free(radio_output_buffers);
        radio_output_buffers = NULL;
    }
    if (radio_ready_network_buffers != NULL) {
        vQueueDelete(radio_ready_network_buffers);
        radio_ready_network_buffers = NULL;
    }
    if (radio_available_network_buffers != NULL) {
        vQueueDelete(radio_available_network_buffers);
        radio_available_network_buffers = NULL;
    }
    if (radio_network_buffers != NULL) {
        heap_caps_free(radio_network_buffers);
        radio_network_buffers = NULL;
    }
    if (pcm != NULL) {
        heap_caps_free(pcm);
    }
    if (decoder != NULL) {
        MP3FreeDecoder(decoder);
    }
    if (stream_buffer != NULL) {
        heap_caps_free(stream_buffer);
    }
    if (radio_speaker != NULL) {
        esp_codec_dev_close(radio_speaker);
        esp_codec_dev_delete(radio_speaker);
        radio_speaker = NULL;
    }
    esp_err_t power_save_result = wifi_network_set_power_save(WIFI_PS_MIN_MODEM);
    if (power_save_result != ESP_OK && power_save_result != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "Unable to restore Wi-Fi power saving: %s",
                 esp_err_to_name(power_save_result));
    }
    radio_result = result;
    bool restart = radio_restart_requested && radio_view_active &&
                   wifi_network_station_has_valid_ip();
    radio_restart_requested = restart;
    radio_task_running = false;
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "Stream failed: %s", esp_err_to_name(result));
        radio_state = RADIO_FAILED;
    } else if (!radio_requested) {
        radio_state = RADIO_STOPPED;
    }
    if (restart) {
        radio_restart_task_running = true;
        if (xTaskCreatePinnedToCoreWithCaps(radio_restart_task, "radio_restart",
                                            RADIO_RESTART_TASK_STACK_SIZE, NULL,
                                            RADIO_TASK_PRIORITY - 1, NULL, RADIO_TASK_CORE,
                                            RADIO_TASK_CAPS) != pdPASS) {
            radio_restart_task_running = false;
            radio_restart_requested = false;
            radio_result = ESP_ERR_NO_MEM;
            radio_state = RADIO_FAILED;
        }
    }
    vTaskDelete(NULL);
}

static void start_radio(lv_event_t *event)
{
    (void)event;

    if (radio_task_running) {
        if (!radio_requested) {
            radio_restart_requested = true;
            radio_state = RADIO_BUFFERING;
            return;
        }
        radio_paused = !radio_paused;
        radio_state = radio_paused ? RADIO_PAUSED : RADIO_PLAYING;
        return;
    }
    if (!wifi_network_station_has_valid_ip()) {
        radio_result = ESP_ERR_INVALID_STATE;
        radio_state = RADIO_FAILED;
        return;
    }
    start_radio_task();
}

static void update_volume(lv_event_t *event)
{
    radio_volume = lv_slider_get_value(lv_event_get_target(event));
    volume_changed = true;
    lv_label_set_text_fmt(volume_label, "Volume %u%%", radio_volume);
}

static void return_to_main_menu(void *user_data)
{
    (void)user_data;

    radio_view_active = false;
    radio_restart_requested = false;
    radio_requested = false;
    radio_paused = false;
    if (radio_ui_timer != NULL) {
        lv_timer_delete(radio_ui_timer);
        radio_ui_timer = NULL;
    }
    lv_obj_clean(lv_screen_active());
    lv_async_call(create_main_menu, NULL);
    sleep_timer_resume();
}

static void request_main_menu(lv_event_t *event)
{
    (void)event;
    lv_async_call(return_to_main_menu, NULL);
}

static void refresh_radio_ui(lv_timer_t *timer)
{
    (void)timer;

    if (!radio_view_active) {
        return;
    }
    if (radio_state == RADIO_BUFFERING) {
        lv_label_set_text(status_label, "Buffering stream...");
        lv_label_set_text(play_label, LV_SYMBOL_STOP);
    } else if (radio_state == RADIO_PLAYING) {
        lv_label_set_text(status_label, "Playing");
        lv_label_set_text(play_label, LV_SYMBOL_PAUSE);
    } else if (radio_state == RADIO_PAUSED) {
        lv_label_set_text(status_label, "Paused");
        lv_label_set_text(play_label, LV_SYMBOL_PLAY);
    } else if (radio_state == RADIO_FAILED) {
        lv_label_set_text_fmt(status_label, "Radio unavailable: %s", esp_err_to_name(radio_result));
        lv_label_set_text(play_label, LV_SYMBOL_PLAY);
    } else {
        lv_label_set_text(status_label, "Press Play to start");
        lv_label_set_text(play_label, LV_SYMBOL_PLAY);
    }
}

void internet_radio_create(void)
{
    sleep_timer_pause();
    radio_view_active = true;
    radio_requested = false;
    radio_paused = false;
    radio_restart_requested = false;
    radio_task_running = false;
    radio_state = RADIO_STOPPED;
    radio_result = ESP_OK;
    radio_volume = RADIO_VOLUME;
    volume_changed = false;

    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_grad_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Internet radio");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 42);

    lv_obj_t *station = lv_label_create(screen);
    lv_label_set_text(station, RADIO_STATION_NAME);
    lv_obj_set_style_text_font(station, &lv_font_montserrat_40, 0);
    lv_obj_set_style_text_color(station, lv_color_hex(0xFFB35C), 0);
    lv_obj_align(station, LV_ALIGN_TOP_MID, 0, 108);

    status_label = lv_label_create(screen);
    lv_obj_set_width(status_label, 350);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_white(), 0);
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 168);

    lv_obj_t *play_button = lv_button_create(screen);
    lv_obj_set_size(play_button, 84, 84);
    ui_theme_apply_button(play_button);
    lv_obj_set_style_radius(play_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_align(play_button, LV_ALIGN_TOP_MID, 0, 212);
    lv_obj_add_event_cb(play_button, start_radio, LV_EVENT_CLICKED, NULL);
    play_label = lv_label_create(play_button);
    lv_label_set_text(play_label, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(play_label, &lv_font_montserrat_48, 0);
    lv_obj_center(play_label);

    volume_label = lv_label_create(screen);
    lv_label_set_text_fmt(volume_label, "Volume %u%%", radio_volume);
    lv_obj_set_style_text_color(volume_label, lv_color_hex(0xFFB35C), 0);
    lv_obj_align(volume_label, LV_ALIGN_TOP_MID, 0, 332);

    lv_obj_t *volume_slider = lv_slider_create(screen);
    lv_obj_set_size(volume_slider, 250, 14);
    lv_slider_set_range(volume_slider, 0, 100);
    lv_slider_set_value(volume_slider, radio_volume, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0x1C1C1C), 0);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0xFF7A00), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_slider, lv_color_hex(0xFFB35C), LV_PART_KNOB);
    lv_obj_align(volume_slider, LV_ALIGN_TOP_MID, 0, 362);
    lv_obj_add_event_cb(volume_slider, update_volume, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *back_button = lv_button_create(screen);
    lv_obj_set_size(back_button, 110, 44);
    ui_theme_apply_button(back_button);
    lv_obj_align(back_button, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_add_event_cb(back_button, request_main_menu, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back");
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_center(back_label);

    radio_ui_timer = lv_timer_create(refresh_radio_ui, RADIO_UI_REFRESH_MS, NULL);
}
