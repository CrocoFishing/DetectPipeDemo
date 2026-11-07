idf_component_register(SRCS "app_main.cpp" "sd_export_integration.cpp" INCLUDE_DIRS "."
  REQUIRES event_manager audio_pipeline board_support system_controller wifi_sd_export)
