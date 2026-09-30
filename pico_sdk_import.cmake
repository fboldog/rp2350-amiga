# Locate an existing Pico SDK or download it into the CMake build directory.
# This file must be included before project().

if(DEFINED ENV{PICO_SDK_PATH} AND NOT PICO_SDK_PATH)
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}")
    message(STATUS "Using PICO_SDK_PATH from environment: ${PICO_SDK_PATH}")
endif()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH
    "Path to an existing Raspberry Pi Pico SDK")
set(PICO_SDK_GIT_TAG "2.3.1" CACHE STRING
    "Pico SDK Git tag downloaded when PICO_SDK_PATH is not set")

if(NOT PICO_SDK_PATH)
    include(FetchContent)

    message(STATUS
        "PICO_SDK_PATH is not set; downloading Pico SDK ${PICO_SDK_GIT_TAG}")
    FetchContent_Declare(
        pico_sdk
        GIT_REPOSITORY https://github.com/raspberrypi/pico-sdk.git
        GIT_TAG        ${PICO_SDK_GIT_TAG}
        GIT_SHALLOW    TRUE
        # Keep TinyUSB available for future USB HID/stdio builds without
        # cloning every optional SDK dependency.
        GIT_SUBMODULES lib/tinyusb
        GIT_SUBMODULES_RECURSE TRUE
        # Populate only; pico_sdk_init.cmake adds the SDK after project().
        SOURCE_SUBDIR  _fetch_only
    )
    FetchContent_MakeAvailable(pico_sdk)

    set(PICO_SDK_PATH "${pico_sdk_SOURCE_DIR}" CACHE PATH
        "Path to the Raspberry Pi Pico SDK" FORCE)
endif()

get_filename_component(PICO_SDK_PATH "${PICO_SDK_PATH}" REALPATH
    BASE_DIR "${CMAKE_BINARY_DIR}")

set(PICO_SDK_INIT "${PICO_SDK_PATH}/pico_sdk_init.cmake")
if(NOT EXISTS "${PICO_SDK_INIT}")
    message(FATAL_ERROR
        "${PICO_SDK_PATH} does not appear to contain the Pico SDK")
endif()

include("${PICO_SDK_INIT}")
