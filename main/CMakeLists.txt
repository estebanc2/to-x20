idf_component_register(
    SRCS
        "main.c"
        "audio_capture.c"
        "bt_audio.c"
    INCLUDE_DIRS "."
    REQUIRES
        bt
        driver
        esp_adc
        esp_timer 
        freertos
        log
        nvs_flash
        esp_ringbuf
)
