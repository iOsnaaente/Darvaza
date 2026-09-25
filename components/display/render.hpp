#pragma once
// Render procedural de um olho, em faixas (sem dependência do ESP-IDF)
#include <cstddef>
#include <cstdint>

namespace display {

constexpr int    LCD_W      = 240;
constexpr int    LCD_H      = 240;
constexpr int    BAND_LINES = 40;
constexpr int    N_BANDS    = LCD_H / BAND_LINES;
constexpr size_t BAND_BYTES = LCD_W * BAND_LINES * sizeof(uint16_t);

struct EyeState {
    int gx = 0, gy = 0;   // deslocamento do olhar (px)
    int iris_r  = 62;
    int pupil_r = 26;
    int lid_top = 20;     // borda da pálpebra superior
    int lid_bot = 220;    // borda da pálpebra inferior
};

// Preenche BAND_LINES linhas a partir de y0 (RGB565 big-endian, como o GC9A01 espera)
void render_band(uint16_t *dst, int y0, const EyeState &s);

}  // namespace display
