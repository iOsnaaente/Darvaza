/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Detecção + reconhecimento facial (modelos do esp-dl) numa task própria, no
 * core 1, no ritmo que der. O vídeo não espera por ela: a captura oferece um
 * quadro quando a IA está livre e desenha o último resultado em cada quadro.
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

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_MAX_BOXES 4

typedef struct {
    int16_t x0, y0, x1, y1;   /* em pixels do quadro da câmera */
    int16_t id;               /* -1 = desconhecido */
    uint8_t sim_pct;          /* semelhança com o cadastro, 0-100 */
} face_box_t;

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
    uint32_t detect_us;       /* último ciclo: detecção */
    uint32_t recog_us;        /* último ciclo: reconhecimento de todos os rostos */
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

/* Último resultado, se tiver no máximo max_age_ms. Devolve quantas caixas. */
size_t face_get_boxes(face_box_t *out, size_t max, uint32_t max_age_ms);

/* Cadastra o maior rosto do próximo quadro com rosto. Bloqueia até timeout_ms. */
face_enroll_result_t face_enroll(uint32_t timeout_ms);

/* Apaga um cadastro; id < 0 apaga todos (e os IDs recomeçam em 1). */
esp_err_t face_delete(int id);

/* Preenche até max IDs cadastrados; devolve o total. */
size_t face_list(int *ids, size_t max);

void face_get_stats(face_stats_t *out);

#ifdef __cplusplus
}
#endif
