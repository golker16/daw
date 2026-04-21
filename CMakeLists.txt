cmake_minimum_required(VERSION 3.20)

project(DAWCloudTemplate VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(DAWCloudTemplate WIN32
    main.cpp
    AudioEngine.cpp
    UI.cpp
    AudioEngine.h
    UI.h
)

target_compile_definitions(DAWCloudTemplate PRIVATE UNICODE _UNICODE)

if (MSVC)
    target_compile_options(DAWCloudTemplate PRIVATE /W4 /EHsc)
else()
    target_compile_options(DAWCloudTemplate PRIVATE -Wall -Wextra -pedantic)
endif()
