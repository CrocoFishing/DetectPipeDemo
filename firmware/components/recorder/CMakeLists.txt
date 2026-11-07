idf_component_register(
    SRCS "wav_writer.c" "sd_recorder.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES audio_common board_support fatfs sdmmc esp_driver_sdspi esp_driver_spi esp_timer freertos
)
