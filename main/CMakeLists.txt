idf_component_register(
    SRCS
        "main.c"
        "audio_player.c"
        "voice_recorder.c"
        "agent_mode.c"
        "welcome_display.c"
        "wifi_setup.c"
        "wifi_credentials.c"
        "wifi_ui.c"
        "wifi_ui_view.c"
    INCLUDE_DIRS
        "."
        "include"
    REQUIRES
        esp_driver_i2s
        spiffs
        nvs_flash
        freertos
        esp_psram
        minimp3
        esp_lcd
        esp_driver_gpio
        esp_driver_i2c
        esp_wifi
        esp_event
        esp_netif
        esp_timer
        lwip
        esp_http_client
        tcp_transport
        mbedtls
        json
    EMBED_FILES "assets/welcome.rgb565" "assets/ui_font.bin" "assets/wifi_background.rgb565"
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
