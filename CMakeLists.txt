cmake_minimum_required(VERSION 3.27)

project(MetaVoice)

if(NOT DEFINED METAHOOKSV_DIR)
    get_filename_component(_raspad_metahook "${PROJECT_SOURCE_DIR}/../metahook" ABSOLUTE)
    if(EXISTS "${_raspad_metahook}/include/metahook.h")
        set(METAHOOKSV_DIR "${_raspad_metahook}")
    else()
        message(FATAL_ERROR "METAHOOKSV_DIR not set and ../metahook not found")
    endif()
endif()

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(MetaVoice SHARED
    src/metavoce.cpp
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

target_link_libraries(MetaVoice PRIVATE winmm)

if (MSVC)
    target_compile_options(MetaVoice PRIVATE /utf-8)
endif()
