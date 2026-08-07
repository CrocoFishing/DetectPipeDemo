idf_component_register(SRCS "ble_transport.cpp" INCLUDE_DIRS "include"
  REQUIRES bt nvs_flash application_protocol board_support metrics esp_timer freertos)

