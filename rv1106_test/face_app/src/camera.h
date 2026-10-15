/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Câmera pelo RKMPI: ISP + 3A (rkaiq) e um canal VI em NV12 com vários
 * buffers. (A captura do OpenCV-mobile usa um buffer só e o segura durante a
 * conversão de cor: em 1280x960 perde metade dos quadros.)
 *
 * Cada quadro é o próprio buffer da câmera, sem cópia; ele volta para o VI
 * quando a última referência (FramePtr) some.
 */
#pragma once

#include <cstdint>
#include <memory>

#include <opencv2/core.hpp>

struct Frame {
    int w, h;          /* pixels */
    int stride;        /* bytes por linha do Y e do UV */
    int vstride;       /* linhas do plano Y na memória */
    uint8_t *y;        /* NV12: Y, depois UV intercalado com metade da resolução */
    uint8_t *uv;
    int fd;            /* dma-buf, para o RGA */
    int64_t t_us;      /* chegada do quadro (now_us) */
    void *priv;        /* VIDEO_FRAME_INFO_S do RKMPI */
};
using FramePtr = std::shared_ptr<const Frame>;

/* Inicia ISP e VI. power_hz = frequência da rede elétrica (antiflicker): 60
 * no Brasil. fps > 0 fixa a taxa (o AE compensa com ganho); 0 = automática.
 * Devolve false se o sensor não respondeu. */
bool camera_open(int width, int height, int power_hz, int fps);

/* Próximo quadro; nullptr se não veio em timeout_ms. */
FramePtr camera_get(int timeout_ms);

void camera_close();

/* r com início e fim em pixel par (o UV do NV12 tem metade da resolução),
 * cortado no quadro. */
cv::Rect frame_even_rect(const Frame &f, cv::Rect r);

/* Região r do quadro (já passada por frame_even_rect) em BGR, reduzida para
 * out (vazio = mesmo tamanho; a redução pula pixels). */
cv::Mat frame_to_bgr(const Frame &f, const cv::Rect &r, cv::Size out = cv::Size());
