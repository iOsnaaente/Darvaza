/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Mede o whisper.cpp na placa: carrega o modelo uma vez e transcreve cada
 * WAV, uma linha JSON por arquivo (formato em bench.h). Decodificação gulosa,
 * sem timestamps: o mais rápido que o whisper.cpp faz.
 *
 *   asr_whisper -m ggml-tiny-q8_0.bin [-l pt] [-t 1] [--ac N|auto] [-v] a.wav b.wav ...
 *
 *   --ac N  janela de áudio do encoder, em quadros de 20 ms (padrão: 1500 =
 *           30 s, o tamanho com que o modelo foi treinado). O encoder
 *           processa a janela inteira mesmo com 5 s de fala: na placa ele é
 *           ~95% do tempo. Menor acelera quase na proporção, mas o modelo foi
 *           treinado com 30 s e pode errar mais.
 *   --ac auto  a janela de cada arquivo: a duração dele + AUTO_AC_MARGIN,
 *           arredondada para cima em múltiplos de 32
 *   -v      log do whisper.cpp na saída de erro
 *
 * Campos a mais na linha: encode_ms (encoder), decode_ms (decoder, todos os
 * tokens) e audio_ctx.
 */
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bench.h"
#include "whisper.h"

#define AC_PER_SECOND   50      /* quadros do encoder por segundo de áudio */
#define AUTO_AC_MARGIN  32      /* ~0,6 s a mais que a fala */

static bool s_verbose;

static void log_cb(enum ggml_log_level, const char *text, void *)
{
    if (s_verbose) {
        fputs(text, stderr);
    }
}

static void usage()
{
    fprintf(stderr, "uso: asr_whisper -m modelo.bin [-l pt] [-t threads] [--ac N|auto] [-v] arquivo.wav...\n");
}

int main(int argc, char **argv)
{
    const char *model = nullptr, *lang = "pt";
    int threads = 1, audio_ctx = 0;     /* -1 = auto */
    std::vector<const char *> files;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "-v") {
            s_verbose = true;
        } else if ((a == "-m" || a == "-l" || a == "-t" || a == "--ac") && v) {
            if (a == "-m") {
                model = v;
            } else if (a == "-l") {
                lang = v;
            } else if (a == "-t") {
                threads = atoi(v);
            } else {
                audio_ctx = strcmp(v, "auto") == 0 ? -1 : atoi(v);
            }
            i++;
        } else if (a[0] == '-') {
            usage();
            return 2;
        } else {
            files.push_back(argv[i]);
        }
    }
    if (!model || files.empty()) {
        usage();
        return 2;
    }
    whisper_log_set(log_cb, nullptr);
    const char *name = bench_basename(model);

    double t0 = bench_now_ms();
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = false;
    whisper_context *ctx = whisper_init_from_file_with_params(model, cparams);
    double load_ms = bench_now_ms() - t0;
    if (!ctx) {
        for (const char *f : files) {
            bench_print_error(name, f, "o modelo não carregou");
        }
        return 1;
    }

    whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wp.language = lang;
    wp.n_threads = threads;
    wp.translate = false;
    wp.no_timestamps = true;
    const int full_ctx = whisper_model_n_audio_ctx(ctx);
    wp.print_progress = false;
    wp.print_realtime = false;
    wp.print_timestamps = false;
    wp.print_special = false;

    for (const char *f : files) {
        int16_t *pcm = nullptr;
        size_t n = 0;
        if (const char *err = bench_read_wav(f, &pcm, &n)) {
            bench_print_error(name, f, err);
            continue;
        }
        std::vector<float> samples(n);
        for (size_t i = 0; i < n; i++) {
            samples[i] = pcm[i] / 32768.0f;
        }
        free(pcm);

        wp.audio_ctx = audio_ctx >= 0 ? audio_ctx
                                      : std::min(full_ctx, ((int)(n * AC_PER_SECOND / BENCH_RATE) + AUTO_AC_MARGIN + 31) / 32 * 32);
        whisper_reset_timings(ctx);
        double c0 = bench_cpu_ms(), p0 = bench_now_ms();
        int ret = whisper_full(ctx, wp, samples.data(), (int)samples.size());
        double proc_ms = bench_now_ms() - p0, cpu_ms = bench_cpu_ms() - c0;
        if (ret != 0) {
            bench_print_error(name, f, "whisper_full falhou");
            continue;
        }

        /* Cada segmento começa com espaço; o do começo sai. */
        std::string text;
        for (int i = 0; i < whisper_full_n_segments(ctx); i++) {
            text += whisper_full_get_segment_text(ctx, i);
        }
        text.erase(0, std::min(text.size(), text.find_first_not_of(' ')));
        const whisper_timings *t = whisper_get_timings(ctx);
        char extra[160];
        snprintf(extra, sizeof(extra), ",\"encode_ms\":%.0f,\"decode_ms\":%.0f,\"audio_ctx\":%d", t ? t->encode_ms : 0.0,
                 t ? t->decode_ms + t->batchd_ms + t->prompt_ms : 0.0, wp.audio_ctx ? wp.audio_ctx : full_ctx);
        bench_print_result(name, f, n, load_ms, proc_ms, cpu_ms, text.c_str(), extra);
    }

    whisper_free(ctx);
    return 0;
}
