/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Para onde a pessoa está olhando, sem modelo novo: só os 5 pontos do
 * RetinaFace e o plano Y (cinza) do quadro, ~0,1 ms por rosto na CPU.
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

#include "camera.h"

struct gaze_t {
    float head_yaw, head_pitch;   /* giro da cabeça */
    float eye_yaw, eye_pitch;     /* giro dos olhos em relação à cabeça */
    bool eyes;                    /* achou ao menos uma pupila neste quadro */
    int pupil[4];                 /* xe, ye, xd, yd em pixels do quadro; -1 = não achou */
};

/* Mede o olhar de um rosto pelos 5 pontos (Det::kp: olho esquerdo da imagem,
 * olho direito, nariz, cantos da boca). Sem calibração e sem suavizar. */
void gaze_measure(const Frame &f, const int kp[10], gaze_t *out);

/* Média móvel por rosto: acc vai em direção a m. first = rosto novo. Sem
 * pupila neste quadro, os olhos ficam como estavam. */
void gaze_smooth(gaze_t &acc, const gaze_t &m, bool first);
