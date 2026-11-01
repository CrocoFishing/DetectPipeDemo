idf_component_register(SRCS "wifi_sd_export_service.cpp" INCLUDE_DIRS "include"
  REQUIRES application_protocol esp_wifi esp_netif esp_event esp_http_server
           esp_timer esp_system nvs_flash mbedtls fatfs freertos)
