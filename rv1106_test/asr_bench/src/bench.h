/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * O que os dois programas de medição (asr_whisper e asr_vosk) têm em comum:
 * leitura do WAV, relógios e a linha JSON de cada arquivo. Em C, porque o
 * asr_vosk é C (é compilado contra a glibc; ver ../README.md).
 *
 * Uma linha por arquivo, na saída padrão:
 *   {"model":"...","file":"...","audio_ms":N,"load_ms":N,"proc_ms":N,
 *    "cpu_ms":N,"rtf":N,"maxrss_kb":N,"text":"...", <extras do motor>}
 * ou, se o arquivo não deu:
 *   {"model":"...","file":"...","error":"..."}
 *
 *   load_ms   carregar o modelo (uma vez por processo; repetido em cada linha)
 *   proc_ms   relógio de parede para transcrever este arquivo
 *   cpu_ms    CPU (usuário + sistema) gasta neste arquivo; bem menor que
 *             proc_ms = o processo esperou CPU (outro programa usando)
 *   rtf       proc_ms / audio_ms: < 1 acompanha a fala em tempo real
 *   maxrss_kb pico de memória do processo até aqui
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#define BENCH_RATE 16000

static inline double bench_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static inline double bench_cpu_ms(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1e3 + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1e3;
}

static inline long bench_maxrss_kb(void)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return ru.ru_maxrss;
}

static inline uint32_t le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static inline uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/*
 * Lê um WAV PCM de 16 bits e 16 kHz (o que os dois modelos esperam), mono ou
 * estéreo (vira mono pela média). *pcm é do chamador (free). Devolve NULL se
 * deu certo, ou a mensagem de erro.
 */
static inline const char *bench_read_wav(const char *path, int16_t **pcm, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return "não abriu o arquivo";
    }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)malloc(size > 0 ? (size_t)size : 1);
    if (!buf || size < 12 || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        free(buf);
        return "não leu o arquivo";
    }
    fclose(f);
    if (memcmp(buf, "RIFF", 4) != 0 || memcmp(buf + 8, "WAVE", 4) != 0) {
        free(buf);
        return "não é WAV";
    }

    unsigned channels = 0, rate = 0, bits = 0, format = 0;
    const uint8_t *data = NULL;
    size_t data_len = 0;
    for (size_t pos = 12; pos + 8 <= (size_t)size;) {
        uint32_t len = le32(buf + pos + 4);
        const uint8_t *body = buf + pos + 8;
        size_t avail = (size_t)size - pos - 8;
        if (memcmp(buf + pos, "fmt ", 4) == 0 && len >= 16 && avail >= 16) {
            format = le16(body);
            channels = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
            if (format == 0xFFFE && len >= 26 && avail >= 26) {
                format = le16(body + 24);   /* WAVE_FORMAT_EXTENSIBLE: o formato real vem no GUID */
            }
        } else if (memcmp(buf + pos, "data", 4) == 0) {
            data = body;
            data_len = len < avail ? len : avail;   /* gravações cortadas: usa o que tem */
            break;
        }
        pos += 8 + (size_t)len + (len & 1);
    }

    const char *err = NULL;
    if (!data) {
        err = "WAV sem dados";
    } else if (format != 1 || bits != 16) {
        err = "o WAV precisa ser PCM de 16 bits";
    } else if (rate != BENCH_RATE) {
        err = "o WAV precisa ser de 16000 Hz";
    } else if (channels != 1 && channels != 2) {
        err = "o WAV precisa ser mono ou estéreo";
    }
    if (err) {
        free(buf);
        return err;
    }

    *n = data_len / (2 * channels);
    *pcm = (int16_t *)malloc(*n * sizeof(int16_t) + 1);
    for (size_t i = 0; i < *n; i++) {
        const uint8_t *s = data + i * 2 * channels;
        int v = (int16_t)le16(s);
        if (channels == 2) {
            v = (v + (int16_t)le16(s + 2)) / 2;
        }
        (*pcm)[i] = (int16_t)v;
    }
    free(buf);
    return NULL;
}

/* Só o nome do arquivo, sem a pasta. */
static inline const char *bench_basename(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static inline void bench_json_str(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            printf("\\%c", c);
        } else if (c == '\n') {
            fputs("\\n", stdout);
        } else if (c < 0x20) {
            printf("\\u%04x", c);
        } else {
            putchar(c);
        }
    }
    putchar('"');
}

static inline void bench_print_error(const char *model, const char *file, const char *err)
{
    fputs("{\"model\":", stdout);
    bench_json_str(model);
    fputs(",\"file\":", stdout);
    bench_json_str(bench_basename(file));
    fputs(",\"error\":", stdout);
    bench_json_str(err);
    puts("}");
    fflush(stdout);
}

/* extra: campos a mais do motor, já em JSON e começando com ",", ou "". */
static inline void bench_print_result(const char *model, const char *file, size_t samples, double load_ms,
                                      double proc_ms, double cpu_ms, const char *text, const char *extra)
{
    double audio_ms = samples * 1000.0 / BENCH_RATE;
    fputs("{\"model\":", stdout);
    bench_json_str(model);
    fputs(",\"file\":", stdout);
    bench_json_str(bench_basename(file));
    printf(",\"audio_ms\":%.0f,\"load_ms\":%.0f,\"proc_ms\":%.0f,\"cpu_ms\":%.0f,\"rtf\":%.3f,\"maxrss_kb\":%ld,\"text\":",
           audio_ms, load_ms, proc_ms, cpu_ms, audio_ms > 0 ? proc_ms / audio_ms : 0.0, bench_maxrss_kb());
    bench_json_str(text);
    fputs(extra, stdout);
    puts("}");
    fflush(stdout);
}
