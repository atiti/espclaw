cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED ENV{IDF_PATH})
  message(FATAL_ERROR "IDF_PATH is not set. Install ESP-IDF and export IDF_PATH before building firmware.")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(espclaw_firmware)
