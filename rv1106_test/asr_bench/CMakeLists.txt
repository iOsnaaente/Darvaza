cmake_minimum_required(VERSION 3.13)
project(asr_bench C CXX)

# Só o asr_whisper (uClibc, como o resto). O asr_vosk é compilado contra a
# glibc pelo scripts/asr.sh: a libvosk.so pronta é glibc.

set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# O ggml vê __GLIBC__ (a uClibc define, por compatibilidade) e inclui
# execinfo.h, que a uClibc não tem: compat/ tem um execinfo.h vazio.
include_directories(SYSTEM ${CMAKE_CURRENT_SOURCE_DIR}/compat)
add_compile_options(-mcpu=cortex-a7)
# std::filesystem (backends do ggml) fica numa lib à parte no GCC 8.
set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} -lstdc++fs")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GGML_NATIVE OFF CACHE BOOL "" FORCE)
set(GGML_OPENMP OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER OFF CACHE BOOL "" FORCE)
add_subdirectory(${RV1106_THIRD_PARTY}/whisper.cpp whisper.cpp EXCLUDE_FROM_ALL)

add_executable(asr_whisper src/asr_whisper.cpp)
target_compile_options(asr_whisper PRIVATE -Wall -Wextra)
target_link_libraries(asr_whisper whisper)

install(TARGETS asr_whisper DESTINATION bin)
install(PROGRAMS vosk.sh DESTINATION bin)
install(FILES engines.tsv comandos.json DESTINATION .)
