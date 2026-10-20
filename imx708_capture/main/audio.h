/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microfone da placa: codec ES8311 (I2C 0x18, o mesmo barramento da câmera)
 * no I2S0, 16 kHz, 16 bits, estéreo. Esquerda = microfone; direita = a
 * referência do DAC (o que o alto-falante toca), que é o formato "MR" que o
 * AFE do ESP-SR espera para cancelar eco. Um quadro = 512 amostras (32 ms),
 * o mesmo tamanho que o WakeNet consome.
 *
 * Mede o nível de cada quadro (para o slider da página) e grava trechos do
 * microfone para a página de fala (asr.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float rms_db;       /* microfone, último quadro, em dBFS (0 = fundo de escala) */
    float peak_db;      /* pico do microfone, segurado ~1 s e caindo devagar */
    float ref_db;       /* canal da direita (referência do DAC), último quadro */
} audio_level_t;

/* bus = o barramento I2C onde está o ES8311. */
esp_err_t audio_start(i2c_master_bus_handle_t bus);
bool audio_ok(void);
void audio_get_level(audio_level_t *out);

/* Ganho do microfone no codec: 0..42 dB, em passos de 6 dB. */
esp_err_t audio_set_gain(int db);
int audio_gain(void);   /* -1 = microfone não iniciou */

/* Grava as próximas n amostras do microfone (mono, 16 kHz) em dst. Bloqueia
 * até completar (ESP_OK) ou timeout_ms (ESP_ERR_TIMEOUT). Uma por vez. */
esp_err_t audio_record(int16_t *dst, size_t n, uint32_t timeout_ms);
size_t audio_record_progress(void);     /* amostras já gravadas da gravação atual */

#ifdef __cplusplus
}
#endif
