# Toolchain CMake para a Luckfox Pico Max (RV1106, Buildroot/uclibc).
# Uso: cmake -B build -DCMAKE_TOOLCHAIN_FILE=<rv1106_test>/cmake/rv1106.cmake

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

get_filename_component(_rv1106_root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(RV1106_TOOLCHAIN
    "${_rv1106_root}/sdk/luckfox-pico/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf")
set(CMAKE_C_COMPILER   "${RV1106_TOOLCHAIN}/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc")
set(CMAKE_CXX_COMPILER "${RV1106_TOOLCHAIN}/bin/arm-rockchip830-linux-uclibcgnueabihf-g++")

# Bibliotecas da placa (RKNN, OpenCV-mobile, RGA, rockit) ficam nos exemplos da Luckfox.
set(RV1106_THIRD_PARTY "${_rv1106_root}/third_party")

# Programas (cmake, python) vêm do PC; bibliotecas e headers nunca de /usr do PC.
set(CMAKE_FIND_ROOT_PATH "${RV1106_TOOLCHAIN}/arm-rockchip830-linux-uclibcgnueabihf/sysroot")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
