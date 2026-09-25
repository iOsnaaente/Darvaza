idf_component_register(SRCS "storage.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES ao
                       PRIV_REQUIRES app proto esp_partition)
