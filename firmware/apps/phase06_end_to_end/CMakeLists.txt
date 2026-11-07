cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32s3 CACHE STRING "Target" FORCE)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/board_support"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/pipeline_core"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/trigger_service"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/camera_service"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/face_detection"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/image_processing"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/ble_transport"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/application_protocol"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/metrics"
    "${CMAKE_CURRENT_LIST_DIR}/../../components/system_controller"
)
if(DEFINED EVENTPIPE_OFFLINE_COMPONENTS)
    list(APPEND EXTRA_COMPONENT_DIRS "${EVENTPIPE_OFFLINE_COMPONENTS}")
endif()
set(COMPONENTS main system_controller)
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../../sdkconfig.defaults;${CMAKE_CURRENT_LIST_DIR}/../../sdkconfig.ble.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(phase06_end_to_end)
