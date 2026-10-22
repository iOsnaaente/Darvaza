idf_component_register(
    SRCS "asr.c" "audio.c" "isp_tune.c" "imx708_capture_main.c" "face.cpp" "gaze.cpp" "overlay.c" "wifi.c"
    INCLUDE_DIRS "."
    EMBED_TXTFILES "index.html" "asr.html"
    REQUIRES esp_video esp_cam_sensor_imx esp_driver_jpeg esp_driver_i2c esp_http_server esp_timer
             esp_wifi esp_netif esp_event nvs_flash lwip fatfs mbedtls human_face_recognition
             esp_ipa esp_driver_i2s esp_codec_dev esp-sr
)

# Fail the build if an ISP tuning config did not resolve. esp_ipa's own
# existence check is spelled message(FETAL_ERROR ...) - not a real CMake mode -
# so it prints and carries on, and a missing file shows up much later as a
# stream with no auto-exposure and no white balance rather than as an error.
# By the time this component is configured the paths are final, so check here.
idf_build_get_property(camera_ipa_jsons CAMERA_IPA_JSONS_RESOLVED)
idf_build_get_property(camera_project_dir PROJECT_DIR)
foreach(ipa_json IN LISTS camera_ipa_jsons)
    get_filename_component(ipa_abs "${ipa_json}" ABSOLUTE BASE_DIR "${camera_project_dir}")
    if(NOT EXISTS "${ipa_abs}")
        message(FATAL_ERROR
            "ISP tuning config not found at:\n  ${ipa_abs}\n"
            "esp_ipa would silently skip it and the camera would run with no "
            "auto-exposure and no white balance. Check the ESP_IPA_JSON_CONFIG_FILE_PATH "
            "block in the project CMakeLists.txt.")
    endif()
endforeach()
