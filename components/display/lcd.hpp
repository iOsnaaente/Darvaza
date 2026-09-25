#pragma once
// Hardware dos olhos: 2x GC9A01 (SPI2 e SPI3, DMA em paralelo) e backlight
#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_lcd_panel_ops.h"

namespace display {

struct Eye {
    esp_lcd_panel_handle_t panel     = nullptr;
    SemaphoreHandle_t      free_bufs = nullptr;   // buffers livres (0..2)
    uint16_t              *buf[2]    = {nullptr, nullptr};
    int                    next      = 0;
};

// isr_core: core da task que desenha (a ISR do SPI tem que ficar no mesmo)
void lcd_init_all(Eye (&eyes)[2], BaseType_t isr_core);

void backlight_init();   // começa apagado
void backlight_set(int percent);

}  // namespace display
