cmake_minimum_required(VERSION 3.16)

set(COMPONENTS main)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(dqr222_ESP_project)

spiffs_create_partition_image(spiffs init_spiff_data FLASH_IN_PROJECT)
