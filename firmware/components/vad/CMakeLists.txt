idf_component_register(
    SRCS "vad_service.c"
    INCLUDE_DIRS "include"
    REQUIRES audio_common audio_dsp
)
