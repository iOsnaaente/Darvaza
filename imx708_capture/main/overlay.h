/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Desenho mínimo direto no quadro RGB565 (little-endian, como sai do ISP):
 * retângulos e texto com fonte 5x7. Tudo é recortado nas bordas do quadro.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OVERLAY_RGB565(r, g, b) ((uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | ((b) >> 3)))

typedef struct {
    uint16_t *px;
    int w;
    int h;
} overlay_fb_t;

/* Contorno de espessura thick, para dentro de (x0,y0)-(x1,y1) inclusive. */
void overlay_rect(const overlay_fb_t *fb, int x0, int y0, int x1, int y1, int thick, uint16_t color);

void overlay_fill(const overlay_fb_t *fb, int x0, int y0, int x1, int y1, uint16_t color);

/* Texto com o canto superior esquerdo em (x,y). Aceita A-Z, a-z (desenhado
 * como maiúscula), 0-9, espaço e  % - . : ?  - o resto vira espaço. */
void overlay_text(const overlay_fb_t *fb, int x, int y, const char *s, int scale, uint16_t color);

int overlay_text_width(const char *s, int scale);
int overlay_text_height(int scale);

#ifdef __cplusplus
}
#endif
