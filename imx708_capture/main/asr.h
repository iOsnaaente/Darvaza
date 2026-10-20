/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Página /asr ("Fala"): mede reconhecimento de fala do ESP-SR na placa, como
 * o asr_bench da Luckfox mede o whisper.cpp e o Vosk. O Whisper não cabe na
 * P4 (o menor modelo tem 43 MB; a flash tem 16): os motores aqui são o
 * WakeNet10 "Olá ESP" (wake word em português) e o MultiNet7 (comandos, só
 * inglês), direto no áudio ou passando pelo AFE.
 *
 * As frases ficam na FAT "clips" (/clips): <nome>.wav (16 kHz, mono, 16 bits)
 * com a transcrição de referência em <nome>.txt. Gravadas pelo microfone da
 * placa (mic-NNN) ou enviadas pela página (wav-NNN). Uma medição roda um
 * motor por vez, carregando o modelo uma vez e passando por todas as frases;
 * cada frase vira uma linha JSON em /clips/results.txt e a página calcula o
 * resto (acerto, médias).
 *
 *   GET  /asr                 a página (asr.html)
 *   GET  /asr/state           motores, frases, comandos, medição, gravação e resultados (JSON)
 *   GET  /asr/clip?name=X     o WAV de uma frase
 *   POST /asr/run             corpo engines=a,b&clips=x,y&pause=1; pause=1 para
 *                             a IA de rostos enquanto mede (o core 1 fica livre)
 *   POST /asr/stop            interrompe a medição (entre uma frase e outra)
 *   POST /asr/record          corpo secs=N&ref=T: grava N s pelo microfone da placa
 *   POST /asr/upload?ref=T    corpo = WAV 16 kHz mono 16 bits (cabeçalho de 44 bytes)
 *   POST /asr/delete?name=X   apaga uma frase (mic-... ou wav-...)
 *   POST /asr/clear           apaga os resultados
 *   POST /asr/commands        corpo = os comandos do MultiNet, um por linha
 */
#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Monta /clips, abre a partição de modelos e cria a task de medição. */
esp_err_t asr_init(void);

/* Registra as rotas acima (10 handlers). */
void asr_register(httpd_handle_t server);

#ifdef __cplusplus
}
#endif
