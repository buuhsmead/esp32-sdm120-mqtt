# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(EXCLUDE_COMPONENTS freemodbus)

IF (DEFINED ENV{IDF_PATH})
    MESSAGE(STATUS "*** FROM CLI ***")
ELSE ()
    MESSAGE(STATUS "*** INSIDE IDE ***")
    SET(ENV{IDF_PATH}  "/Users/hdaems/esp/esp-idf")
ENDIF ()


include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# "Trim" the build. Include the minimal set of components, main, and anything it depends on.
idf_build_set_property(MINIMAL_BUILD ON)
project(sdm120-mqtt)



