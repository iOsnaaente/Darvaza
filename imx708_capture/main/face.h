/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Detecção + reconhecimento facial (modelos do esp-dl) numa task própria, no
 * core 1, no ritmo que der. A captura não espera por ela: oferece um quadro
 * quando a IA está livre e usa sempre o último resultado publicado.
 *
 * Cadastros: um vetor de características por pessoa, com ID estável, em
 * /spiflash/faces.bin (FAT na partição "storage"). Sobrevive a reboot e a
 * reflash do app; só some com erase-flash ou apagando pela web.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "gaze.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_MAX_BOXES 4

/* Um rosto. Terminal e web mostram o mesmo [ID, X, Y, dX, dY]:
 * X,Y = centro ((x0+x1)/2, (y0+y1)/2), dX,dY = tamanho (x1-x0, y1-y0). */
typedef struct {
    int16_t x0, y0, x1, y1;   /* em pixels do quadro da câmera */
    int16_t id;               /* -1 = desconhecido */
    uint8_t sim_pct;          /* semelhança com o cadastro, 0-100 */
    uint32_t track;           /* o mesmo enquanto este rosto é seguido */
    int64_t since_us;         /* quando ele apareceu (esp_timer) */
    int16_t kp[10];           /* os 5 pontos do rosto (ordem do gaze.h), em pixels do quadro */
    gaze_t gaze;              /* cabeça e olhos, já com a calibração (gaze.h) */
    float yaw, pitch;         /* olhar = cabeça + olhos, em graus */
    bool look;                /* olhando para a câmera */
} face_box_t;

/* O que a IA achou num quadro. */
typedef struct {
    uint32_t seq;             /* muda a cada quadro analisado */
    int64_t frame_us;         /* captura do quadro analisado (esp_timer) */
    int16_t luma;             /* brilho (0-255, como sai na imagem) do miolo do maior
                               * rosto detectado neste quadro; -1 = nenhum */
    size_t n;
    face_box_t box[FACE_MAX_BOXES];
} face_result_t;

typedef enum {
    FACE_ENROLL_NEW,          /* cadastrado com ID novo */
    FACE_ENROLL_EXISTING,     /* já era cadastrado: devolve o ID dele */
    FACE_ENROLL_NO_FACE,      /* nenhum rosto apareceu no prazo */
    FACE_ENROLL_TOO_SMALL,    /* rosto longe demais para um bom cadastro */
    FACE_ENROLL_FAILED,       /* erro no modelo ou gravando na flash */
    FACE_ENROLL_BUSY,         /* outro cadastro em andamento */
} face_enroll_status_t;

typedef struct {
    face_enroll_status_t status;
    int id;
    uint8_t sim_pct;
} face_enroll_result_t;

typedef struct {
    uint32_t cycles;          /* quadros analisados */
    uint32_t detect_us;       /* último quadro: detecção */
    uint32_t latency_us;      /* último quadro: da captura ao resultado publicado */
    uint32_t recogs;          /* reconhecimentos feitos */
    uint32_t recog_us;        /* último reconhecimento (um rosto) */
} face_stats_t;

/* Devolve à captura um buffer que a IA terminou de usar (chamada nas tasks da
 * IA). fresh = o quadro acabou de ser analisado e as caixas publicadas são as
 * dele; false = ficou com o reconhecimento e já é velho (não serve para a web). */
typedef void (*face_release_fn)(int buf_index, bool fresh);

/* Monta a FAT, carrega modelos e cadastros e inicia a task. */
esp_err_t face_init(int width, int height, face_release_fn release);

/* Chamada pela captura a cada quadro (RGB565, width*height*2 bytes), SEM
 * cópia: se a IA estiver livre ela fica com o buffer (devolve true) e o
 * devolve por release(buf_index) quando terminar de usá-lo; se estiver
 * ocupada devolve false e o buffer continua com quem chamou. */
bool face_offer_frame(uint8_t *rgb565, int buf_index);

/* Pausa a IA (face_offer_frame passa a recusar todo quadro) e retoma. */
void face_pause(bool paused);
bool face_paused(void);

/* Último resultado publicado; sem caixas (n = 0, luma = -1) se tiver mais que max_age_ms. */
void face_get_result(face_result_t *out, uint32_t max_age_ms);

/* Cadastra o maior rosto do próximo quadro com rosto. Bloqueia até timeout_ms. */
face_enroll_result_t face_enroll(uint32_t timeout_ms);

/* Apaga um cadastro; id < 0 apaga todos (e os IDs recomeçam em 1). */
esp_err_t face_delete(int id);

/* Preenche até max IDs cadastrados; devolve o total. */
size_t face_list(int *ids, size_t max);

void face_get_stats(face_stats_t *out);

/* Calibra o olhar: o maior rosto na imagem, olhando para a câmera, passa a
 * ser yaw = pitch = 0 (cabeça e olhos). reset = volta para sem calibração.
 * false = nenhum rosto. Vale até reiniciar a placa. */
bool face_gaze_calibrate(bool reset);

/* Correção atual, em graus: cabeça yaw, pitch; olhos yaw, pitch. */
void face_gaze_offsets(float out[4]);

#ifdef __cplusplus
}
#endif
