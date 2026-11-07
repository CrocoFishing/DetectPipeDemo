idf_component_register(
    SRCS "audio_ring_buffer.c"
    INCLUDE_DIRS "include"
    REQUIRES audio_common freertos heap
)
