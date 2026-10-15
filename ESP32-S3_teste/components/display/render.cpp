#include "render.hpp"

namespace display {
namespace {

// Cores RGB565 já com byte swap (GC9A01 espera big-endian)
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t sw(uint16_t c) { return static_cast<uint16_t>((c >> 8) | (c << 8)); }

constexpr uint16_t C_SKIN      = sw(rgb565(20, 20, 28));
constexpr uint16_t C_SCLERA    = sw(rgb565(235, 235, 240));
constexpr uint16_t C_IRIS      = sw(rgb565(30, 140, 200));
constexpr uint16_t C_IRIS_EDGE = sw(rgb565(10, 60, 110));
constexpr uint16_t C_PUPIL     = sw(rgb565(5, 5, 10));
constexpr uint16_t C_HIGHLIGHT = sw(rgb565(255, 255, 255));

}  // namespace

void render_band(uint16_t *dst, int y0, const EyeState &s)
{
    const int cx       = LCD_W / 2 + s.gx;
    const int cy       = LCD_H / 2 + s.gy;
    const int iris2    = s.iris_r * s.iris_r;
    const int iris_in2 = (s.iris_r - 6) * (s.iris_r - 6);
    const int pupil2   = s.pupil_r * s.pupil_r;
    const int hl_cx    = cx - s.iris_r / 3;
    const int hl_cy    = cy - s.iris_r / 3;
    constexpr int HL2  = 10 * 10;

    for (int y = y0; y < y0 + BAND_LINES; ++y) {
        uint16_t *row = dst + (y - y0) * LCD_W;

        if (y < s.lid_top || y > s.lid_bot) {
            for (int x = 0; x < LCD_W; ++x) row[x] = C_SKIN;
            continue;
        }

        const int dy2 = (y - cy) * (y - cy);
        const int hy2 = (y - hl_cy) * (y - hl_cy);

        for (int x = 0; x < LCD_W; ++x) {
            const int dx = x - cx;
            const int d2 = dx * dx + dy2;
            uint16_t c;
            if (d2 < pupil2)        c = C_PUPIL;
            else if (d2 < iris_in2) c = C_IRIS;
            else if (d2 < iris2)    c = C_IRIS_EDGE;
            else                    c = C_SCLERA;

            const int hx = x - hl_cx;
            if (hx * hx + hy2 < HL2) c = C_HIGHLIGHT;
            row[x] = c;
        }
    }
}

}  // namespace display
