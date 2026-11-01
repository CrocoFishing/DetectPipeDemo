idf_component_register(
    SRCS "audio_dsp.c" "fixed_gain.c" "esp_sr_afe.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "private_include"
    REQUIRES audio_common esp-sr esp_timer freertos heap
)
