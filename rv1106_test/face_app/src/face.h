/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Detecção + reconhecimento facial numa thread própria, no ritmo que der (a
 * mesma lógica do imx708_capture do ESP32-P4). A captura não espera por ela:
 * oferece todo quadro, a IA analisa sempre o mais novo, e a captura usa
 * sempre o último resultado publicado.
 *
 * Cadastros: um vetor de características por pessoa, com ID estável, num
 * arquivo (por padrão /userdata/faces.bin, mesmo formato do P4). Sobrevive a
 * reboot; só some apagando pela web ou regravando a placa.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "camera.h"
#include "gaze.h"

#define FACE_MAX_BOXES 4

/* Um rosto. Terminal e web mostram o mesmo [ID, X, Y, dX, dY]:
 * X,Y = centro ((x0+x1)/2, (y0+y1)/2), dX,dY = tamanho (x1-x0, y1-y0). */
struct face_box_t {
    int16_t x0, y0, x1, y1;   /* em pixels do quadro da câmera */
    int16_t id;               /* -1 = desconhecido */
    uint8_t sim_pct;          /* semelhança com o cadastro, 0-100 */
    uint32_t track;           /* o mesmo enquanto este rosto é seguido */
    int64_t since_us;         /* quando ele apareceu (now_us) */
    int16_t kp[10];           /* os 5 pontos do RetinaFace (Det::kp), em pixels do quadro */
    gaze_t gaze;              /* cabeça e olhos, já com a calibração (gaze.h) */
    float yaw, pitch;         /* olhar = cabeça + olhos, em graus */
    bool look;                /* olhando para a câmera */
};

/* O que a IA achou num quadro. */
struct face_result_t {
    uint32_t seq;             /* muda a cada quadro analisado */
    int64_t frame_us;         /* captura do quadro analisado (now_us) */
    size_t n;
    face_box_t box[FACE_MAX_BOXES];
};

enum face_enroll_status_t {
    FACE_ENROLL_NEW,          /* cadastrado com ID novo */
    FACE_ENROLL_EXISTING,     /* já era cadastrado: devolve o ID dele */
    FACE_ENROLL_NO_FACE,      /* nenhum rosto apareceu no prazo */
    FACE_ENROLL_TOO_SMALL,    /* rosto longe demais para um bom cadastro */
    FACE_ENROLL_FAILED,       /* erro no modelo ou gravando o arquivo */
    FACE_ENROLL_BUSY,         /* outro cadastro em andamento */
};

struct face_enroll_result_t {
    face_enroll_status_t status;
    int id;
    uint8_t sim_pct;
};

struct face_stats_t {
    uint32_t cycles;          /* quadros analisados */
    uint32_t detect_us;       /* último quadro: detecção */
    uint32_t latency_us;      /* último quadro: da captura ao resultado publicado */
    uint32_t recogs;          /* reconhecimentos feitos */
    uint32_t recog_us;        /* último reconhecimento (um rosto) */
};

/* Devolve à captura um quadro que a IA terminou de analisar; as caixas
 * publicadas nesse momento são as dele. */
using face_release_fn = std::function<void(const FramePtr &frame)>;

/* Carrega modelos e cadastros e inicia a thread. thr = semelhança de cosseno
 * mínima para dizer "é o ID x". */
bool face_init(int width, int height, const std::string &model_dir, const std::string &db_path, float thr,
               face_release_fn release);

/* Chamada pela captura a cada quadro, SEM cópia: a IA fica com uma
 * referência (devolve true) e a devolve por release() quando terminar de
 * analisá-lo; se ela estava ocupada e chegou outro antes, este é descartado.
 * false = a IA não iniciou. */
bool face_offer_frame(const FramePtr &frame);

/* Último resultado publicado; sem caixas (n = 0) se tiver mais que max_age_ms. */
void face_get_result(face_result_t *out, uint32_t max_age_ms);

/* Cadastra o maior rosto do próximo quadro com rosto. Bloqueia até timeout_ms. */
face_enroll_result_t face_enroll(uint32_t timeout_ms);

/* Apaga um cadastro; id < 0 apaga todos (e os IDs recomeçam em 1).
 * 0 = ok, -1 = ID não cadastrado, -2 = falha gravando. */
int face_delete(int id);

/* Preenche até max IDs cadastrados; devolve o total. */
size_t face_list(int *ids, size_t max);

void face_get_stats(face_stats_t *out);

/* Calibra o olhar: o maior rosto na imagem, olhando para a câmera, passa a
 * ser yaw = pitch = 0 (cabeça e olhos). reset = volta para sem calibração.
 * false = nenhum rosto. Vale até o app reiniciar. */
bool face_gaze_calibrate(bool reset);

/* Correção atual, em graus: cabeça yaw, pitch; olhos yaw, pitch. */
void face_gaze_offsets(float out[4]);
