idf_component_register(SRCS "event_manager.cpp" INCLUDE_DIRS "include"
  REQUIRES pipeline_core system_controller audio_pipeline recorder nvs_flash esp_timer)
