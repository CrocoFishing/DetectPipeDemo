idf_component_register(
    SRCS "audio_pipeline.c"
    INCLUDE_DIRS "include"
    REQUIRES audio_common audio_capture audio_dsp audio_metrics vad ring_buffer recorder
             esp_timer freertos heap esp_system
)
