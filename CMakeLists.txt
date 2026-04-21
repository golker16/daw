cmake_minimum_required(VERSION 3.20)

project(DAWCloudTemplate VERSION 1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(APP_TARGET_NAME DAWCloudTemplate)
set(APP_OUTPUT_NAME DAWCloudTemplate)
set(PLUGIN_HOST_TARGET_NAME DAWCloudPluginHost)
set(PLUGIN_HOST_OUTPUT_NAME DAWCloudPluginHost)

option(DAW_ENABLE_WASAPI "Enable WASAPI backend support" ON)
option(DAW_ENABLE_GRAPH "Enable compiled audio graph subsystem" ON)
option(DAW_ENABLE_AUTOMATION "Enable sample-accurate automation subsystem" ON)
option(DAW_ENABLE_PDC "Enable plugin delay compensation subsystem" ON)
option(DAW_ENABLE_OFFLINE_RENDER "Enable offline render subsystem" ON)
option(DAW_ENABLE_PLUGIN_HOST "Enable plugin host subsystem" ON)
option(DAW_ENABLE_SANDBOX "Enable process sandbox/watchdog support" ON)
option(DAW_ENABLE_DISK_STREAMING "Enable disk streaming/cache support" ON)
option(DAW_ENABLE_MULTICORE "Enable anticipative helper threads" ON)
option(DAW_ENABLE_DOUBLE_PRECISION_MIX "Prefer double-precision internal mix" ON)
option(DAW_BUILD_PLUGIN_HOST_TARGET "Build the standalone plugin host executable" ON)
option(DAW_ENABLE_RESOURCES "Enable Windows resources when available" OFF)
option(DAW_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

set(APP_SOURCES
    PMain.cpp
    PAudioEngineIndex.cpp
    PUIIndex.cpp
)

set(APP_HEADERS
    PProjectIndex.h
    PAudioEngine.h
    PAudioEngineIndex.h
    PUI.h
    PUIIndex.h
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

function(configure_daw_target TARGET_NAME OUTPUT_NAME)
    add_executable(${TARGET_NAME} WIN32
        ${APP_SOURCES}
        ${APP_HEADERS}
        ${APP_RESOURCE_FILES}
    )

    set_target_properties(${TARGET_NAME} PROPERTIES
        OUTPUT_NAME "${OUTPUT_NAME}"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/bin/Debug"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/bin/Release"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/bin/RelWithDebInfo"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/bin/MinSizeRel"
    )

    target_include_directories(${TARGET_NAME}
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_compile_definitions(${TARGET_NAME}
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

            $<$<BOOL:${DAW_ENABLE_SANDBOX}>:DAW_ENABLE_SANDBOX=1>
            $<$<NOT:$<BOOL:${DAW_ENABLE_SANDBOX}>>:DAW_ENABLE_SANDBOX=0>

            $<$<BOOL:${DAW_ENABLE_DISK_STREAMING}>:DAW_ENABLE_DISK_STREAMING=1>
            $<$<NOT:$<BOOL:${DAW_ENABLE_DISK_STREAMING}>>:DAW_ENABLE_DISK_STREAMING=0>

            $<$<BOOL:${DAW_ENABLE_MULTICORE}>:DAW_ENABLE_MULTICORE=1>
            $<$<NOT:$<BOOL:${DAW_ENABLE_MULTICORE}>>:DAW_ENABLE_MULTICORE=0>

            $<$<BOOL:${DAW_ENABLE_DOUBLE_PRECISION_MIX}>:DAW_ENABLE_DOUBLE_PRECISION_MIX=1>
            $<$<NOT:$<BOOL:${DAW_ENABLE_DOUBLE_PRECISION_MIX}>>:DAW_ENABLE_DOUBLE_PRECISION_MIX=0>

            $<$<BOOL:${DAW_ENABLE_RESOURCES}>:DAW_ENABLE_RESOURCES=1>
            $<$<NOT:$<BOOL:${DAW_ENABLE_RESOURCES}>>:DAW_ENABLE_RESOURCES=0>
    )

    if(MSVC)
        target_compile_options(${TARGET_NAME}
            PRIVATE
                /W4
                /EHsc
                /permissive-
                /Zc:preprocessor
                /Zc:__cplusplus
                /utf-8
                $<$<BOOL:${DAW_WARNINGS_AS_ERRORS}>:/WX>
        )

        target_link_options(${TARGET_NAME}
            PRIVATE
                $<$<CONFIG:Debug>:/DEBUG:FULL>
                $<$<CONFIG:RelWithDebInfo>:/DEBUG:FULL>
        )
    else()
        target_compile_options(${TARGET_NAME}
            PRIVATE
                -Wall
                -Wextra
                -Wpedantic
                -Wshadow
                -Wconversion
                -Wsign-conversion
                $<$<BOOL:${DAW_WARNINGS_AS_ERRORS}>:-Werror>
        )
    endif()

    if(WIN32)
        target_link_libraries(${TARGET_NAME}
            PRIVATE
                user32
                gdi32
                winmm
                ole32
                uuid
                avrt
                shell32
        )
    endif()
endfunction()

configure_daw_target(${APP_TARGET_NAME} ${APP_OUTPUT_NAME})

if(DAW_BUILD_PLUGIN_HOST_TARGET)
    configure_daw_target(${PLUGIN_HOST_TARGET_NAME} ${PLUGIN_HOST_OUTPUT_NAME})
    target_compile_definitions(${PLUGIN_HOST_TARGET_NAME} PRIVATE DAW_PLUGIN_HOST_TARGET=1)
endif()

message(STATUS "========================================")
message(STATUS "Configuring ${APP_TARGET_NAME}")
message(STATUS "C++ standard                 : ${CMAKE_CXX_STANDARD}")
message(STATUS "Build type                   : ${CMAKE_BUILD_TYPE}")
message(STATUS "WASAPI enabled               : ${DAW_ENABLE_WASAPI}")
message(STATUS "Graph enabled                : ${DAW_ENABLE_GRAPH}")
message(STATUS "Automation enabled           : ${DAW_ENABLE_AUTOMATION}")
message(STATUS "PDC enabled                  : ${DAW_ENABLE_PDC}")
message(STATUS "Offline render enabled       : ${DAW_ENABLE_OFFLINE_RENDER}")
message(STATUS "Plugin host enabled          : ${DAW_ENABLE_PLUGIN_HOST}")
message(STATUS "Sandbox enabled              : ${DAW_ENABLE_SANDBOX}")
message(STATUS "Disk streaming enabled       : ${DAW_ENABLE_DISK_STREAMING}")
message(STATUS "Multicore helpers enabled    : ${DAW_ENABLE_MULTICORE}")
message(STATUS "Double precision mix enabled : ${DAW_ENABLE_DOUBLE_PRECISION_MIX}")
message(STATUS "Plugin host target           : ${DAW_BUILD_PLUGIN_HOST_TARGET}")
message(STATUS "Warnings as errors           : ${DAW_WARNINGS_AS_ERRORS}")
message(STATUS "Output directory             : ${CMAKE_BINARY_DIR}/bin")
message(STATUS "========================================")

