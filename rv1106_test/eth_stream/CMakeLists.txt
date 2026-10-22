cmake_minimum_required(VERSION 3.13)
project(eth_stream CXX)

set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Câmera e utilitários são os do face_app; bibliotecas como lá (RKMPI VI +
# ISP, RGA, OpenCV-mobile com JPEG por hardware).
set(FACE_APP ${CMAKE_CURRENT_SOURCE_DIR}/../face_app/src)
set(RKMPI ${RV1106_THIRD_PARTY}/luckfox_pico_rkmpi_example)
set(OpenCV_DIR ${RKMPI}/lib/uclibc/lib/cmake/opencv4)
find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)

add_executable(eth_stream main.cpp ${FACE_APP}/camera.cpp ${FACE_APP}/util.cpp)
target_compile_options(eth_stream PRIVATE -Wall -Wextra)
target_compile_definitions(eth_stream PRIVATE ISP_HW_V30 RKPLATFORM=ON ARCH64=OFF UAPI2)
target_include_directories(eth_stream PRIVATE
    ${FACE_APP}
    ${RKMPI}/include
    ${RKMPI}/include/librga
    ${RKMPI}/include/rkaiq
    ${RKMPI}/include/rkaiq/uAPI2
    ${RKMPI}/include/rkaiq/common
    ${RKMPI}/include/rkaiq/xcore
    ${RKMPI}/include/rkaiq/algos
    ${RKMPI}/include/rkaiq/iq_parser
    ${RKMPI}/include/rkaiq/iq_parser_v2
    ${RKMPI}/include/rkaiq/smartIr
)
target_link_directories(eth_stream PRIVATE ${RKMPI}/lib/uclibc)
target_link_options(eth_stream PRIVATE -Wl,-rpath-link,${RKMPI}/lib/uclibc)
target_link_libraries(eth_stream ${OpenCV_LIBS} rockit rockchip_mpp rkaiq rga Threads::Threads)
set_target_properties(eth_stream PROPERTIES INSTALL_RPATH "$ORIGIN/lib")

install(TARGETS eth_stream DESTINATION .)
install(FILES ${RKMPI}/lib/uclibc/librga.so DESTINATION lib)
