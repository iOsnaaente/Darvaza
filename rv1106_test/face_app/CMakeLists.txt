cmake_minimum_required(VERSION 3.13)
project(face_app CXX)

set(CMAKE_CXX_STANDARD 17)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Câmera (RKMPI VI + ISP), RGA, OpenCV-mobile (JPEG por hardware) e
# librknnmrt vêm do exemplo RKMPI da Luckfox; os modelos, do exemplo RKNN.
set(RKMPI ${RV1106_THIRD_PARTY}/luckfox_pico_rkmpi_example)
set(OpenCV_DIR ${RKMPI}/lib/uclibc/lib/cmake/opencv4)
find_package(OpenCV REQUIRED)
find_package(Threads REQUIRED)

add_executable(face_app
    src/main.cpp
    src/asr.cpp
    src/camera.cpp
    src/face.cpp
    src/models.cpp
    src/rknn_model.cpp
    src/http.cpp
    src/overlay.cpp
    src/util.cpp
)
target_compile_options(face_app PRIVATE -Wall -Wextra)
# Os headers do rkaiq dependem destas definições (as dos exemplos RKMPI).
target_compile_definitions(face_app PRIVATE ISP_HW_V30 RKPLATFORM=ON ARCH64=OFF UAPI2)
target_include_directories(face_app PRIVATE
    ${RKMPI}/include
    ${RKMPI}/include/rknn
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
target_link_directories(face_app PRIVATE ${RKMPI}/lib/uclibc)
target_link_options(face_app PRIVATE -Wl,-rpath-link,${RKMPI}/lib/uclibc)
target_link_libraries(face_app ${OpenCV_LIBS} rknnmrt rockit rockchip_mpp rkaiq rga Threads::Threads)
# librknnmrt e librga vão junto em lib/ (imagens antigas trazem versões
# velhas); rockit, mpp e rkaiq são os da imagem (/oem/usr/lib).
set_target_properties(face_app PROPERTIES INSTALL_RPATH "$ORIGIN/lib")

# Pasta pronta para mandar para a placa.
set(MODELS ${RV1106_THIRD_PARTY}/luckfox_pico_rknn_example/example/luckfox_pico_retinaface_facenet/model)
install(TARGETS face_app DESTINATION .)
install(FILES www/index.html www/asr.html DESTINATION .)
install(FILES ${RKMPI}/lib/uclibc/librknnmrt.so ${RKMPI}/lib/uclibc/librga.so DESTINATION lib)
install(FILES ${MODELS}/RetinaFace.rknn ${MODELS}/mobilefacenet.rknn DESTINATION model)
