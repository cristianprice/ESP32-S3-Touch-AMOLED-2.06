idf_component_register(SRCS "main.c" "main_menu.c" "motion_detection.c" "steps_meter.c" "settings_ui.c" "ftp_server.c" "wifi_network.c" "sd_card.c" "ui_theme.c" "time_mgmt.c" "power_button.c" "deep_sleep.c"
                       "assets/abstract_timekeeper.c" "assets/chart_icon.c" "assets/ftp_icon.c" "assets/running_icon.c" "assets/settings_icon.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_event esp_http_server esp_netif esp_wifi nvs_flash spiffs)

target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE)
