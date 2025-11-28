# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)


include(FetchContent)
FetchContent_Declare(
  espidflib
  GIT_REPOSITORY https://github.com/UncleRus/esp-idf-lib.git
)

FetchContent_MakeAvailable(espidflib)

set(EXTRA_COMPONENT_DIRS 
    ${EXTRA_COMPONENT_DIRS}
    ${espidflib_SOURCE_DIR}/components/i2cdev
    ${espidflib_SOURCE_DIR}/components/esp_idf_lib_helpers
    ${espidflib_SOURCE_DIR}/components/button
    )

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(hamster)
