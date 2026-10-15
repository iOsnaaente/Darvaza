/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Os dois modelos dos exemplos da Luckfox (convertidos com o RKNN-Toolkit2
 * 1.6.0 a partir do facenet-retinaface do bubbliiiing):
 *   RetinaFace (mobilenet 0.25, 640x640, entrada BGR) - caixa + 5 pontos;
 *   MobileFaceNet/FaceNet (160x160, entrada RGB) - vetor de 128 posições.
 */
#pragma once

#include <array>
#include <string>
#include <vector>

#include <opencv2/core.hpp>

#include "camera.h"
#include "rknn_model.h"

/* Um rosto detectado, em pixels do quadro. kp = 5 pontos (x, y): olho da
 * esquerda da imagem, olho da direita, nariz, canto esquerdo e direito da boca. */
struct Det {
    int box[4];   /* x0, y0, x1, y1 */
    float score;
    int kp[10];
    int area() const { return (box[2] - box[0]) * (box[3] - box[1]); }
};

class RetinaFace {
public:
    bool load(const std::string &path);
    /* Rostos do quadro, do maior para o menor, no máximo max_faces. O RGA
     * reduz e converte o quadro direto na entrada da NPU (sem CPU). */
    bool detect(const Frame &f, std::vector<Det> &out, size_t max_faces);

private:
    RknnModel m_;
    std::vector<std::array<float, 4>> priors_;   /* cx, cy, w, h (0..1) */
    int frame_w_ = 0, frame_h_ = 0;
    float scale_ = 1;                             /* quadro -> entrada do modelo */
    int pad_x_ = 0, pad_y_ = 0;
};

class FaceNet {
public:
    bool load(const std::string &path);
    int feat_len() const { return feat_len_; }
    /* Vetor normalizado (norma 1) do rosto d: recorta a caixa, gira para os
     * olhos ficarem na horizontal e encaixa em 160x160 com borda cinza, como
     * no treino do modelo. */
    bool extract(const Frame &f, const Det &d, float *out);

private:
    RknnModel m_;
    int feat_len_ = 0;
};
