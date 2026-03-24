cmake_minimum_required(VERSION 3.14)
project(AksynAudioPrototype)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 1. Fetch IXWebSocket (Zero dependency, native Windows socket support)
include(FetchContent)
FetchContent_Declare(
    ixwebsocket
    GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
    GIT_TAG master
)
# Turn off TLS/SSL so we don't need OpenSSL on Windows for the prototype
set(USE_TLS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ixwebsocket)

# 2. Download miniaudio.h
set(MINIAUDIO_DIR ${CMAKE_BINARY_DIR}/miniaudio)
file(DOWNLOAD 
    https://raw.githubusercontent.com/mackron/miniaudio/master/miniaudio.h 
    ${MINIAUDIO_DIR}/miniaudio.h
)

# 3. Build Node A
add_executable(node_A node_A.cpp)
target_include_directories(node_A PRIVATE ${MINIAUDIO_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(node_A PRIVATE ixwebsocket)

# 4. Build Node B
add_executable(node_B node_B.cpp)
target_include_directories(node_B PRIVATE ${MINIAUDIO_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(node_B PRIVATE ixwebsocket)