cmake_minimum_required(VERSION 3.16)
project(AudioLink VERSION 2.0 LANGUAGES CXX)

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
    # Try a local checkout at <project_root>/third_party/IXWebSocket
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
#  We do NOT create a library target for miniaudio because MINIAUDIO_IMPLEMENTATION
#  must be defined in exactly one .cpp per executable.  Instead we just expose
#  an INTERFACE target that adds the include path.
# ─────────────────────────────────────────────────────────────────────────────
add_library(miniaudio INTERFACE)

# Look for miniaudio.h in the project root, then in third_party/miniaudio/
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
#  Shared include directory (protocol.h, session.h, etc.)
# ─────────────────────────────────────────────────────────────────────────────
set(SHARED_INCLUDE_DIR "${CMAKE_SOURCE_DIR}")

# ─────────────────────────────────────────────────────────────────────────────
#  Platform link libraries
# ─────────────────────────────────────────────────────────────────────────────
set(PLATFORM_LIBS "")
if(WIN32)
    list(APPEND PLATFORM_LIBS ws2_32 ole32 winmm)
elseif(APPLE)
    find_library(CORE_AUDIO   CoreAudio   REQUIRED)
    find_library(AUDIO_UNIT   AudioUnit   REQUIRED)
    find_library(AUDIO_TOOLBOX AudioToolbox REQUIRED)
    find_library(CORE_FOUNDATION CoreFoundation REQUIRED)
    list(APPEND PLATFORM_LIBS
        ${CORE_AUDIO} ${AUDIO_UNIT} ${AUDIO_TOOLBOX} ${CORE_FOUNDATION})
elseif(UNIX)
    find_package(Threads REQUIRED)
    list(APPEND PLATFORM_LIBS Threads::Threads dl m)
    # ALSA (Linux default) — swap for pulse / jack if preferred
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
    add_compile_options(/W3 /wd4996)           # suppress POSIX deprecation noise
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# ─────────────────────────────────────────────────────────────────────────────
#  Helper: resolve the ixwebsocket link target regardless of how it was found
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
#
#  Sources
#    node_A.cpp          — main(): wires Session + AudioCapture + Transmitter
#    session.cpp         — Session::init() random ID + timestamp
#    audio_capture.cpp   — MINIAUDIO_IMPLEMENTATION lives here for node_A
#                          probe_native_sample_rate(), device open/start/stop
#    transmitter.cpp     — IXWebSocket server, client list, packet broadcast
# ─────────────────────────────────────────────────────────────────────────────
add_executable(node_A
    node_A.cpp
    session.cpp
    audio_capture.cpp
    transmitter.cpp
)

target_include_directories(node_A PRIVATE "${SHARED_INCLUDE_DIR}")
target_link_libraries(node_A PRIVATE ${IX_TARGET} miniaudio ${PLATFORM_LIBS})

# ─────────────────────────────────────────────────────────────────────────────
#  node_B
#
#  Sources
#    node_B.cpp          — main(): wires WavSaver + Playback + Receiver
#    playback.cpp        — MINIAUDIO_IMPLEMENTATION lives here for node_B
#                          miniaudio playback device, pull-callback trampoline
#    wav_saver.cpp       — background writer thread, ma_encoder, INFO chunk
#    receiver.cpp        — WebSocket client, jitter buffer, PLC, latency stats
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
#  Installation (optional — 'cmake --install .')
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
message(STATUS "  Output dir  : ${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
message(STATUS "")
