/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Para onde a pessoa está olhando, sem modelo novo: só os 5 pontos do
 * detector de rosto do esp-dl e o brilho (Y) tirado do próprio quadro RGB565.
 * Mesmo algoritmo do rv1106_test/face_app (gaze.cpp de lá).
 *
 *   - cabeça: o nariz se desloca em relação aos olhos e à boca quando a
 *     cabeça gira (modelo 3D simples do rosto, ver gaze.cpp);
 *   - olhos: a pupila/íris é a parte mais escura de uma janelinha em volta
 *     de cada olho; o quanto ela sai do centro vira o giro do olho.
 *
 * Olhar = cabeça + olhos. Ângulos em graus, do ponto de vista da câmera:
 * yaw > 0 = para a direita da imagem, pitch > 0 = para cima. (0, 0) = olhando
 * para a câmera, depois de calibrar (face_gaze_calibrate, em face.h).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float head_yaw, head_pitch;   /* giro da cabeça */
    float eye_yaw, eye_pitch;     /* giro dos olhos em relação à cabeça */
    bool eyes;                    /* achou ao menos uma pupila neste quadro */
    int16_t pupil[4];             /* xe, ye, xd, yd em pixels do quadro; -1 = não achou */
} gaze_t;

/* Mede o olhar de um rosto pelos 5 pontos, na ordem olho esquerdo da imagem,
 * olho direito, nariz, canto esquerdo da boca, canto direito (face.cpp
 * converte da ordem do esp-dl). img = quadro RGB565 w x h. Sem calibração e
 * sem suavizar. Usa buffers estáticos: chamar só da task da IA. */
void gaze_measure(const uint8_t *img, int w, int h, const int kp[10], gaze_t *out);

/* Média móvel por rosto: acc vai em direção a m. first = rosto novo. Sem
 * pupila neste quadro, os olhos ficam como estavam. */
void gaze_smooth(gaze_t *acc, const gaze_t *m, bool first);

#ifdef __cplusplus
}
#endif
