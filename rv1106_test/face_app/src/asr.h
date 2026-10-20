/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Página /asr: mede reconhecimento de fala (whisper.cpp e Vosk) na placa.
 *
 * Os motores e os modelos ficam no asr_bench (scripts/asr.sh deploy):
 * engines.tsv dá o comando de cada motor e clips/ tem as frases (WAV de
 * 16 kHz com a transcrição num .txt do lado). Uma medição roda um processo
 * por motor, um motor por vez, com todas as frases escolhidas (o modelo
 * carrega uma vez). Cada linha JSON que o processo imprime vai para
 * results.jsonl com o nome do motor; a página calcula o resto (WER, médias).
 *
 *   GET  /asr                 a página (asr.html)
 *   GET  /asr/state           motores, frases, medição em andamento e resultados (JSON)
 *   GET  /asr/clip?name=X     o WAV de uma frase
 *   POST /asr/run?engines=a,b&clips=x,y&pause=1
 *                             começa uma medição; pause=1 para a IA de rostos
 *                             enquanto ela roda (a CPU fica só para a fala)
 *   POST /asr/stop            interrompe a medição
 *   POST /asr/upload?ref=T    corpo = WAV 16 kHz mono 16 bits; grava como rec-<data>
 *   POST /asr/delete?name=rec-...   apaga uma gravação
 *   POST /asr/clear           apaga os resultados
 */
#pragma once

#include <functional>
#include <string>

class HttpServer;

/* dir = pasta do asr_bench na placa. pause_ai(true) no começo de uma medição
 * que pediu a IA de rostos parada e pause_ai(false) no fim. */
void asr_init(const std::string &dir, std::function<void(bool)> pause_ai);
void asr_routes(HttpServer &server);
