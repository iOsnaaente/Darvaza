idf_component_register(
    SRCS "app_main.cpp" "frame_cap_pipeline.cpp" "face_recognition_app.cpp"
    INCLUDE_DIRS "."
    REQUIRES who_spiflash_fatfs who_recognition esp_video_cxx esp_video console
)

# O esp_ipa não falha o build se o JSON de tuning sumir (o check dele é
# message(FETAL_ERROR ...), que não é um modo do CMake). Aqui o caminho já é
# final, então confere de verdade.
idf_build_get_property(imx708_ipa_json IMX708_IPA_JSON_RESOLVED)
idf_build_get_property(project_dir PROJECT_DIR)
get_filename_component(imx708_ipa_abs "${imx708_ipa_json}" ABSOLUTE BASE_DIR "${project_dir}")
if(NOT EXISTS "${imx708_ipa_abs}")
    message(FATAL_ERROR "Tuning do ISP da IMX708 não encontrado em:\n  ${imx708_ipa_abs}")
endif()
