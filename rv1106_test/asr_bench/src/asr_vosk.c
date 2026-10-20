/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mede o Vosk na placa: carrega o modelo uma vez e transcreve cada WAV, uma
 * linha JSON por arquivo (formato em bench.h). O áudio entra em pedaços de
 * 250 ms, como chegaria de um microfone; o texto é a soma das frases que o
 * Vosk fecha no caminho mais a final.
 *
 *   asr_vosk -m vosk-model-small-pt-0.3 [-g gramatica.json] [-v] a.wav b.wav ...
 *
 *   -g  só reconhece as frases da lista (JSON: ["liga a luz", ..., "[unk]"];
 *       "[unk]" = qualquer outra coisa). Para comandos: a busca fica
 *       pequena e o resto do português não compete com eles.
 *
 * Compilado contra a glibc (a libvosk.so pronta é glibc) e rodado pelo loader
 * da glibc que vai junto: ver vosk.sh e ../README.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"
#include "vosk_api.h"

#define CHUNK_SAMPLES (BENCH_RATE / 4)

/* O arquivo inteiro numa string (do chamador), ou NULL. */
static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = (char *)malloc(size > 0 ? (size_t)size + 1 : 1);
    size_t n = size > 0 ? fread(s, 1, (size_t)size, f) : 0;
    s[n] = 0;
    fclose(f);
    return s;
}

/* Acrescenta a "text" de um resultado do Vosk ({"text" : "..."}) em out. */
static void append_text(const char *json, char **out, size_t *len)
{
    const char *p = strstr(json, "\"text\"");
    if (!p || !(p = strchr(p + 6, ':')) || !(p = strchr(p, '"'))) {
        return;
    }
    p++;
    const char *end = p;
    while (*end && *end != '"') {
        end += end[0] == '\\' && end[1] ? 2 : 1;
    }
    size_t n = (size_t)(end - p);
    if (n == 0) {
        return;
    }
    *out = (char *)realloc(*out, *len + n + 2);
    if (*len) {
        (*out)[(*len)++] = ' ';
    }
    memcpy(*out + *len, p, n);
    *len += n;
    (*out)[*len] = 0;
}

int main(int argc, char **argv)
{
    const char *model_dir = NULL, *grammar_path = NULL;
    const char **files = (const char **)calloc((size_t)argc, sizeof(*files));
    int verbose = 0, nfiles = 0, bad = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            model_dir = argv[++i];
        } else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            grammar_path = argv[++i];
        } else if (argv[i][0] == '-') {
            bad = 1;
        } else {
            files[nfiles++] = argv[i];
        }
    }
    if (bad || !model_dir || nfiles == 0) {
        fprintf(stderr, "uso: asr_vosk -m pasta_do_modelo [-g gramatica.json] [-v] arquivo.wav...\n");
        return 2;
    }
    vosk_set_log_level(verbose ? 0 : -1);
    const char *name = bench_basename(model_dir);

    double t0 = bench_now_ms();
    VoskModel *model = vosk_model_new(model_dir);
    char *grammar = grammar_path ? read_text(grammar_path) : NULL;
    if (grammar_path && !grammar) {
        fprintf(stderr, "asr_vosk: não li a gramática %s\n", grammar_path);
        return 2;
    }
    VoskRecognizer *rec = !model ? NULL
                          : grammar ? vosk_recognizer_new_grm(model, BENCH_RATE, grammar)
                                    : vosk_recognizer_new(model, BENCH_RATE);
    double load_ms = bench_now_ms() - t0;

    for (int i = 0; i < nfiles; i++) {
        const char *f = files[i];
        if (!rec) {
            bench_print_error(name, f, "o modelo não carregou");
            continue;
        }
        int16_t *pcm = NULL;
        size_t n = 0;
        const char *err = bench_read_wav(f, &pcm, &n);
        if (err) {
            bench_print_error(name, f, err);
            continue;
        }

        char *text = NULL;
        size_t len = 0;
        double c0 = bench_cpu_ms(), p0 = bench_now_ms();
        for (size_t pos = 0; pos < n; pos += CHUNK_SAMPLES) {
            int k = (int)(n - pos < CHUNK_SAMPLES ? n - pos : CHUNK_SAMPLES);
            if (vosk_recognizer_accept_waveform_s(rec, pcm + pos, k) == 1) {
                append_text(vosk_recognizer_result(rec), &text, &len);
            }
        }
        append_text(vosk_recognizer_final_result(rec), &text, &len);
        double proc_ms = bench_now_ms() - p0, cpu_ms = bench_cpu_ms() - c0;
        vosk_recognizer_reset(rec);

        bench_print_result(name, f, n, load_ms, proc_ms, cpu_ms, text ? text : "", "");
        free(text);
        free(pcm);
    }

    if (rec) {
        vosk_recognizer_free(rec);
    }
    if (model) {
        vosk_model_free(model);
    }
    free(grammar);
    free(files);
    return 0;
}
