cmake_minimum_required(VERSION 3.22)

project(lwir_receiver LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV 4.5 REQUIRED COMPONENTS core highgui videoio)

find_package(PkgConfig REQUIRED)

pkg_check_modules(
    GSTREAMER
    REQUIRED
    IMPORTED_TARGET
    gstreamer-1.0>=1.20
    gstreamer-app-1.0>=1.20
    gstreamer-rtp-1.0>=1.20
)

add_executable(
    lwir_receiver
    main.cpp
    receiver.cpp
)

target_include_directories(
    lwir_receiver
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(
    lwir_receiver
    PRIVATE
    ${OpenCV_LIBS}
    PkgConfig::GSTREAMER
)
