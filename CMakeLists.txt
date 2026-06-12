cmake_minimum_required(VERSION 3.16)
project(hscrcpy CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O2 -msse2")
set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -O2 -msse2")

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

# Prefer .dll files directly (avoids dlltool .def issue)
# MinGW GCC can link directly against .dll files
# The package must have bin/*.dll with the actual shared libraries
set(FFMPEG_BIN "${FFMPEG_ROOT}/bin")
set(FFMPEG_LIB "${FFMPEG_ROOT}/lib")

# Try direct DLL linking first
file(GLOB AVCODEC_DLL "${FFMPEG_BIN}/avcodec-*.dll")
file(GLOB AVUTIL_DLL  "${FFMPEG_BIN}/avutil-*.dll")
file(GLOB SWSCALE_DLL "${FFMPEG_BIN}/swscale-*.dll")

if(AVCODEC_DLL AND AVUTIL_DLL AND SWSCALE_DLL)
    list(GET AVCODEC_DLL 0 AVCODEC_LIB)
    list(GET AVUTIL_DLL 0 AVUTIL_LIB)
    list(GET SWSCALE_DLL 0 SWSCALE_LIB)
    message(STATUS "FFmpeg: linking via DLLs from ${FFMPEG_BIN}")
else()
    # Fallback: .dll.a import libs
    find_library(AVCODEC_LIB avcodec.dll.a PATHS "${FFMPEG_LIB}" NO_DEFAULT_PATH)
    find_library(AVUTIL_LIB  avutil.dll.a  PATHS "${FFMPEG_LIB}" NO_DEFAULT_PATH)
    find_library(SWSCALE_LIB swscale.dll.a PATHS "${FFMPEG_LIB}" NO_DEFAULT_PATH)
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

message(STATUS "FFmpeg: OK (${AVCODEC_LIB})")

# ============================================
# Windows libraries
# ============================================
target_link_libraries(hscrcpy
    gdi32 gdiplus ws2_32 shell32 user32 kernel32 ole32
)

target_compile_definitions(hscrcpy PRIVATE _CRT_SECURE_NO_WARNINGS)
target_compile_options(hscrcpy PRIVATE -flto)
target_link_options(hscrcpy PRIVATE -static -static-libgcc -static-libstdc++ -flto)

install(TARGETS hscrcpy DESTINATION bin)