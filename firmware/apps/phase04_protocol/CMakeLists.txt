cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32s3 CACHE STRING "Target" FORCE)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components/application_protocol"
)
if(DEFINED EVENTPIPE_OFFLINE_COMPONENTS)
    list(APPEND EXTRA_COMPONENT_DIRS "${EVENTPIPE_OFFLINE_COMPONENTS}")
endif()
set(COMPONENTS main application_protocol)
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/../../sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(phase04_protocol)
