idf_component_register(SRCS "comm.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES ao esp_driver_uart
                       PRIV_REQUIRES app proto esp_timer vfs)
