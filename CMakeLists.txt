cmake_minimum_required(VERSION 3.16)
project(hscrcpy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER "D:/Program Files/cygwin64/bin/x86_64-w64-mingw32-gcc.exe")
set(CMAKE_CXX_COMPILER "D:/Program Files/cygwin64/bin/x86_64-w64-mingw32-g++.exe")
set(CMAKE_RC_COMPILER "D:/Program Files/cygwin64/bin/x86_64-w64-mingw32-windres.exe")

set(CMAKE_CXX_COMPILER_LAUNCHER "")
set(CMAKE_C_COMPILER_LAUNCHER "")

add_executable(hscrcpy WIN32
    src/main.cpp
    src/hdc_wrapper.cpp
    src/screen_capture.cpp
    src/input_injector.cpp
    src/h264_decoder.cpp
    src/jpeg_decoder.cpp
    src/grpc_client.cpp
    src/window_renderer.cpp
    src/cli.cpp
)

set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/ffmpeg-master-latest-win64-gpl-shared")

target_include_directories(hscrcpy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${FFMPEG_ROOT}/include
)

target_link_libraries(hscrcpy
    ${FFMPEG_ROOT}/lib/libavcodec.dll.a
    ${FFMPEG_ROOT}/lib/libavutil.dll.a
    ${FFMPEG_ROOT}/lib/libswscale.dll.a
    gdi32
    gdiplus
    ws2_32
    shell32
    user32
    kernel32
    ole32
)

target_compile_definitions(hscrcpy PRIVATE
    _CRT_SECURE_NO_WARNINGS
)

target_link_options(hscrcpy PRIVATE -static -static-libgcc -static-libstdc++)

install(TARGETS hscrcpy DESTINATION bin)
