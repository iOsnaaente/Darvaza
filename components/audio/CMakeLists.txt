idf_component_register(SRCS "audio.cpp" "ssl.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES ao
                       PRIV_REQUIRES app comm proto esp_driver_i2s esp_driver_gpio esp_timer)
