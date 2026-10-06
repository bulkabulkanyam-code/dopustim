cmake_minimum_required(VERSION 3.25)
project(frame_window_mod VERSION 0.1.0 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Standalone tests of the Geode-independent core (no Geode needed):
#   cmake -S . -B build-tests -DFWM_TESTS_ONLY=ON && cmake --build build-tests && ctest --test-dir build-tests
option(FWM_TESTS_ONLY "Only build the core unit tests" OFF)

set(FWM_CORE_SOURCES src/core/Analyzer.cpp src/core/Precision.cpp src/core/Export.cpp)

if(FWM_TESTS_ONLY)
    enable_testing()
    add_executable(fwm_tests tests/test_core.cpp ${FWM_CORE_SOURCES})
    target_include_directories(fwm_tests PRIVATE src)
    add_test(NAME core COMMAND fwm_tests)
    return()
endif()

# ---------------------------------------------------------------------------
# Geode mod
# ---------------------------------------------------------------------------
set(ARC_FEATURE_FULL ON CACHE BOOL "" FORCE)  # async runtime used by the desktop file picker

if(NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Define the GEODE_SDK environment variable.")
endif()
add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# CPM (dependency manager)
set(CPM_DOWNLOAD_VERSION 0.40.2)
if(NOT EXISTS "${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")
    file(DOWNLOAD
        https://github.com/cpm-cmake/CPM.cmake/releases/download/v${CPM_DOWNLOAD_VERSION}/CPM.cmake
        "${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake"
        STATUS DOWNLOAD_STATUS)
    list(GET DOWNLOAD_STATUS 0 STATUS_CODE)
    if(NOT STATUS_CODE EQUAL 0)
        message(FATAL_ERROR "Failed to download CPM.cmake: ${DOWNLOAD_STATUS}")
    endif()
endif()
include("${CMAKE_CURRENT_BINARY_DIR}/cmake/CPM_${CPM_DOWNLOAD_VERSION}.cmake")

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp" "src/*.hpp")
add_library(${PROJECT_NAME} SHARED ${SOURCES})
target_include_directories(${PROJECT_NAME} PRIVATE src)
setup_geode_mod(${PROJECT_NAME})

# Replay formats: GDR2 (libGDR) and GDR1 (converter)
CPMAddPackage("gh:maxnut/GDReplayFormat#2a0ca3c")
CPMAddPackage(
    NAME msgpack-for-matjson
    GITHUB_REPOSITORY prevter/msgpack-for-matjson
    GIT_TAG main)
CPMAddPackage(
    NAME GDRconverter
    GITHUB_REPOSITORY maxnut/GDR-converter
    GIT_TAG main
    EXCLUDE_FROM_ALL YES
    OPTIONS "BUILD_TESTING OFF")

target_link_libraries(${PROJECT_NAME} libGDR GDRconverter)
