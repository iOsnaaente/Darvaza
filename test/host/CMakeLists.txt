# Testes no PC (sem ESP-IDF):
#   cmake -S test/host -B build-host && cmake --build build-host && build-host/host_tests
cmake_minimum_required(VERSION 3.16)
project(darvaza_host_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(C ${CMAKE_CURRENT_SOURCE_DIR}/../../components)

add_executable(host_tests
    test_main.cpp
    ${C}/proto/proto.cpp
    ${C}/display/anim.cpp
    ${C}/audio/ssl.cpp)
target_include_directories(host_tests PRIVATE ${C}/proto/include ${C}/display ${C}/audio)
target_compile_options(host_tests PRIVATE -Wall -Wextra -O2)

enable_testing()
add_test(NAME host_tests COMMAND host_tests)
