idf_component_register(SRCS "main.c" "main_menu.c" "motion_detection.c" "steps_meter.c" "ntp_client.c" "microphone_recorder.c" "multimedia_player.c" "wifi_manager.c" "internet_radio.c" "settings_ui.c" "ftp_server.c" "wifi_network.c" "sd_card.c" "ui_theme.c" "time_mgmt.c" "power_button.c" "deep_sleep.c"
                       "assets/abstract_timekeeper.c" "assets/chart_icon.c" "assets/ftp_icon.c" "assets/microphone_icon.c" "assets/multimedia_player_icon.c" "assets/ntp_client_icon.c" "assets/radio_station_icon.c" "assets/running_icon.c" "assets/settings_icon.c" "assets/wifi_icon.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_event esp_http_client esp_http_server esp_lcd esp_netif esp_wifi nvs_flash spiffs chmorgan__esp-libhelix-mp3)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
