idf_component_register(SRCS "app.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES ao proto esp_driver_gpio
                       PRIV_REQUIRES nvs_flash)
