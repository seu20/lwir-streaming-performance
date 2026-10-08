cmake_minimum_required(VERSION 3.22)

project(video_pipeline LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# ============================================================
# Dependencies
# ============================================================

find_package(Threads REQUIRED)
find_package(OpenCV 4.5 REQUIRED COMPONENTS core)

find_package(PkgConfig REQUIRED)

pkg_check_modules(
    GSTREAMER
    REQUIRED
    IMPORTED_TARGET
    gstreamer-1.0>=1.20
    gstreamer-app-1.0>=1.20
)


# ============================================================
# i3system TE-EV1 SDK
# ============================================================
#
# CMake 실행 시 실제 경로를 지정:
#
# cmake .. \
#   -DI3SYSTEM_INCLUDE_DIR=/path/to/i3/include \
#   -DI3SYSTEM_LIBRARY=/path/to/i3/lib/libXXX.so
#

set(
    I3SYSTEM_INCLUDE_DIR
    ""
    CACHE PATH
    "Directory containing i3system_TE.h"
)

set(
    I3SYSTEM_LIBRARY
    ""
    CACHE FILEPATH
    "i3system SDK shared library"
)

if(NOT EXISTS "${I3SYSTEM_INCLUDE_DIR}/i3system_TE.h")
    message(FATAL_ERROR
        "i3system_TE.h not found. "
        "Set -DI3SYSTEM_INCLUDE_DIR=<include path>"
    )
endif()

if(NOT EXISTS "${I3SYSTEM_LIBRARY}")
    message(FATAL_ERROR
        "i3system SDK library not found. "
        "Set -DI3SYSTEM_LIBRARY=<shared library path>"
    )
endif()


# ============================================================
# Sources
# ============================================================

set(SOURCES
    src/main.cpp

    src/app/application.cpp
    src/app/capture.cpp
    src/app/preprocess.cpp
    src/app/streaming.cpp

    src/pipeline/capture_thread.cpp
    src/pipeline/preprocess_thread.cpp
    src/pipeline/streaming_thread.cpp

    src/common/logger.cpp
    src/common/metrics_report.cpp
)


# ============================================================
# Executable
# ============================================================

add_executable(
    video_pipeline
    ${SOURCES}
)


# ============================================================
# Include directories
# ============================================================

target_include_directories(
    video_pipeline
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/inc
    ${I3SYSTEM_INCLUDE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)


# ============================================================  
# Link libraries
# ============================================================

target_link_libraries(
    video_pipeline
    PRIVATE
    Threads::Threads
    ${OpenCV_LIBS}
    PkgConfig::GSTREAMER
    ${I3SYSTEM_LIBRARY}
)


# ============================================================
# Runtime path for i3system .so
# ============================================================

get_filename_component(
    I3SYSTEM_LIBRARY_DIR
    "${I3SYSTEM_LIBRARY}"
    DIRECTORY
)

set_target_properties(
    video_pipeline
    PROPERTIES
    BUILD_RPATH "${I3SYSTEM_LIBRARY_DIR}"
)