# SPDX-License-Identifier: MIT
# © LinuxUDPShardedEcho Contributors. All rights reserved.

cmake_minimum_required(VERSION 3.20)

project(LinuxUDPShardedEcho LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Option to enable AddressSanitizer
option(ENABLE_ADDRESS_SANITIZER "Enable AddressSanitizer (ASan) for supported compilers" OFF)

# Server executable
add_executable(echo_server
    src/server/main.cpp
    src/common/socket_utils.cpp
)

target_include_directories(echo_server PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# Client executable
add_executable(echo_client
    src/client/main.cpp
    src/common/socket_utils.cpp
    src/common/tdigest.cpp
    src/common/percentile_estimator.cpp
)

target_include_directories(echo_client PRIVATE
    ${CMAKE_SOURCE_DIR}/src
)

# If requested, enable AddressSanitizer for echo_client (MSVC or gcc/clang)
if(ENABLE_ADDRESS_SANITIZER)
    message(STATUS "Enabling AddressSanitizer for target: echo_client (gcc/clang)")
    target_compile_options(echo_client PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(echo_client PRIVATE -fsanitize=address)
endif()

