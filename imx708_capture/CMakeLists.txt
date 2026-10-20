# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.16)

# ---------------------------------------------------------------------------
# This example ships inside the esp_cam_sensor_imx component, so it builds two
# ways, and the driver arrives from a different place in each:
#
#   in-repo    main/idf_component.yml carries an override_path pointing at the
#              component root two directories up, so the solver uses this
#              working tree instead of downloading a published version.
#
#   from the   `idf.py create-project-from-example` copies this directory out
#   registry   on its own and the driver is resolved normally, landing in
#              managed_components/mushbraindave__esp_cam_sensor_imx.
#
# Only the in-repo layout can be detected here: managed_components/ does not
# exist yet at this point on a clean build - the dependency solver runs inside
# project() - whereas the component root either is in this checkout or is not.
set(imx_component_root "${CMAKE_CURRENT_LIST_DIR}/../..")
if(EXISTS "${imx_component_root}/idf_component.yml")
    set(imx708_ipa_json "${imx_component_root}/sensors/imx708/cfg/imx708_default.json")
else()
    set(imx708_ipa_json
        "managed_components/mushbraindave__esp_cam_sensor_imx/sensors/imx708/cfg/imx708_default.json")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Hand the IMX708's ISP tuning config to esp_ipa. This has to sit between
# project.cmake and project(): esp_ipa reads the property while its own
# CMakeLists is processed, which is too late for a component to set it.
#
# Only the string is needed now - esp_ipa resolves it against the project
# directory and checks it exists when *it* is configured, by which time the
# component manager has already populated managed_components/. So the registry
# path above is valid even on a clean first build.
#
# Get it wrong and nothing complains: esp_ipa's existence check calls
# message(FETAL_ERROR ...), which is not a real CMake mode, so a missing file
# prints a line and the build carries on to produce a stream with no
# auto-exposure and no white balance. main/CMakeLists.txt re-checks the
# resolved path and fails the build instead.
#
# The OV5647 (the camera Waveshare bundles with its P4 kits) is enabled too and
# esp_video binds whichever sensor answers. Its tuning is NOT added here:
# esp_cam_sensor appends it itself (CAMERA_OV5647_DEFAULT_IPA_JSON_CONFIGURATION_FILE),
# and listing it twice makes esp_ipa's generator abort.
idf_build_set_property(ESP_IPA_JSON_CONFIG_FILE_PATH "${imx708_ipa_json}" APPEND)
idf_build_set_property(CAMERA_IPA_JSONS_RESOLVED "${imx708_ipa_json}")

project(imx708_capture)

# O esp-sr registra no esp-dl a lista de ops que o WakeNet usa
# (tool/esp-sr-req.yml), e com qualquer lista registrada o esp-dl compila SÓ
# os ops listados. Os modelos de rosto não registram a deles: sem isto o
# detector e o reconhecedor deixariam de carregar ("no Conv kernel" ou op
# desconhecido). Sem nenhuma lista o esp-dl compila tudo, como antes do
# esp-sr. Tem que vir depois de project(): o esp-dl só lê a lista no fim da
# configuração (cmake_language DEFER), depois desta linha.
idf_build_set_property(ESPDL_COMPILE_REQS "")
