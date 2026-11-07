idf_component_register(
    SRCS "audio_capture.c"
    INCLUDE_DIRS "include"
    REQUIRES audio_common board_support esp_driver_i2s esp_timer freertos
)
