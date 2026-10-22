cmake_minimum_required(VERSION 3.16)
project(hscrcpy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -msse2 -DUNICODE -D_UNICODE")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2 -msse2")

add_executable(hscrcpy WIN32
    src/main.cpp
    src/hdc_wrapper.cpp
    src/screen_capture.cpp
    src/input_injector.cpp
    src/h264_decoder.cpp
    src/jpeg_decoder.cpp
    src/grpc_client.cpp
    src/websocket_client.cpp
    src/window_renderer.cpp
    src/cli.cpp
    src/config.cpp
    src/layout_window.cpp
    src/file_manager_window.cpp
    src/cph_window.cpp
    src/msgbox_util.cpp
)

# ============================================
# FFmpeg
# Priority: FFMPEG_DIR env var > bundled ffmpeg/
# ============================================
if(DEFINED ENV{FFMPEG_DIR})
    set(FFMPEG_ROOT "$ENV{FFMPEG_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/ffmpeg-master-latest-win64-gpl-shared/include")
    set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/ffmpeg-master-latest-win64-gpl-shared")
else()
    message(FATAL_ERROR "FFmpeg not found. Set FFMPEG_DIR or download to ffmpeg/ (run download_ffmpeg.ps1)")
endif()

message(STATUS "FFmpeg root: ${FFMPEG_ROOT}")

target_include_directories(hscrcpy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${FFMPEG_ROOT}/include
)

# Prefer .dll.a import libs (avoids MinGW direct-DLL auto-import issues,
# e.g. av_base64_encode not resolved from the export table).
set(FFMPEG_BIN "${FFMPEG_ROOT}/bin")
set(FFMPEG_LIB "${FFMPEG_ROOT}/lib")

find_library(AVCODEC_LIB avcodec.dll.a PATHS "${FFMPEG_LIB}" NO_DEFAULT_PATH)
find_library(AVUTIL_LIB  avutil.dll.a  PATHS "${FFMPEG_LIB}" NO_DEFAULT_PATH)
find_library(SWSCALE_LIB swscale.dll.a PATHS "${FFMPEG_LIB}" NO_DEFAULT_PATH)

if(NOT AVCODEC_LIB OR NOT AVUTIL_LIB OR NOT SWSCALE_LIB)
    # Fallback: link .dll files directly
    file(GLOB AVCODEC_DLL "${FFMPEG_BIN}/avcodec-*.dll")
    file(GLOB AVUTIL_DLL  "${FFMPEG_BIN}/avutil-*.dll")
    file(GLOB SWSCALE_DLL "${FFMPEG_BIN}/swscale-*.dll")
    if(AVCODEC_DLL AND AVUTIL_DLL AND SWSCALE_DLL)
        list(GET AVCODEC_DLL 0 AVCODEC_LIB)
        list(GET AVUTIL_DLL 0 AVUTIL_LIB)
        list(GET SWSCALE_DLL 0 SWSCALE_LIB)
        message(STATUS "FFmpeg: linking via DLLs from ${FFMPEG_BIN}")
    endif()
endif()

if(NOT AVCODEC_LIB OR NOT AVUTIL_LIB OR NOT SWSCALE_LIB)
    message(FATAL_ERROR "FFmpeg libraries not found in ${FFMPEG_ROOT}/bin or ${FFMPEG_ROOT}/lib")
endif()

target_link_libraries(hscrcpy "${AVCODEC_LIB}" "${AVUTIL_LIB}" "${SWSCALE_LIB}")

# swresample is optional
file(GLOB SWRESAMPLE_DLL "${FFMPEG_BIN}/swresample-*.dll")
if(SWRESAMPLE_DLL)
    list(GET SWRESAMPLE_DLL 0 SWRESAMPLE_LIB)
    target_link_libraries(hscrcpy "${SWRESAMPLE_LIB}")
else()
    find_library(SWRESAMPLE_LIB swresample.dll.a PATHS "${FFMPEG_LIB}" NO_DEFAULT_PATH)
    if(SWRESAMPLE_LIB)
        target_link_libraries(hscrcpy "${SWRESAMPLE_LIB}")
    endif()
endif()

# ============================================
# nghttp2 (HTTP/2 framing for gRPC client)
# ============================================
set(NGHTTP2_ROOT "$ENV{MSYS2_ROOT}" CACHE PATH "MSYS2 root")
if(NOT NGHTTP2_ROOT)
    foreach(p "C:/msys64" "D:/msys64" "C:/tools/msys64" "D:/tools/msys64")
        if(EXISTS "${p}/ucrt64/lib/libnghttp2.a")
            set(NGHTTP2_ROOT "${p}")
            break()
        endif()
    endforeach()
endif()

if(NGHTTP2_ROOT AND EXISTS "${NGHTTP2_ROOT}/ucrt64/lib/libnghttp2.a")
    set(NGHTTP2_INCLUDE "${NGHTTP2_ROOT}/ucrt64/include")
    set(NGHTTP2_LIB "${NGHTTP2_ROOT}/ucrt64/lib/libnghttp2.a")
    message(STATUS "nghttp2: ${NGHTTP2_LIB}")
else()
    # Fallback: try pkg-config
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(NGHTTP2 nghttp2)
    endif()
    if(NOT NGHTTP2_FOUND)
        message(FATAL_ERROR "nghttp2 not found. Install: pacman -S mingw-w64-ucrt-x86_64-nghttp2")
    endif()
endif()

target_include_directories(hscrcpy PRIVATE ${NGHTTP2_INCLUDE})
target_compile_definitions(hscrcpy PRIVATE NGHTTP2_STATICLIB)
target_link_libraries(hscrcpy "${NGHTTP2_LIB}")

message(STATUS "FFmpeg: OK (${AVCODEC_LIB})")

# ============================================
# Windows libraries
# ============================================
target_link_libraries(hscrcpy
    gdi32 gdiplus ws2_32 shell32 user32 kernel32 ole32 d2d1 comctl32 uxtheme
)

target_compile_definitions(hscrcpy PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(hscrcpy PRIVATE -flto)
target_link_options(hscrcpy PRIVATE -static -static-libgcc -static-libstdc++ -flto)

install(TARGETS hscrcpy DESTINATION bin)