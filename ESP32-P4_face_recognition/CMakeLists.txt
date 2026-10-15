cmake_minimum_required(VERSION 3.16)

# Componentes do esp-who copiados para esp-who/ (ver esp-who/README.md)
set(EXTRA_COMPONENT_DIRS esp-who/components/who_task
                         esp-who/components/who_peripherals/esp_video_cxx
                         esp-who/components/who_peripherals/who_spiflash_fatfs
                         esp-who/components/who_frame_cap
                         esp-who/components/who_detect
                         esp-who/components/who_recognition)

add_compile_options(-fdiagnostics-color=always)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Tuning do ISP da IMX708 (AE/AWB/AF). Precisa ficar entre project.cmake e
# project(): o esp_ipa lê a propriedade enquanto processa o próprio CMakeLists.
# Se o caminho estiver errado o esp_ipa só imprime uma linha e segue sem
# auto-exposição nem balanço de branco, por isso main/CMakeLists.txt confere.
set(imx708_ipa_json
    "managed_components/mushbraindave__esp_cam_sensor_imx/sensors/imx708/cfg/imx708_default.json")
idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH "${imx708_ipa_json}" APPEND)
idf_build_set_property(IMX708_IPA_JSON_RESOLVED "${imx708_ipa_json}")

project(face_recognition)
