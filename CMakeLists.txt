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

# ── Platform detection (moved above SOURCES — audit "audio_engine.cpp +
# audio_stub.cpp both compiled unconditionally") ────────────────────────────
# BUGFIX: audio_engine.cpp and audio_stub.cpp both unconditionally *define*
# every AudioEngine:: method (their bodies vary per #ifdef __ANDROID__ /
# #ifdef AUDIO_STUB, but the function signatures themselves are always
# compiled). Previously both files were always in SOURCES regardless of
# platform, so any non-Android build hit duplicate-symbol linker errors —
# the project could not link on desktop Linux/macOS as configured, even
# though the README documents that as a supported target. Exactly one of
# these two files must be compiled per build now.
set(CLIMUSIC_IS_ANDROID FALSE)
if(ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "Android" OR EXISTS "/data/data/com.termux")
    set(CLIMUSIC_IS_ANDROID TRUE)
endif()

set(SOURCES
    src/main.cpp
    src/player.cpp
    src/av_decoder.cpp
    src/visualizer.cpp
    src/playlist.cpp
)
if(CLIMUSIC_IS_ANDROID)
    list(APPEND SOURCES src/audio_engine.cpp)
else()
    list(APPEND SOURCES src/audio_stub.cpp)
endif()

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
find_library(SWSCALE_LIB  swscale)
find_library(SWRESAMPLE_LIB swresample)

if(NOT AVFORMAT_LIB)
    message(FATAL_ERROR "FFmpeg not found — run: pkg install ffmpeg")
endif()

target_include_directories(musico_verse PRIVATE ${AVFORMAT_INC})
target_link_libraries(musico_verse PRIVATE
    ${AVFORMAT_LIB}
    ${AVCODEC_LIB}
    ${AVUTIL_LIB}
    ${SWSCALE_LIB}
    ${SWRESAMPLE_LIB}
)

# BUGFIX (audit C10): these used to be hardcoded bare names passed straight
# to target_link_libraries instead of the find_library-resolved paths above
# (which were computed and then left unused) — only worked by coincidence
# on systems where the linker's default search path happens to contain
# exactly these names. Now each is actually probed with find_library and
# only linked if genuinely present, so a build never hard-fails over an
# optional codec/compression lib the host simply doesn't have.
foreach(OPT_LIB z bz2 lzma iconv x264 x265)
    string(TOUPPER ${OPT_LIB} OPT_LIB_UPPER)
    find_library(${OPT_LIB_UPPER}_FOUND ${OPT_LIB})
    if(${OPT_LIB_UPPER}_FOUND)
        target_link_libraries(musico_verse PRIVATE ${${OPT_LIB_UPPER}_FOUND})
    endif()
endforeach()

# drm/OpenCL/android-shmem are Android/Termux-FFmpeg-build specifics, not
# general desktop dependencies — android-shmem in particular doesn't exist
# on desktop systems at all and would fail the link outright if forced.
if(CLIMUSIC_IS_ANDROID)
    foreach(OPT_LIB drm OpenCL android-shmem)
        string(TOUPPER ${OPT_LIB} OPT_LIB_UPPER)
        string(REPLACE "-" "_" OPT_LIB_UPPER ${OPT_LIB_UPPER})
        find_library(${OPT_LIB_UPPER}_FOUND ${OPT_LIB})
        if(${OPT_LIB_UPPER}_FOUND)
            target_link_libraries(musico_verse PRIVATE ${${OPT_LIB_UPPER}_FOUND})
        endif()
    endforeach()
endif()

# ── OpenSL ES (Android / Termux) ─────────────────────────────────────────────
if(CLIMUSIC_IS_ANDROID)
    target_link_libraries(musico_verse PRIVATE OpenSLES)
    target_link_libraries(musico_verse PRIVATE x265 ${OTHER_LIBS})
    target_compile_definitions(musico_verse PRIVATE __ANDROID__)
else()
    message(STATUS "Non-Android: audio stubbed (silent playback)")
    target_compile_definitions(musico_verse PRIVATE AUDIO_STUB)
endif()

# ── Threads + stdlib ──────────────────────────────────────────────────────────
find_package(Threads REQUIRED)
# BUGFIX (audit D7): "stdc++" was hardcoded, which only exists on
# glibc/libstdc++ toolchains — this would fail to link on any system using
# libc++ (macOS's default clang, some Termux/Android NDK setups). The
# compiler driver already links the correct C++ runtime for whichever
# toolchain is actually in use; forcing a specific one here was both
# redundant and non-portable.
target_link_libraries(musico_verse PRIVATE Threads::Threads)

# ── Linker flags for small Termux binary ─────────────────────────────────────
if(CLIMUSIC_IS_ANDROID)
    target_link_options(musico_verse PRIVATE -Wl,--gc-sections)
endif()

install(TARGETS musico_verse DESTINATION bin)
