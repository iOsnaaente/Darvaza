cmake_minimum_required(VERSION 3.13)
project(npu_check C)

set(RKNN_EXAMPLE ${RV1106_THIRD_PARTY}/luckfox_pico_rknn_example)

add_executable(npu_check main.c)
target_include_directories(npu_check PRIVATE ${RKNN_EXAMPLE}/include/rknn)
target_link_directories(npu_check PRIVATE ${RKNN_EXAMPLE}/lib/uclibc)
target_link_libraries(npu_check rknnmrt)
# A imagem da placa pode trazer um librknnmrt mais velho que o dos modelos
# (a nossa veio com 1.4.1); o 1.6.0 vai junto em lib/ e tem prioridade.
set_target_properties(npu_check PROPERTIES INSTALL_RPATH "$ORIGIN/lib")

# Pasta pronta para mandar para a placa: binário, runtime e modelo de teste.
install(TARGETS npu_check DESTINATION .)
install(FILES ${RKNN_EXAMPLE}/lib/uclibc/librknnmrt.so DESTINATION lib)
install(FILES ${RKNN_EXAMPLE}/example/luckfox_pico_retinaface_facenet/model/RetinaFace.rknn
        DESTINATION model)
