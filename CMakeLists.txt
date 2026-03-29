cmake_minimum_required(VERSION 3.16)
project(AudioLink VERSION 2.0 LANGUAGES C CXX)

# ─────────────────────────────────────────────────────────────────────────────
#  C++ standard
# ─────────────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─────────────────────────────────────────────────────────────────────────────
#  Build type default
# ─────────────────────────────────────────────────────────────────────────────
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  Output directories — keep .exe files next to each other
# ─────────────────────────────────────────────────────────────────────────────
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ─────────────────────────────────────────────────────────────────────────────
#  Find IXWebSocket
#
#  Preferred: installed via vcpkg or system package manager.
#  Fallback:  manually set IXWEBSOCKET_DIR on the cmake command line.
# ─────────────────────────────────────────────────────────────────────────────
find_package(ixwebsocket CONFIG QUIET)

if(NOT ixwebsocket_FOUND)
    set(_ix_fallback "${CMAKE_SOURCE_DIR}/third_party/IXWebSocket")
    if(EXISTS "${_ix_fallback}/CMakeLists.txt")
        message(STATUS "[AudioLink] Using bundled IXWebSocket at ${_ix_fallback}")
        set(USE_TLS OFF CACHE BOOL "" FORCE)
        add_subdirectory("${_ix_fallback}" ix_build EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR
            "IXWebSocket not found.\n"
            "Install it with vcpkg:  vcpkg install ixwebsocket\n"
            "Or clone it:            git clone https://github.com/machinezone/IXWebSocket "
            "third_party/IXWebSocket")
    endif()
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  miniaudio — header-only, lives in the project root (or third_party/)
# ─────────────────────────────────────────────────────────────────────────────
add_library(miniaudio INTERFACE)

if(EXISTS "${CMAKE_SOURCE_DIR}/miniaudio.h")
    target_include_directories(miniaudio INTERFACE "${CMAKE_SOURCE_DIR}")
elseif(EXISTS "${CMAKE_SOURCE_DIR}/third_party/miniaudio/miniaudio.h")
    target_include_directories(miniaudio INTERFACE
        "${CMAKE_SOURCE_DIR}/third_party/miniaudio")
else()
    message(FATAL_ERROR
        "miniaudio.h not found.\n"
        "Download it:  https://github.com/mackron/miniaudio/releases\n"
        "Place it at:  ${CMAKE_SOURCE_DIR}/miniaudio.h")
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  RNNoise — optional neural noise suppressor (used by node_A only)
#
#  Detection order:
#    1. System package  (MSYS2: pacman -S mingw-w64-ucrt-x86_64-rnnoise)
#                       (Ubuntu: apt install librnnoise-dev)
#    2. Local source    third_party/rnnoise/   (git clone)
#
#  When found, HAVE_RNNOISE is defined for node_A's sources and rnnoise
#  is added to node_A's link libraries.
#
#  To skip RNNoise entirely:  cmake ... -DRNNOISE=OFF
# ─────────────────────────────────────────────────────────────────────────────
option(RNNOISE "Enable RNNoise neural noise suppression on node_A" ON)

set(RNNOISE_FOUND FALSE)
set(RNNOISE_TARGET "")

if(RNNOISE)
    # ── Try system package first ──────────────────────────────────────────────
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(RNNOISE_PKG QUIET rnnoise)
        if(RNNOISE_PKG_FOUND)
            message(STATUS "[AudioLink] RNNoise: found via pkg-config")
            add_library(rnnoise_iface INTERFACE)
            target_include_directories(rnnoise_iface INTERFACE ${RNNOISE_PKG_INCLUDE_DIRS})
            target_link_libraries(rnnoise_iface INTERFACE ${RNNOISE_PKG_LIBRARIES})
            set(RNNOISE_TARGET rnnoise_iface)
            set(RNNOISE_FOUND TRUE)
        endif()
    endif()

    # ── Fallback: local source checkout ──────────────────────────────────────
    if(NOT RNNOISE_FOUND)
        set(_rnn_src "${CMAKE_SOURCE_DIR}/third_party/rnnoise")
        if(EXISTS "${_rnn_src}/src/rnnoise.c")
            message(STATUS "[AudioLink] RNNoise: building from source at ${_rnn_src}")

            # Build only the files we need — skip the training/demo tools
            add_library(rnnoise_static STATIC
                "${_rnn_src}/src/rnn.c"
                "${_rnn_src}/src/rnn_data.c"
                "${_rnn_src}/src/rnn_reader.c"
                "${_rnn_src}/src/opus_encoder.c"
                "${_rnn_src}/src/kiss_fft.c"
                "${_rnn_src}/src/pitch.c"
                "${_rnn_src}/src/celt_lpc.c"
                "${_rnn_src}/src/denoise.c"
            )
            target_include_directories(rnnoise_static PUBLIC
                "${_rnn_src}/include"
                "${_rnn_src}/src"
            )
            # Suppress warnings from third-party code
            target_compile_options(rnnoise_static PRIVATE
                $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-w>
                $<$<CXX_COMPILER_ID:MSVC>:/w>
            )
            set(RNNOISE_TARGET rnnoise_static)
            set(RNNOISE_FOUND TRUE)
        else()
            message(STATUS "[AudioLink] RNNoise: not found — noise suppression disabled")
            message(STATUS "            To enable, run one of:")
            message(STATUS "              MSYS2:  pacman -S mingw-w64-ucrt-x86_64-rnnoise")
            message(STATUS "              Ubuntu: apt install librnnoise-dev")
            message(STATUS "              Source: git clone https://github.com/xiph/rnnoise third_party/rnnoise")
        endif()
    endif()
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  Shared include directory
# ─────────────────────────────────────────────────────────────────────────────
set(SHARED_INCLUDE_DIR "${CMAKE_SOURCE_DIR}")

# ─────────────────────────────────────────────────────────────────────────────
#  Platform link libraries
# ─────────────────────────────────────────────────────────────────────────────
set(PLATFORM_LIBS "")
if(WIN32)
    list(APPEND PLATFORM_LIBS ws2_32 ole32 winmm)
elseif(APPLE)
    find_library(CORE_AUDIO    CoreAudio    REQUIRED)
    find_library(AUDIO_UNIT    AudioUnit    REQUIRED)
    find_library(AUDIO_TOOLBOX AudioToolbox REQUIRED)
    find_library(CORE_FOUNDATION CoreFoundation REQUIRED)
    list(APPEND PLATFORM_LIBS
        ${CORE_AUDIO} ${AUDIO_UNIT} ${AUDIO_TOOLBOX} ${CORE_FOUNDATION})
elseif(UNIX)
    find_package(Threads REQUIRED)
    list(APPEND PLATFORM_LIBS Threads::Threads dl m)
    find_package(ALSA QUIET)
    if(ALSA_FOUND)
        list(APPEND PLATFORM_LIBS ALSA::ALSA)
        add_compile_definitions(MA_ENABLE_ALSA)
    endif()
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  Compiler warnings
# ─────────────────────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W3 /wd4996)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  Helper: resolve the ixwebsocket link target
# ─────────────────────────────────────────────────────────────────────────────
if(TARGET ixwebsocket::ixwebsocket)
    set(IX_TARGET ixwebsocket::ixwebsocket)
elseif(TARGET ixwebsocket)
    set(IX_TARGET ixwebsocket)
else()
    message(FATAL_ERROR "Cannot locate IXWebSocket CMake target")
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  node_A
# ─────────────────────────────────────────────────────────────────────────────
add_executable(node_A
    node_A.cpp
    session.cpp
    audio_capture.cpp
    transmitter.cpp
)

target_include_directories(node_A PRIVATE "${SHARED_INCLUDE_DIR}")
target_link_libraries(node_A PRIVATE ${IX_TARGET} miniaudio ${PLATFORM_LIBS})

# Wire in RNNoise when available
if(RNNOISE_FOUND)
    target_compile_definitions(node_A PRIVATE HAVE_RNNOISE)
    target_link_libraries(node_A PRIVATE ${RNNOISE_TARGET})
    message(STATUS "[AudioLink] node_A: RNNoise denoising ENABLED")
else()
    message(STATUS "[AudioLink] node_A: RNNoise denoising DISABLED")
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  node_B  (unchanged — no audio processing added here)
# ─────────────────────────────────────────────────────────────────────────────
add_executable(node_B
    node_B.cpp
    playback.cpp
    wav_saver.cpp
    receiver.cpp
)

target_include_directories(node_B PRIVATE "${SHARED_INCLUDE_DIR}")
target_link_libraries(node_B PRIVATE ${IX_TARGET} miniaudio ${PLATFORM_LIBS})

# ─────────────────────────────────────────────────────────────────────────────
#  Installation
# ─────────────────────────────────────────────────────────────────────────────
install(TARGETS node_A node_B
        RUNTIME DESTINATION bin)

# ─────────────────────────────────────────────────────────────────────────────
#  Summary
# ─────────────────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "AudioLink v${PROJECT_VERSION}")
message(STATUS "  Build type  : ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ std     : ${CMAKE_CXX_STANDARD}")
message(STATUS "  IXWebSocket : ${IX_TARGET}")
message(STATUS "  RNNoise     : ${RNNOISE_FOUND}")
message(STATUS "  Output dir  : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "")

# ── sqlite3 static lib ────────────────────────────────────────────────────────
add_library(sqlite3_lib STATIC sqlite3.c)
target_include_directories(sqlite3_lib PUBLIC "${CMAKE_SOURCE_DIR}")
if(UNIX)
    target_link_libraries(sqlite3_lib PUBLIC dl pthread)
endif()

# ── api_server ────────────────────────────────────────────────────────────────
add_executable(api_server api_server.cpp db_writer.cpp)
target_include_directories(api_server PRIVATE "${CMAKE_SOURCE_DIR}")
target_link_libraries(api_server PRIVATE sqlite3_lib ${PLATFORM_LIBS})
if(WIN32)
    target_link_libraries(api_server PRIVATE ws2_32)
endif()

# ── Add db_writer to node_B ───────────────────────────────────────────────────
# Find the existing node_B target and add db_writer.cpp to it
target_sources(node_B PRIVATE db_writer.cpp)
target_link_libraries(node_B PRIVATE sqlite3_lib)