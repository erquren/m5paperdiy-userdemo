set(APP_SRCS
    "./clock/time_service.cpp"
    "./clock/clock_fonts.cpp"
    "./network/wifi_manager.cpp"
    "./network/captive_dns.cpp"
    # HAL
    "./hal/hal.cpp"
    "./hal/hal_button.cpp"
    "./hal/hal_display.cpp"
    "./hal/hal_tf_card.cpp"

    # Applications
    "./apps/app_clock/app_clock.cpp"
    "./apps/app_battery/app_battery.cpp"
    "./apps/app_button_feedback/app_button_feedback.cpp"
    "./apps/app_shutdown/app_shutdown.cpp"
    "./apps/app_tf_card/app_tf_card.cpp"
    "./apps/app_wifi_scan/app_wifi_scan.cpp"
)

set(APP_INCLUDE_DIRS
    "."
    "./hal"
    "./apps"
)

idf_component_register(SRCS "main.cpp" ${APP_SRCS}
                    PRIV_REQUIRES esp_http_server json lwip esp_timer M5GFX M5Unified mooncake LGFX_Fonts esp_wifi esp_event esp_netif nvs_flash fatfs sdmmc esp_driver_sdspi esp_driver_spi
                    INCLUDE_DIRS ${APP_INCLUDE_DIRS}
                    EMBED_FILES
                        "assets/img_bg.png"
                        "assets/img_logo.png"
                        "assets/wifi_setup.html")
