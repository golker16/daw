cmake_minimum_required(VERSION 3.20)

project(DAWCloudTemplate VERSION 1.0 LANGUAGES CXX)

# =========================================================
# Global build configuration
# =========================================================

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(APP_TARGET_NAME DAWCloudTemplate)
set(APP_OUTPUT_NAME DAWCloudTemplate)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# =========================================================
# Options
# =========================================================

option(DAW_ENABLE_WASAPI "Enable WASAPI backend support" ON)
option(DAW_ENABLE_GRAPH "Enable compiled audio graph subsystem" ON)
option(DAW_ENABLE_AUTOMATION "Enable sample-accurate automation subsystem" ON)
option(DAW_ENABLE_PDC "Enable plugin delay compensation subsystem" ON)
option(DAW_ENABLE_OFFLINE_RENDER "Enable offline render subsystem" ON)
option(DAW_ENABLE_PLUGIN_HOST "Enable plugin host subsystem" OFF)
option(DAW_ENABLE_RESOURCES "Enable Windows resources when available" OFF)
option(DAW_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

# =========================================================
# Source files
# =========================================================

set(APP_SOURCES
    main.cpp
    AudioEngine.cpp
    UI.cpp
)

set(APP_HEADERS
    AudioEngine.h
    UI.h
)

set(APP_RESOURCE_FILES "")

if(DAW_ENABLE_RESOURCES)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/app.rc")
        list(APPEND APP_RESOURCE_FILES app.rc)
        message(STATUS "Windows resources enabled: app.rc found")
    else()
        message(WARNING "DAW_ENABLE_RESOURCES is ON but app.rc was not found. Continuing without resources.")
    endif()
endif()

add_executable(${APP_TARGET_NAME} WIN32
    ${APP_SOURCES}
    ${APP_HEADERS}
    ${APP_RESOURCE_FILES}
)

# =========================================================
# Output layout
# =========================================================

set_target_properties(${APP_TARGET_NAME} PROPERTIES
    OUTPUT_NAME "${APP_OUTPUT_NAME}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin/RelWithDebInfo"
    RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin/MinSizeRel"
)

# =========================================================
# Include paths
# =========================================================

target_include_directories(${APP_TARGET_NAME}
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# =========================================================
# Compile definitions
# =========================================================

target_compile_definitions(${APP_TARGET_NAME}
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX

        $<$<BOOL:${DAW_ENABLE_WASAPI}>:DAW_ENABLE_WASAPI=1>
        $<$<NOT:$<BOOL:${DAW_ENABLE_WASAPI}>>:DAW_ENABLE_WASAPI=0>

        $<$<BOOL:${DAW_ENABLE_GRAPH}>:DAW_ENABLE_GRAPH=1>
        $<$<NOT:$<BOOL:${DAW_ENABLE_GRAPH}>>:DAW_ENABLE_GRAPH=0>

        $<$<BOOL:${DAW_ENABLE_AUTOMATION}>:DAW_ENABLE_AUTOMATION=1>
        $<$<NOT:$<BOOL:${DAW_ENABLE_AUTOMATION}>>:DAW_ENABLE_AUTOMATION=0>

        $<$<BOOL:${DAW_ENABLE_PDC}>:DAW_ENABLE_PDC=1>
        $<$<NOT:$<BOOL:${DAW_ENABLE_PDC}>>:DAW_ENABLE_PDC=0>

        $<$<BOOL:${DAW_ENABLE_OFFLINE_RENDER}>:DAW_ENABLE_OFFLINE_RENDER=1>
        $<$<NOT:$<BOOL:${DAW_ENABLE_OFFLINE_RENDER}>>:DAW_ENABLE_OFFLINE_RENDER=0>

        $<$<BOOL:${DAW_ENABLE_PLUGIN_HOST}>:DAW_ENABLE_PLUGIN_HOST=1>
        $<$<NOT:$<BOOL:${DAW_ENABLE_PLUGIN_HOST}>>:DAW_ENABLE_PLUGIN_HOST=0>

        $<$<BOOL:${DAW_ENABLE_RESOURCES}>:DAW_ENABLE_RESOURCES=1>
        $<$<NOT:$<BOOL:${DAW_ENABLE_RESOURCES}>>:DAW_ENABLE_RESOURCES=0>

        $<$<CONFIG:Debug>:DAW_DEBUG_BUILD=1>
        $<$<CONFIG:Release>:DAW_RELEASE_BUILD=1>
        $<$<CONFIG:RelWithDebInfo>:DAW_RELWITHDEBINFO_BUILD=1>
        $<$<CONFIG:MinSizeRel>:DAW_MINSIZEREL_BUILD=1>
)

# =========================================================
# Compiler warnings and build behavior
# =========================================================

if(MSVC)
    target_compile_options(${APP_TARGET_NAME}
        PRIVATE
            /W4
            /EHsc
            /permissive-
            /Zc:preprocessor
            /Zc:__cplusplus
            /utf-8

            $<$<BOOL:${DAW_WARNINGS_AS_ERRORS}>:/WX>

            $<$<CONFIG:Debug>:/Od>
            $<$<CONFIG:Debug>:/Zi>

            $<$<CONFIG:Release>:/O2>
            $<$<CONFIG:Release>:/Ob2>
            $<$<CONFIG:Release>:/DNDEBUG>

            $<$<CONFIG:RelWithDebInfo>:/O2>
            $<$<CONFIG:RelWithDebInfo>:/Zi>
            $<$<CONFIG:RelWithDebInfo>:/DNDEBUG>

            $<$<CONFIG:MinSizeRel>:/O1>
            $<$<CONFIG:MinSizeRel>:/DNDEBUG>
    )

    target_link_options(${APP_TARGET_NAME}
        PRIVATE
            $<$<CONFIG:Debug>:/DEBUG:FULL>
            $<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>
    )
else()
    target_compile_options(${APP_TARGET_NAME}
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wshadow
            -Wconversion
            -Wsign-conversion

            $<$<BOOL:${DAW_WARNINGS_AS_ERRORS}>:-Werror>

            $<$<CONFIG:Debug>:-O0>
            $<$<CONFIG:Debug>:-g>

            $<$<CONFIG:Release>:-O3>
            $<$<CONFIG:Release>:-DNDEBUG>

            $<$<CONFIG:RelWithDebInfo>:-O2>
            $<$<CONFIG:RelWithDebInfo>:-g>
            $<$<CONFIG:RelWithDebInfo>:-DNDEBUG>

            $<$<CONFIG:MinSizeRel>:-Os>
            $<$<CONFIG:MinSizeRel>:-DNDEBUG>
    )
endif()

# =========================================================
# Runtime/system libraries for Windows audio-oriented app
# =========================================================

if(WIN32)
    target_link_libraries(${APP_TARGET_NAME}
        PRIVATE
            winmm
            ole32
            uuid
            avrt
    )
endif()

# =========================================================
# Status summary
# =========================================================

message(STATUS "========================================")
message(STATUS "Configuring ${APP_TARGET_NAME}")
message(STATUS "C++ standard             : ${CMAKE_CXX_STANDARD}")
message(STATUS "Build type               : ${CMAKE_BUILD_TYPE}")
message(STATUS "WASAPI enabled           : ${DAW_ENABLE_WASAPI}")
message(STATUS "Graph enabled            : ${DAW_ENABLE_GRAPH}")
message(STATUS "Automation enabled       : ${DAW_ENABLE_AUTOMATION}")
message(STATUS "PDC enabled              : ${DAW_ENABLE_PDC}")
message(STATUS "Offline render enabled   : ${DAW_ENABLE_OFFLINE_RENDER}")
message(STATUS "Plugin host enabled      : ${DAW_ENABLE_PLUGIN_HOST}")
message(STATUS "Resources enabled        : ${DAW_ENABLE_RESOURCES}")
message(STATUS "Warnings as errors       : ${DAW_WARNINGS_AS_ERRORS}")
message(STATUS "Output directory         : ${CMAKE_BINARY_DIR}/bin")
message(STATUS "========================================")
