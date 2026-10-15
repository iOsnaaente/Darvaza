Cópia de parte de https://github.com/espressif/esp-who
(commit 1abda05e1c0782237fcb9e8d33a2fa7e105f06b2, branch master).

Só os componentes que não dependem de BSP/LCD: o resto do esp-who
(who_lcd, who_frame_lcd_disp, who_app) amarra a placa ao BSP da
ESP32-P4-Function-EV-Board, que trava o esp_video em 2.0 - e o driver da
IMX708 precisa de 2.4+. Para atualizar, copie de novo as mesmas pastas.

Única alteração: who_recognition/idf_component.yml pede
human_face_recognition "~0.3.2" em vez de "*". Com "*" o component manager
avalia as versões 0.1.x, que dependem de BSP + LVGL com if-clauses em
LV_USE_*, e o configure aborta com "Missing required kconfig option".
