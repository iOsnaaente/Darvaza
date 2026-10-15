idf_component_register(SRCS "display.cpp" "lcd.cpp" "render.cpp" "anim.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES ao
                       PRIV_REQUIRES app proto esp_lcd esp_driver_spi esp_driver_ledc esp_timer
                                     espressif__esp_lcd_gc9a01)
