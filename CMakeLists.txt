cmake_minimum_required(VERSION 3.16)
project(musico_verse CXX)

# ── C++ Standard ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_compile_options(
    -Wall -Wextra -Wno-unused-parameter
    -O2
)

set(SOURCES
    src/main.cpp
    src/player.cpp
    src/audio_engine.cpp
    src/audio_stub.cpp
    src/av_decoder.cpp
    src/visualizer.cpp
    src/playlist.cpp
)

add_executable(musico_verse ${SOURCES})
target_include_directories(musico_verse PRIVATE src/)

# ── ncurses ───────────────────────────────────────────────────────────────────
find_package(Curses REQUIRED)
target_include_directories(musico_verse PRIVATE ${CURSES_INCLUDE_DIRS})
target_link_libraries(musico_verse PRIVATE ${CURSES_LIBRARIES})

find_library(TINFO_LIB tinfo)
if(TINFO_LIB)
    target_link_libraries(musico_verse PRIVATE ${TINFO_LIB})
endif()

# ── FFmpeg ────────────────────────────────────────────────────────────────────
find_path(AVFORMAT_INC NAMES libavformat/avformat.h)
find_library(AVFORMAT_LIB avformat)
find_library(AVCODEC_LIB  avcodec)
find_library(AVUTIL_LIB   avutil)
find_library(SWRESAMPLE_LIB swresample)

if(NOT AVFORMAT_LIB)
    message(FATAL_ERROR "FFmpeg not found — run: pkg install ffmpeg")
endif()

target_include_directories(musico_verse PRIVATE ${AVFORMAT_INC})
 target_link_libraries(musico_verse PRIVATE
     avformat
     avcodec
     avutil
     swscale
     swresample
     z
     bz2
     lzma        # Add this for TIFF and other compressed formats
     drm         # Add this for the hardware context symbols
     iconv
     x264
     x265
     OpenCL
     android-shmem
     pthread
 )



#target_link_libraries(musico_verse PRIVATE
#    ${AVFORMAT_LIB}
#    ${AVCODEC_LIB}
#    ${AVUTIL_LIB}
#    ${SWRESAMPLE_LIB}
#)

# ── OpenSL ES (Android / Termux) ─────────────────────────────────────────────
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android" OR EXISTS "/data/data/com.termux")
    target_link_libraries(musico_verse PRIVATE OpenSLES)
    target_compile_definitions(musico_verse PRIVATE __ANDROID__)
else()
    message(STATUS "Non-Android: audio stubbed (silent playback)")
    target_compile_definitions(musico_verse PRIVATE AUDIO_STUB)
endif()

# ── Threads + stdlib ──────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
target_link_libraries(musico_verse PRIVATE Threads::Threads stdc++)

# ── Linker flags for small Termux binary ─────────────────────────────────────
if(CMAKE_SYSTEM_NAME STREQUAL "Android" OR EXISTS "/data/data/com.termux")
    target_link_options(musico_verse PRIVATE -Wl,--gc-sections)
endif()

install(TARGETS musico_verse DESTINATION bin)
