cmake_minimum_required(VERSION 3.27)

project(MetaVoice)

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_STATIC_RUNTIME ON CACHE BOOL "" FORCE)
FetchContent_Declare(opus
    GIT_REPOSITORY https://github.com/xiph/opus.git
    GIT_TAG v1.5.2
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(opus)

if(NOT DEFINED METAHOOKSV_DIR)
    get_filename_component(_raspad_metahook "${PROJECT_SOURCE_DIR}/../metahook" ABSOLUTE)
    if(EXISTS "${_raspad_metahook}/include/metahook.h")
        set(METAHOOKSV_DIR "${_raspad_metahook}")
    else()
        message(FATAL_ERROR "METAHOOKSV_DIR not set and ../metahook not found")
    endif()
endif()

add_library(MetaVoice SHARED
    src/metavoce.cpp
    src/steam_voice.cpp
    ${METAHOOKSV_DIR}/include/HLSDK/common/interface.cpp
)

set_property(TARGET MetaVoice PROPERTY CXX_STANDARD 17)

target_include_directories(MetaVoice
    PRIVATE
        ${METAHOOKSV_DIR}/include
        ${METAHOOKSV_DIR}/include/Interface
        ${METAHOOKSV_DIR}/include/HLSDK/common
        ${METAHOOKSV_DIR}/include/HLSDK/cl_dll
        ${METAHOOKSV_DIR}/include/HLSDK/engine
        ${METAHOOKSV_DIR}/include/HLSDK/public
        ${METAHOOKSV_DIR}/include/HLSDK/pm_shared
)

target_link_libraries(MetaVoice PRIVATE winmm opus)
target_include_directories(MetaVoice PRIVATE src)

if (MSVC)
    target_compile_options(MetaVoice PRIVATE /utf-8)
endif()
