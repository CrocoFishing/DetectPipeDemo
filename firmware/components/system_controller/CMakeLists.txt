idf_component_register(SRCS "system_controller.cpp" INCLUDE_DIRS "include"
  REQUIRES pipeline_core board_support trigger_service camera_service face_detection image_processing ble_transport application_protocol metrics esp_timer esp_event)
