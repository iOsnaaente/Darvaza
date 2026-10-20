/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Página Fala (ver asr.h).
 *
 * Uma task só ("asr") mede e grava as frases do microfone: as duas usam a
 * CPU e escrevem na flash, e nunca rodam juntas. Os handlers HTTP só validam,
 * deixam o pedido em s_req e acordam a task; o estado (s_job, s_rec) fica sob
 * s_lock.
 *
 * Como cada motor é medido, por frase:
 *   load_ms   criar o modelo (uma vez por motor; repetido em cada linha)
 *   proc_ms   relógio de parede para passar a frase inteira pelo motor
 *   rtf       proc_ms / fed_ms: < 1 acompanha a fala em tempo real
 *   fed_ms    áudio que o motor processou: a frase e o silêncio do fim que
 *             ele chegou a usar (o MultiNet para assim que decide)
 *   ram_kb    pico de memória (PSRAM + interna) desde antes de carregar o
 *             modelo; ram_int_kb só a interna. É a memória livre do sistema
 *             inteiro: outra task alocando ao mesmo tempo também conta.
 * Depois da frase vão mais 0,5 s (WakeNet) ou 1,5 s (MultiNet) de silêncio,
 * para o modelo terminar de decidir: entra no fed_ms, não no audio_ms.
 */
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "model_path.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "esp_mn_iface.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "audio.h"
#include "face.h"
#include "asr.h"

static const char *TAG = "asr";

#define CLIPS_MOUNT     "/clips"
#define CLIPS_PART      "clips"
#define MODEL_PART      "model"
#define RESULTS_PATH    CLIPS_MOUNT "/results.txt"
#define COMMANDS_PATH   CLIPS_MOUNT "/commands.txt"

#define RATE            16000
#define WAV_HEADER      44
#define MAX_CLIP_S      30
#define MAX_REC_S       15
#define NAME_LEN        8       /* a FAT está sem nomes longos (CONFIG_FATFS_LFN_NONE): nomes 8.3 */
#define MAX_CLIPS       64
#define REF_CAP         160
#define CMD_LINE_CAP    128
#define MAX_COMMANDS    200

#define MN_TIMEOUT_MS   6000    /* MultiNet: sem comando em 6 s = timeout */
#define WN_TAIL_MS      500
#define MN_TAIL_MS      1500
#define AFE_MAX_LAG     4       /* quadros alimentados ao AFE antes de esperar a saída */
#define AFE_WAIT_MS     300

#define ASR_TASK_STACK  12288
#define ASR_TASK_PRIO   3       /* = reconhecimento de rostos; abaixo da IA (4) e da captura (6) */
#define CHUNK_CAP       4096    /* buffer das respostas em pedaços */
#define LINE_CAP        1024    /* uma linha do results.txt */
#define BODY_CAP        2048    /* corpo de /asr/run e /asr/record */
#define COMMANDS_BODY   8192

/* Comandos de fábrica do MultiNet (inglês, em maiúsculas como no
 * commands_en.txt do esp-sr). A página edita /clips/commands.txt. */
static const char *const DEFAULT_COMMANDS[] = {
    "TURN ON THE LIGHT",
    "TURN OFF THE LIGHT",
    "LOOK AT ME",
    "PLAY SOME MUSIC",
    "INCREASE THE VOLUME",
    "DECREASE THE VOLUME",
    "WHAT TIME IS IT",
    "STOP",
};

/* ---- Motores ----------------------------------------------------------------- */

typedef enum { ENG_WN, ENG_MN, ENG_AFE_WN, ENG_AFE_MN } eng_type_t;

typedef struct {
    const char *name;
    const char *kind;       /* "wake" ou "cmd": como a página pontua */
    eng_type_t type;
    det_mode_t mode;        /* do WakeNet */
    const char *desc;
} engine_t;

static const engine_t ENGINES[] = {
    { "wakenet-olaesp",     "wake", ENG_WN,     DET_MODE_90,
      "WakeNet10 \"Olá ESP\", modo normal, direto no áudio" },
    { "wakenet-olaesp-95",  "wake", ENG_WN,     DET_MODE_95,
      "o mesmo no modo agressivo: detecta mais, e também dá mais alarme falso" },
    { "afe-wakenet-olaesp", "wake", ENG_AFE_WN, DET_MODE_90,
      "WakeNet10 dentro do AFE (com o VAD), como numa aplicação de verdade" },
    { "multinet-en",        "cmd",  ENG_MN,     DET_MODE_90,
      "MultiNet7 inglês, só os comandos da lista, direto no áudio" },
    { "afe-multinet-en",    "cmd",  ENG_AFE_MN, DET_MODE_90,
      "o mesmo, com o áudio passando antes pelo AFE (o log mostra os estágios)" },
};
#define ENGINE_COUNT (sizeof(ENGINES) / sizeof(ENGINES[0]))

static bool engine_is_wake(const engine_t *e)
{
    return e->type == ENG_WN || e->type == ENG_AFE_WN;
}

static bool engine_uses_afe(const engine_t *e)
{
    return e->type == ENG_AFE_WN || e->type == ENG_AFE_MN;
}

/* ---- Estado ------------------------------------------------------------------ */

static bool s_ready;                /* /clips montado */
static srmodel_list_t *s_models;
static char *s_wn_name, *s_mn_name;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_lock;
static long s_run_seq;              /* número da última medição (vem do results.txt) */

typedef enum { REQ_NONE, REQ_RUN, REQ_REC } req_type_t;

/* O pedido para a task: escrito pelo httpd com a task parada (s_job/s_rec
 * marcados ocupados sob s_lock), lido por ela depois do notify. */
static struct {
    req_type_t type;
    unsigned engines;               /* bit i = ENGINES[i] */
    int nclips;
    char clips[MAX_CLIPS][NAME_LEN + 1];
    bool pause;
    int secs;
    char ref[REF_CAP];
} s_req;

static struct {
    bool running, stop, pause_ai;
    char engine[24];
    char clip[NAME_LEN + 1];
    int done, total;
    int64_t started_us, ended_us;
    char error[112];
} s_job;

static struct {
    bool active;
    int secs;
    int64_t started_us;
    char last[NAME_LEN + 1];
    char error[64];
} s_rec;

/* Frases que o MultiNet recusou na última carga dos comandos. */
static char s_cmd_rejected[256];

static bool busy_locked(void)
{
    return s_job.running || s_rec.active;
}

/* ---- Texto e JSON ------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t cap, used;
} jb_t;

static void jput(jb_t *j, const char *fmt, ...)
{
    if (j->used >= j->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(j->buf + j->used, j->cap - j->used, fmt, ap);
    va_end(ap);
    j->used += n > 0 ? (size_t)n : 0;
}

static void jstr(jb_t *j, const char *s)
{
    jput(j, "\"");
    for (; s && *s && j->used < j->cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            jput(j, "\\%c", c);
        } else if (c < 0x20) {
            jput(j, "\\u%04x", c);
        } else {
            jput(j, "%c", c);
        }
    }
    jput(j, "\"");
}

static void trim(char *s)
{
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) {
        s[--n] = '\0';
    }
    size_t i = 0;
    while (s[i] && isspace((unsigned char)s[i])) {
        i++;
    }
    memmove(s, s + i, n - i + 1);
}

static void lower_copy(char *dst, const char *src, size_t cap)
{
    size_t i = 0;
    for (; src && src[i] && i + 1 < cap; i++) {
        dst[i] = (char)tolower((unsigned char)src[i]);
    }
    dst[i] = '\0';
}

static void url_decode(char *s)
{
    char *o = s;
    for (; *s; s++) {
        if (*s == '+') {
            *o++ = ' ';
        } else if (*s == '%' && isxdigit((unsigned char)s[1]) && isxdigit((unsigned char)s[2])) {
            char h[3] = { s[1], s[2], '\0' };
            *o++ = (char)strtol(h, NULL, 16);
            s += 2;
        } else {
            *o++ = *s;
        }
    }
    *o = '\0';
}

/* Referência numa linha só, sem caracteres de controle. */
static void clean_ref(char *s)
{
    for (char *p = s; *p; p++) {
        if ((unsigned char)*p < 0x20) {
            *p = ' ';
        }
    }
    trim(s);
}

/* ---- Arquivos ---------------------------------------------------------------- */

/* Nomes viram caminhos: só letras, dígitos, "-" e "_", até 8 (8.3). */
static bool valid_name(const char *s)
{
    size_t n = strlen(s);
    if (n == 0 || n > NAME_LEN) {
        return false;
    }
    for (; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_') {
            return false;
        }
    }
    return true;
}

static void clip_path(char *buf, size_t cap, const char *name, const char *ext)
{
    snprintf(buf, cap, CLIPS_MOUNT "/%s.%s", name, ext);
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* As frases (os .wav de /clips, sem o .wav, em minúsculas), em ordem. */
static int list_clips(char names[][NAME_LEN + 1], int max)
{
    DIR *d = opendir(CLIPS_MOUNT);
    if (!d) {
        return 0;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        char low[16];
        lower_copy(low, e->d_name, sizeof(low));
        char *dot = strrchr(low, '.');
        if (!dot || strcmp(dot, ".wav") != 0) {
            continue;
        }
        *dot = '\0';
        if (valid_name(low)) {
            strcpy(names[n++], low);
        }
    }
    closedir(d);
    qsort(names, n, NAME_LEN + 1, name_cmp);
    return n;
}

static void read_ref(const char *name, char *ref, size_t cap)
{
    char path[32];
    clip_path(path, sizeof(path), name, "txt");
    ref[0] = '\0';
    FILE *f = fopen(path, "r");
    if (f) {
        if (!fgets(ref, (int)cap, f)) {
            ref[0] = '\0';
        }
        fclose(f);
    }
    clean_ref(ref);
}

static long clip_ms(const char *name)
{
    char path[32];
    struct stat st;
    clip_path(path, sizeof(path), name, "wav");
    if (stat(path, &st) != 0 || st.st_size <= WAV_HEADER) {
        return 0;
    }
    return (long)((st.st_size - WAV_HEADER) / (RATE * 2 / 1000));
}

/* prefix-NNN livre: um a mais que o maior que existe. */
static void next_name(const char *prefix, char *out)
{
    char names[MAX_CLIPS][NAME_LEN + 1];
    int n = list_clips(names, MAX_CLIPS), max = 0;
    size_t plen = strlen(prefix);
    for (int i = 0; i < n; i++) {
        if (strncmp(names[i], prefix, plen) == 0 && names[i][plen] == '-') {
            int v = atoi(names[i] + plen + 1);
            max = v > max ? v : max;
        }
    }
    snprintf(out, NAME_LEN + 1, "%s-%03d", prefix, (max + 1) % 1000);
}

static void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = v & 0xff;
    p[1] = v >> 8;
}

static void put_le32(uint8_t *p, uint32_t v)
{
    put_le16(p, v & 0xffff);
    put_le16(p + 2, v >> 16);
}

static uint32_t get_le32(const uint8_t *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* O cabeçalho de 44 bytes que a placa e a página escrevem: PCM 16 kHz mono 16 bits. */
static void wav_header(uint8_t h[WAV_HEADER], uint32_t samples)
{
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36 + samples * 2);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    put_le16(h + 20, 1);
    put_le16(h + 22, 1);
    put_le32(h + 24, RATE);
    put_le32(h + 28, RATE * 2);
    put_le16(h + 32, 2);
    put_le16(h + 34, 16);
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, samples * 2);
}

static bool wav_header_ok(const uint8_t *h)
{
    return memcmp(h, "RIFF", 4) == 0 && memcmp(h + 8, "WAVEfmt ", 8) == 0 && get_le16(h + 20) == 1 &&
           get_le16(h + 22) == 1 && get_le32(h + 24) == RATE && get_le16(h + 34) == 16 &&
           memcmp(h + 36, "data", 4) == 0;
}

static esp_err_t write_ref(const char *name, const char *ref)
{
    char path[32];
    clip_path(path, sizeof(path), name, "txt");
    FILE *f = fopen(path, "w");
    if (!f) {
        return ESP_FAIL;
    }
    fprintf(f, "%s\n", ref);
    return fclose(f) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t save_clip(const char *name, const int16_t *pcm, size_t n, const char *ref)
{
    char path[32];
    clip_path(path, sizeof(path), name, "wav");
    FILE *f = fopen(path, "wb");
    if (!f) {
        return ESP_FAIL;
    }
    uint8_t h[WAV_HEADER];
    wav_header(h, (uint32_t)n);
    bool ok = fwrite(h, 1, sizeof(h), f) == sizeof(h) && fwrite(pcm, sizeof(int16_t), n, f) == n;
    ok = fclose(f) == 0 && ok;
    if (!ok) {
        unlink(path);
        return ESP_FAIL;
    }
    return write_ref(name, ref);
}

/* Lê a frase em pcm (capacidade MAX_CLIP_S s). NULL = deu certo. */
static const char *load_clip(const char *name, int16_t *pcm, size_t *n)
{
    char path[32];
    clip_path(path, sizeof(path), name, "wav");
    struct stat st;
    FILE *f = fopen(path, "rb");
    if (!f || stat(path, &st) != 0) {
        if (f) {
            fclose(f);
        }
        return "não abriu o arquivo";
    }
    uint8_t h[WAV_HEADER];
    if (fread(h, 1, sizeof(h), f) != sizeof(h) || !wav_header_ok(h)) {
        fclose(f);
        return "o WAV precisa ser PCM de 16 kHz, mono, 16 bits";
    }
    size_t samples = st.st_size > WAV_HEADER ? (size_t)(st.st_size - WAV_HEADER) / 2 : 0;
    if (samples > (size_t)MAX_CLIP_S * RATE) {
        samples = (size_t)MAX_CLIP_S * RATE;   /* o resto da frase fica de fora */
    }
    *n = fread(pcm, sizeof(int16_t), samples, f);
    fclose(f);
    return *n ? NULL : "WAV sem dados";
}

static void append_result(const char *line)
{
    FILE *f = fopen(RESULTS_PATH, "a");
    if (!f) {
        ESP_LOGE(TAG, "não consegui gravar %s: %s", RESULTS_PATH, strerror(errno));
        return;
    }
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
}

/* ---- Comandos do MultiNet ---------------------------------------------------- */

/* "TEXTO" ou "ID,TEXTO[,FONEMAS]". Devolve false se a linha não é comando. */
static bool parse_command(char *line, int *id, char **text, char **phonemes)
{
    trim(line);
    if (!line[0] || line[0] == '#') {
        return false;
    }
    *id = 0;
    *phonemes = NULL;
    char *comma = strchr(line, ',');
    char *p = line;
    if (comma && p != comma && strspn(p, "0123456789") == (size_t)(comma - p)) {
        *id = atoi(p);
        p = comma + 1;
        comma = strchr(p, ',');
    }
    if (comma) {
        *comma = '\0';
        *phonemes = comma + 1;
        trim(*phonemes);
        if (!**phonemes) {
            *phonemes = NULL;
        }
    }
    trim(p);
    for (char *c = p; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
        if (!isupper((unsigned char)*c) && *c != ' ' && *c != '\'') {
            return false;   /* o MultiNet não aceita número nem pontuação */
        }
    }
    size_t len = strlen(p);
    *text = p;
    return len >= ESP_MN_MIN_PHRASE_LEN && len <= ESP_MN_MAX_PHRASE_LEN;
}

/* Chama fn(id, texto, fonemas) para cada comando do arquivo (ou os de
 * fábrica, se ele não tiver nenhum). Devolve quantos. */
static int for_each_command(void (*fn)(int id, const char *text, const char *phonemes, void *arg), void *arg)
{
    int count = 0, next_id = 1;
    FILE *f = fopen(COMMANDS_PATH, "r");
    char line[CMD_LINE_CAP];
    while (f && count < MAX_COMMANDS && fgets(line, sizeof(line), f)) {
        int id;
        char *text, *ph;
        if (!parse_command(line, &id, &text, &ph)) {
            continue;
        }
        if (id <= 0) {
            id = next_id;
        }
        next_id = id + 1;
        fn(id, text, ph, arg);
        count++;
    }
    if (f) {
        fclose(f);
    }
    for (size_t i = 0; count == 0 && i < sizeof(DEFAULT_COMMANDS) / sizeof(DEFAULT_COMMANDS[0]); i++) {
        fn((int)i + 1, DEFAULT_COMMANDS[i], NULL, arg);
    }
    return count ? count : (int)(sizeof(DEFAULT_COMMANDS) / sizeof(DEFAULT_COMMANDS[0]));
}

static void add_command(int id, const char *text, const char *phonemes, void *arg)
{
    esp_err_t err = phonemes ? esp_mn_commands_phoneme_add(id, text, phonemes) : esp_mn_commands_add(id, text);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "comando recusado: %d %s", id, text);
    }
}

static int load_commands(int *rejected)
{
    esp_mn_commands_clear();
    int n = for_each_command(add_command, NULL);
    esp_mn_error_t *err = esp_mn_commands_update();
    *rejected = err ? err->num : 0;
    jb_t j = { s_cmd_rejected, sizeof(s_cmd_rejected), 0 };
    for (int i = 0; err && i < err->num; i++) {
        jput(&j, "%s%s", i ? "; " : "", err->phrases[i] ? err->phrases[i]->string : "?");
        ESP_LOGW(TAG, "o MultiNet não aceitou: %s", err->phrases[i] ? err->phrases[i]->string : "?");
    }
    if (!err) {
        s_cmd_rejected[0] = '\0';
    }
    return n;
}

/* ---- Medição ----------------------------------------------------------------- */

typedef struct {
    size_t base_ps, base_int, min_ps, min_int;
} mem_t;

static void mem_begin(mem_t *m)
{
    m->base_ps = m->min_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    m->base_int = m->min_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
}

static void mem_track(mem_t *m)
{
    size_t ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM), in = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    m->min_ps = ps < m->min_ps ? ps : m->min_ps;
    m->min_int = in < m->min_int ? in : m->min_int;
}

typedef struct {
    const engine_t *e;
    const char *model;
    const esp_wn_iface_t *wn;
    model_iface_data_t *wn_data;
    esp_mn_iface_t *mn;
    model_iface_data_t *mn_data;
    const esp_afe_sr_iface_t *afe;
    esp_afe_sr_data_t *afe_data;
    int wn_chunk, mn_chunk, feed_chunk, fetch_chunk;
    int16_t *buf;           /* um pedaço de entrada (o maior dos chunks) */
    int16_t *acc;           /* saída do AFE juntada até um pedaço do MultiNet */
    int acc_n;
    int cmd_count, cmd_rejected;
} eng_t;

typedef struct {
    char text[96];
    char raw[96];
    const char *state;      /* MultiNet: detected, timeout ou nada */
    bool detected;          /* WakeNet */
    bool done;              /* o MultiNet já decidiu: o resto é ignorado */
    int det_ms;             /* quando decidiu, desde o começo da frase */
    int count;              /* WakeNet: quantas detecções */
    int cmd_id;
    float prob;
    size_t fed;             /* amostras que passaram pelo motor */
} out_t;

static void fill_chunk(int16_t *dst, const int16_t *pcm, size_t n, size_t pos, int chunk)
{
    for (int i = 0; i < chunk; i++) {
        dst[i] = pos + i < n ? pcm[pos + i] : 0;
    }
}

static esp_err_t afe_create(eng_t *c)
{
    afe_config_t *cfg = afe_config_init("M", s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (!cfg) {
        return ESP_ERR_NO_MEM;
    }
    /* Um microfone, sem referência: sem AEC e sem separação de fontes. */
    cfg->aec_init = false;
    cfg->se_init = false;
    cfg->wakenet_init = c->e->type == ENG_AFE_WN;
    cfg->wakenet_mode = c->e->mode;
    cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    cfg->afe_perferred_priority = ASR_TASK_PRIO;
    if (cfg->wakenet_init && !cfg->wakenet_model_name) {
        afe_config_free(cfg);
        return ESP_ERR_NOT_FOUND;
    }
    c->afe = esp_afe_handle_from_config(cfg);
    c->afe_data = c->afe ? c->afe->create_from_config(cfg) : NULL;
    afe_config_free(cfg);
    if (!c->afe_data) {
        return ESP_ERR_NO_MEM;
    }
    c->feed_chunk = c->afe->get_feed_chunksize(c->afe_data) * c->afe->get_feed_channel_num(c->afe_data);
    c->fetch_chunk = c->afe->get_fetch_chunksize(c->afe_data);
    return c->feed_chunk > 0 && c->fetch_chunk > 0 ? ESP_OK : ESP_FAIL;
}

static void eng_close(eng_t *c)
{
    if (c->afe_data) {
        c->afe->destroy(c->afe_data);
    }
    if (c->mn_data) {
        /* O destroy do MultiNet7 já chama esp_mn_commands_free(); chamar
         * antes dele faz o segundo free dar "mn commands is not initialized". */
        c->mn->destroy(c->mn_data);
    }
    if (c->wn_data) {
        c->wn->destroy(c->wn_data);
    }
    free(c->buf);
    free(c->acc);
    memset(c, 0, sizeof(*c));
}

static esp_err_t eng_open(eng_t *c, const engine_t *e, char *err, size_t cap)
{
    memset(c, 0, sizeof(*c));
    c->e = e;
    const bool wake = engine_is_wake(e);
    c->model = wake ? s_wn_name : s_mn_name;
    if (!c->model) {
        snprintf(err, cap, "o modelo %s não está na partição model", wake ? "wn10_pt_olaesp" : "mn7_en");
        return ESP_ERR_NOT_FOUND;
    }
    if (e->type == ENG_WN) {
        c->wn = esp_wn_handle_from_name(s_wn_name);
        c->wn_data = c->wn ? c->wn->create(s_wn_name, e->mode) : NULL;
        if (!c->wn_data) {
            snprintf(err, cap, "o WakeNet não carregou (faltou memória?)");
            return ESP_ERR_NO_MEM;
        }
        c->wn_chunk = c->wn->get_samp_chunksize(c->wn_data);
    }
    if (e->type == ENG_MN || e->type == ENG_AFE_MN) {
        c->mn = esp_mn_handle_from_name(s_mn_name);
        c->mn_data = c->mn ? c->mn->create(s_mn_name, MN_TIMEOUT_MS) : NULL;
        if (!c->mn_data) {
            snprintf(err, cap, "o MultiNet não carregou (faltou memória?)");
            return ESP_ERR_NO_MEM;
        }
        c->mn_chunk = c->mn->get_samp_chunksize(c->mn_data);
        esp_mn_commands_alloc(c->mn, c->mn_data);
        c->cmd_count = load_commands(&c->cmd_rejected);
    }
    if (engine_uses_afe(e)) {
        esp_err_t r = afe_create(c);
        if (r != ESP_OK) {
            snprintf(err, cap, r == ESP_ERR_NOT_FOUND ? "o AFE não achou o WakeNet na partição model"
                                                      : "o AFE não iniciou (faltou memória?)");
            return r;
        }
        c->afe->print_pipeline(c->afe_data);
    }
    int big = c->wn_chunk > c->mn_chunk ? c->wn_chunk : c->mn_chunk;
    big = c->feed_chunk > big ? c->feed_chunk : big;
    c->buf = malloc(big * sizeof(int16_t));
    if (c->afe_data && c->mn_data) {
        c->acc = malloc(c->mn_chunk * sizeof(int16_t));
    }
    if (big <= 0 || !c->buf || (c->afe_data && c->mn_data && !c->acc)) {
        snprintf(err, cap, "sem memória para os buffers");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Antes de cada frase, fora do tempo medido: o estado da anterior não vaza.
 * O AFE é recriado (não há como zerar o VAD e o WakeNet de dentro dele). */
static esp_err_t eng_prepare(eng_t *c, bool first)
{
    if (c->wn_data) {
        c->wn->clean(c->wn_data);
    }
    if (c->mn_data) {
        c->mn->clean(c->mn_data);
    }
    c->acc_n = 0;
    if (!engine_uses_afe(c->e)) {
        return ESP_OK;
    }
    if (c->afe_data && !first) {
        c->afe->destroy(c->afe_data);
        c->afe_data = NULL;
    }
    return c->afe_data ? ESP_OK : afe_create(c);
}

static void mn_take(eng_t *c, out_t *o, size_t pos, esp_mn_state_t st)
{
    esp_mn_results_t *r = c->mn->get_results(c->mn_data);
    o->done = true;
    o->det_ms = (int)(pos * 1000 / RATE);
    o->state = st == ESP_MN_STATE_DETECTED ? "detected" : "timeout";
    if (r) {
        lower_copy(o->raw, r->raw_string, sizeof(o->raw));
    }
    if (st == ESP_MN_STATE_DETECTED && r && r->num > 0) {
        o->cmd_id = r->command_id[0];
        o->prob = r->prob[0];
        esp_mn_phrase_t *ph = esp_mn_commands_get_from_index(r->phrase_id[0]);
        lower_copy(o->text, ph && ph->string ? ph->string : r->string, sizeof(o->text));
    }
}

static void mn_feed(eng_t *c, int16_t *chunk, size_t pos, out_t *o)
{
    esp_mn_state_t st = c->mn->detect(c->mn_data, chunk);
    if (st == ESP_MN_STATE_DETECTED || st == ESP_MN_STATE_TIMEOUT) {
        mn_take(c, o, pos, st);
    }
}

static void wake_hit(out_t *o, size_t pos)
{
    if (!o->detected) {
        o->det_ms = (int)(pos * 1000 / RATE);
    }
    o->detected = true;
    o->count++;
}

static void run_direct(eng_t *c, const int16_t *pcm, size_t n, out_t *o, mem_t *m)
{
    const bool wake = c->e->type == ENG_WN;
    const int chunk = wake ? c->wn_chunk : c->mn_chunk;
    const size_t total = n + (size_t)(wake ? WN_TAIL_MS : MN_TAIL_MS) * RATE / 1000;
    for (size_t pos = 0, k = 0; pos < total && !o->done; pos += chunk, k++) {
        fill_chunk(c->buf, pcm, n, pos, chunk);
        o->fed = pos + chunk;
        if (wake) {
            if (c->wn->detect(c->wn_data, c->buf) == WAKENET_DETECTED) {
                wake_hit(o, pos + chunk);
            }
        } else {
            mn_feed(c, c->buf, pos + chunk, o);
        }
        if (k % 16 == 0) {
            mem_track(m);
        }
    }
}

static void afe_take(eng_t *c, const afe_fetch_result_t *r, size_t got, out_t *o)
{
    if (c->e->type == ENG_AFE_WN) {
        if (r->wakeup_state == WAKENET_DETECTED) {
            wake_hit(o, got);
        }
        return;
    }
    const int16_t *d = r->data;
    int left = r->data_size / (int)sizeof(int16_t);
    while (left > 0 && !o->done && d) {
        int take = c->mn_chunk - c->acc_n < left ? c->mn_chunk - c->acc_n : left;
        memcpy(c->acc + c->acc_n, d, take * sizeof(int16_t));
        c->acc_n += take;
        d += take;
        left -= take;
        if (c->acc_n == c->mn_chunk) {
            mn_feed(c, c->acc, got, o);
            c->acc_n = 0;
        }
    }
}

/*
 * Pelo AFE, sem uma task para alimentar e outra para buscar: alimenta um
 * pedaço e pega a saída que já houver; só espera a saída quando ela fica mais
 * de AFE_MAX_LAG quadros para trás (o anel de entrada do AFE descarta o que
 * passar do tamanho dele). Só busca com um fetch_chunk inteiro alimentado e
 * ainda não lido: cada fetch com o anel vazio imprime "Ringbuffer of AFE is
 * empty" (centenas por frase) e o log na serial atrasa a IA dos rostos.
 * Com o formato "M" (um canal) amostras alimentadas e de saída se equivalem.
 */
static esp_err_t run_afe(eng_t *c, const int16_t *pcm, size_t n, out_t *o, mem_t *m)
{
    const size_t tail = (size_t)(c->e->type == ENG_AFE_WN ? WN_TAIL_MS : MN_TAIL_MS) * RATE / 1000;
    const size_t total = n + tail, lag = (size_t)AFE_MAX_LAG * c->fetch_chunk;
    size_t fed = 0, got = 0;
    int misses = 0;
    for (size_t pos = 0, k = 0; pos < total && !o->done; pos += c->feed_chunk, k++) {
        fill_chunk(c->buf, pcm, n, pos, c->feed_chunk);
        c->afe->feed(c->afe_data, c->buf);
        fed += c->feed_chunk;
        o->fed = fed;
        while (!o->done && fed >= got + c->fetch_chunk) {
            bool behind = fed > got + lag;
            afe_fetch_result_t *r = c->afe->fetch_with_delay(c->afe_data, behind ? pdMS_TO_TICKS(AFE_WAIT_MS) : 0);
            if (!r || r->ret_value == ESP_FAIL || r->data_size <= 0) {
                if (behind && ++misses > 3) {
                    return ESP_ERR_TIMEOUT;
                }
                break;
            }
            got += r->data_size / sizeof(int16_t);
            afe_take(c, r, got, o);
        }
        if (k % 16 == 0) {
            mem_track(m);
        }
    }
    while (fed >= got + c->fetch_chunk && !o->done) {
        afe_fetch_result_t *r = c->afe->fetch_with_delay(c->afe_data, pdMS_TO_TICKS(50));
        if (!r || r->ret_value == ESP_FAIL || r->data_size <= 0) {
            break;
        }
        got += r->data_size / sizeof(int16_t);
        afe_take(c, r, got, o);
    }
    return ESP_OK;
}

static esp_err_t eng_process(eng_t *c, const int16_t *pcm, size_t n, out_t *o, mem_t *m)
{
    memset(o, 0, sizeof(*o));
    o->state = "nada";
    o->cmd_id = -1;
    esp_err_t err = ESP_OK;
    if (engine_uses_afe(c->e)) {
        err = run_afe(c, pcm, n, o, m);
    } else {
        run_direct(c, pcm, n, o, m);
    }
    if (engine_is_wake(c->e) && o->detected) {
        strcpy(o->text, "olá esp");
    }
    return err;
}

static bool stop_requested(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool stop = s_job.stop;
    xSemaphoreGive(s_lock);
    return stop;
}

static void line_head(jb_t *j, const engine_t *e, long run, bool pause, const char *model, const char *clip)
{
    j->used = 0;
    jput(j, "{\"engine\":\"%s\",\"kind\":\"%s\",\"run\":%ld,\"pause_ai\":%s,\"model\":", e->name, e->kind, run,
         pause ? "true" : "false");
    jstr(j, model ? model : "");
    jput(j, ",\"file\":\"%s.wav\"", clip);
}

/* Um motor sobre todas as frases: uma linha do results.txt por frase. */
static void run_engine(const engine_t *e, long run, bool pause, int16_t *pcm)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_job.engine, e->name, sizeof(s_job.engine));
    strlcpy(s_job.clip, s_req.clips[0], sizeof(s_job.clip));
    xSemaphoreGive(s_lock);

    char *line = malloc(LINE_CAP);
    if (!line) {
        return;
    }
    jb_t j = { line, LINE_CAP, 0 };
    mem_t m;
    mem_begin(&m);
    eng_t c;
    char err[96] = "";
    int64_t t0 = esp_timer_get_time();
    esp_err_t r = eng_open(&c, e, err, sizeof(err));
    double load_ms = (esp_timer_get_time() - t0) / 1000.0;
    mem_track(&m);

    if (r != ESP_OK) {
        ESP_LOGW(TAG, "%s: %s", e->name, err);
        for (int i = 0; i < s_req.nclips; i++) {
            line_head(&j, e, run, pause, c.model, s_req.clips[i]);
            jput(&j, ",\"error\":");
            jstr(&j, err);
            jput(&j, "}");
            append_result(line);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_job.done += s_req.nclips;
        snprintf(s_job.error, sizeof(s_job.error), "%s: %s", e->name, err);
        xSemaphoreGive(s_lock);
        eng_close(&c);
        free(line);
        return;
    }
    ESP_LOGI(TAG, "%s: carregou em %.0f ms, %u KB de PSRAM, %u KB internos", e->name, load_ms,
             (unsigned)((m.base_ps - m.min_ps) / 1024), (unsigned)((m.base_int - m.min_int) / 1024));

    for (int i = 0; i < s_req.nclips && !stop_requested(); i++) {
        const char *clip = s_req.clips[i];
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_job.clip, clip, sizeof(s_job.clip));
        xSemaphoreGive(s_lock);

        size_t n = 0;
        const char *lerr = load_clip(clip, pcm, &n);
        if (!lerr && eng_prepare(&c, i == 0) != ESP_OK) {
            lerr = "o AFE não reiniciou (faltou memória?)";
        }
        out_t o;
        int64_t p0 = esp_timer_get_time();
        if (!lerr && eng_process(&c, pcm, n, &o, &m) != ESP_OK) {
            lerr = "o AFE parou de devolver áudio";
        }
        double proc_ms = (esp_timer_get_time() - p0) / 1000.0, audio_ms = n * 1000.0 / RATE;
        double fed_ms = lerr ? 0 : o.fed * 1000.0 / RATE;

        line_head(&j, e, run, pause, c.model, clip);
        if (lerr) {
            jput(&j, ",\"error\":");
            jstr(&j, lerr);
        } else {
            jput(&j, ",\"audio_ms\":%.0f,\"fed_ms\":%.0f,\"load_ms\":%.0f,\"proc_ms\":%.0f,\"rtf\":%.3f,"
                 "\"ram_kb\":%u,\"ram_int_kb\":%u,\"text\":",
                 audio_ms, fed_ms, load_ms, proc_ms, fed_ms > 0 ? proc_ms / fed_ms : 0.0,
                 (unsigned)((m.base_ps - m.min_ps + m.base_int - m.min_int) / 1024),
                 (unsigned)((m.base_int - m.min_int) / 1024));
            jstr(&j, o.text);
            if (engine_is_wake(e)) {
                jput(&j, ",\"detected\":%s,\"det_ms\":%d,\"count\":%d", o.detected ? "true" : "false", o.det_ms,
                     o.count);
            } else {
                jput(&j, ",\"cmd_id\":%d,\"prob\":%.3f,\"state\":\"%s\",\"det_ms\":%d,\"commands\":%d,"
                     "\"cmd_rejected\":%d,\"raw\":",
                     o.cmd_id, o.prob, o.state, o.det_ms, c.cmd_count, c.cmd_rejected);
                jstr(&j, o.raw);
            }
        }
        jput(&j, "}");
        if (j.used < LINE_CAP) {
            append_result(line);
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_job.done++;
        xSemaphoreGive(s_lock);
    }
    eng_close(&c);
    free(line);
}

static void run_job(void)
{
    /* Um buffer para a maior frase, alocado antes da medição: não entra no
     * pico de memória dos motores. */
    int16_t *pcm = heap_caps_malloc((size_t)MAX_CLIP_S * RATE * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    long run = ++s_run_seq;
    bool pause = s_req.pause;
    if (!pcm) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        strlcpy(s_job.error, "sem PSRAM para o áudio da frase", sizeof(s_job.error));
        xSemaphoreGive(s_lock);
    } else {
        if (pause) {
            face_pause(true);
        }
        for (size_t i = 0; i < ENGINE_COUNT && !stop_requested(); i++) {
            if (s_req.engines & (1u << i)) {
                run_engine(&ENGINES[i], run, pause, pcm);
            }
        }
        if (pause) {
            face_pause(false);
        }
        free(pcm);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_job.running = false;
    s_job.ended_us = esp_timer_get_time();
    ESP_LOGI(TAG, "medição %ld terminou em %.1f s%s", run, (s_job.ended_us - s_job.started_us) / 1e6,
             s_job.stop ? " (interrompida)" : "");
    xSemaphoreGive(s_lock);
}

static void record_job(void)
{
    const size_t n = (size_t)s_req.secs * RATE;
    int16_t *pcm = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char name[NAME_LEN + 1] = "", error[64] = "";
    esp_err_t err = pcm ? audio_record(pcm, n, (uint32_t)s_req.secs * 1000 + 2000) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        next_name("mic", name);
        if (save_clip(name, pcm, n, s_req.ref) != ESP_OK) {
            strlcpy(error, "não consegui gravar na flash (cheia?)", sizeof(error));
            name[0] = '\0';
        } else {
            ESP_LOGI(TAG, "gravação %s: %d s", name, s_req.secs);
        }
    } else {
        strlcpy(error, err == ESP_ERR_TIMEOUT ? "o microfone parou de mandar áudio" : "sem memória para gravar",
                sizeof(error));
    }
    free(pcm);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_rec.active = false;
    strlcpy(s_rec.last, name, sizeof(s_rec.last));
    strlcpy(s_rec.error, error, sizeof(s_rec.error));
    xSemaphoreGive(s_lock);
}

static void asr_task(void *arg)
{
    while (1) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (s_req.type == REQ_RUN) {
            run_job();
        } else if (s_req.type == REQ_REC) {
            record_job();
        }
    }
}

/* ---- HTTP -------------------------------------------------------------------- */

extern const char asr_html_start[] asm("_binary_asr_html_start");
extern const char asr_html_end[] asm("_binary_asr_html_end");

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static esp_err_t send_error(httpd_req_t *req, const char *status, const char *msg)
{
    char body[192];
    jb_t j = { body, sizeof(body), 0 };
    jput(&j, "{\"ok\":false,\"error\":");
    jstr(&j, msg);
    jput(&j, "}");
    return send_json(req, status, body);
}

/* O corpo inteiro em buf (termina com \0). -1 = grande demais ou caiu. */
static int read_body(httpd_req_t *req, char *buf, size_t cap)
{
    if (req->content_len >= cap) {
        return -1;
    }
    size_t got = 0;
    int timeouts = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
            continue;
        }
        if (r <= 0) {
            return -1;
        }
        got += r;
    }
    buf[got] = '\0';
    return (int)got;
}

static bool form_value(const char *form, const char *key, char *val, size_t cap)
{
    if (httpd_query_key_value(form, key, val, cap) != ESP_OK) {
        return false;
    }
    url_decode(val);
    return true;
}

static esp_err_t page_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, asr_html_start, asr_html_end - asr_html_start - 1);
}

/* Resposta em pedaços: junta em buf e manda quando enche. */
typedef struct {
    httpd_req_t *req;
    jb_t j;
    bool err;
} cw_t;

static void cw_flush(cw_t *w)
{
    /* Um pedaço que não coube (cw_room subestimou) fica cortado, mas nunca
     * se lê além do buffer. */
    size_t n = w->j.used < w->j.cap ? w->j.used : w->j.cap - 1;
    if (n && !w->err && httpd_resp_send_chunk(w->req, w->j.buf, n) != ESP_OK) {
        w->err = true;
    }
    w->j.used = 0;
}

/* Garante espaço para room bytes antes de escrever um pedaço. */
static void cw_room(cw_t *w, size_t room)
{
    if (w->j.used + room >= w->j.cap) {
        cw_flush(w);
    }
}

typedef struct {
    cw_t *w;
    bool first;
} cmd_list_t;

static void put_command(int id, const char *text, const char *phonemes, void *arg)
{
    cmd_list_t *l = arg;
    cw_room(l->w, CMD_LINE_CAP + 8);
    jput(&l->w->j, "%s", l->first ? "" : ",");
    jstr(&l->w->j, text);
    l->first = false;
}

/*
 * {"ready","models":[...],"engines":[{"name","kind","desc","ok"}],
 *  "clips":[{"name","ms","ref"}],"commands":[...],"cmd_rejected":"...",
 *  "job":{...},"rec":{...},"mic":bool,"level":{"rms","peak"},
 *  "mem":{"psram","internal"},"storage":{"total","free"},"results":[...]}
 */
static esp_err_t state_handler(httpd_req_t *req)
{
    cw_t w = { .req = req, .j = { .buf = heap_caps_malloc(CHUNK_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), .cap = CHUNK_CAP } };
    char (*names)[NAME_LEN + 1] = malloc(MAX_CLIPS * (NAME_LEN + 1));
    char *line = malloc(LINE_CAP);
    if (!w.j.buf || !names || !line) {
        free(w.j.buf);
        free(names);
        free(line);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria");
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    jb_t *j = &w.j;

    jput(j, "{\"ready\":%s,\"models\":[", s_ready && s_models ? "true" : "false");
    for (int i = 0; s_models && i < s_models->num; i++) {
        cw_room(&w, 80);
        jput(j, "%s", i ? "," : "");
        jstr(j, s_models->model_name[i]);
    }
    jput(j, "],\"engines\":[");
    for (size_t i = 0; i < ENGINE_COUNT; i++) {
        const engine_t *e = &ENGINES[i];
        cw_room(&w, 320);
        jput(j, "%s{\"name\":\"%s\",\"kind\":\"%s\",\"ok\":%s,\"desc\":", i ? "," : "", e->name, e->kind,
             (engine_is_wake(e) ? s_wn_name : s_mn_name) ? "true" : "false");
        jstr(j, e->desc);
        jput(j, "}");
    }
    jput(j, "],\"clips\":[");
    int n = s_ready ? list_clips(names, MAX_CLIPS) : 0;
    for (int i = 0; i < n; i++) {
        char ref[REF_CAP];
        read_ref(names[i], ref, sizeof(ref));
        cw_room(&w, REF_CAP * 2 + 64);
        jput(j, "%s{\"name\":\"%s\",\"ms\":%ld,\"ref\":", i ? "," : "", names[i], clip_ms(names[i]));
        jstr(j, ref);
        jput(j, "}");
    }
    jput(j, "],\"commands\":[");
    cmd_list_t cl = { &w, true };
    if (s_ready) {
        for_each_command(put_command, &cl);
    }
    cw_room(&w, sizeof(s_cmd_rejected) * 2 + 512);
    jput(j, "],\"cmd_rejected\":");
    jstr(j, s_cmd_rejected);

    cw_room(&w, 768);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int64_t now = esp_timer_get_time(), end = s_job.running ? now : s_job.ended_us;
    jput(j, ",\"job\":{\"running\":%s,\"engine\":\"%s\",\"clip\":\"%s\",\"done\":%d,\"total\":%d,\"elapsed_ms\":%lld,"
         "\"pause_ai\":%s,\"error\":",
         s_job.running ? "true" : "false", s_job.engine, s_job.clip, s_job.done, s_job.total,
         (long long)(s_job.started_us ? (end - s_job.started_us) / 1000 : 0), s_job.pause_ai ? "true" : "false");
    jstr(j, s_job.error);
    jput(j, "},\"rec\":{\"active\":%s,\"secs\":%d,\"elapsed_ms\":%lld,\"got_ms\":%u,\"last\":\"%s\",\"error\":",
         s_rec.active ? "true" : "false", s_rec.secs,
         (long long)(s_rec.active ? (now - s_rec.started_us) / 1000 : 0),
         (unsigned)(audio_record_progress() * 1000 / RATE), s_rec.last);
    jstr(j, s_rec.error);
    xSemaphoreGive(s_lock);

    audio_level_t lv;
    audio_get_level(&lv);
    uint64_t total = 0, free_b = 0;
    if (s_ready) {
        esp_vfs_fat_info(CLIPS_MOUNT, &total, &free_b);
    }
    cw_room(&w, 320);
    jput(j, "},\"mic\":%s,\"level\":{\"rms\":%.1f,\"peak\":%.1f},\"mem\":{\"psram\":%u,\"internal\":%u},"
         "\"storage\":{\"total\":%llu,\"free\":%llu},\"results\":[",
         audio_ok() ? "true" : "false", lv.rms_db, lv.peak_db,
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
         (unsigned long long)total, (unsigned long long)free_b);

    /* Uma linha por resultado. A task pode estar acrescentando uma agora:
     * linha sem \n no fim (ainda incompleta) fica de fora. */
    FILE *f = s_ready ? fopen(RESULTS_PATH, "r") : NULL;
    bool first = true;
    while (f && fgets(line, LINE_CAP, f)) {
        size_t len = strlen(line);
        if (len < 2 || line[len - 1] != '\n' || line[0] != '{') {
            continue;
        }
        line[--len] = '\0';
        cw_room(&w, len + 2);
        if (len + 2 >= w.j.cap) {
            continue;
        }
        jput(j, "%s%s", first ? "" : ",", line);
        first = false;
    }
    if (f) {
        fclose(f);
    }
    cw_room(&w, 8);
    jput(j, "]}\n");
    cw_flush(&w);
    esp_err_t err = w.err ? ESP_FAIL : httpd_resp_send_chunk(req, NULL, 0);
    free(w.j.buf);
    free(names);
    free(line);
    return err;
}

static esp_err_t clip_handler(httpd_req_t *req)
{
    char query[48], name[16], path[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || !valid_name(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use /asr/clip?name=X");
    }
    clip_path(path, sizeof(path), name, "wav");
    FILE *f = fopen(path, "rb");
    if (!f) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "frase nao encontrada");
    }
    char *buf = heap_caps_malloc(CHUNK_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        fclose(f);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria");
    }
    httpd_resp_set_type(req, "audio/wav");
    esp_err_t err = ESP_OK;
    size_t r;
    while (err == ESP_OK && (r = fread(buf, 1, CHUNK_CAP, f)) > 0) {
        err = httpd_resp_send_chunk(req, buf, r);
    }
    fclose(f);
    free(buf);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

/* Liga s_job/s_rec como ocupados e acorda a task; falso se já estava. */
static bool claim(req_type_t type)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (busy_locked()) {
        xSemaphoreGive(s_lock);
        return false;
    }
    if (type == REQ_RUN) {
        s_job.running = true;
        s_job.stop = false;
        s_job.pause_ai = s_req.pause;
        s_job.done = 0;
        s_job.total = 0;
        for (size_t i = 0; i < ENGINE_COUNT; i++) {
            s_job.total += (s_req.engines >> i & 1) * s_req.nclips;
        }
        s_job.started_us = esp_timer_get_time();
        s_job.error[0] = '\0';
    } else {
        s_rec.active = true;
        s_rec.secs = s_req.secs;
        s_rec.started_us = esp_timer_get_time();
        s_rec.last[0] = s_rec.error[0] = '\0';
    }
    s_req.type = type;
    xSemaphoreGive(s_lock);
    xTaskNotifyGive(s_task);
    return true;
}

static bool is_busy(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool b = busy_locked();
    xSemaphoreGive(s_lock);
    return b;
}

static esp_err_t run_handler(httpd_req_t *req)
{
    if (!s_ready || !s_models) {
        return send_error(req, "503 Service Unavailable", "sem /clips ou sem a partição model (veja o log)");
    }
    if (is_busy()) {
        return send_error(req, "409 Conflict", "já tem uma medição ou gravação rodando");
    }
    char *body = malloc(BODY_CAP), *val = malloc(BODY_CAP);
    if (!body || !val || read_body(req, body, BODY_CAP) < 0) {
        free(body);
        free(val);
        return send_error(req, "400 Bad Request", "corpo grande demais ou incompleto");
    }
    const char *bad = NULL;
    unsigned engines = 0;
    int nclips = 0;
    if (form_value(body, "engines", val, BODY_CAP)) {
        for (char *save = NULL, *t = strtok_r(val, ",", &save); t && !bad; t = strtok_r(NULL, ",", &save)) {
            size_t i = 0;
            while (i < ENGINE_COUNT && strcmp(ENGINES[i].name, t) != 0) {
                i++;
            }
            if (i == ENGINE_COUNT) {
                bad = "motor desconhecido";
            } else {
                engines |= 1u << i;
            }
        }
    }
    if (!bad && form_value(body, "clips", val, BODY_CAP)) {
        for (char *save = NULL, *t = strtok_r(val, ",", &save); t && !bad; t = strtok_r(NULL, ",", &save)) {
            char path[32];
            struct stat st;
            clip_path(path, sizeof(path), t, "wav");
            if (!valid_name(t) || stat(path, &st) != 0) {
                bad = "frase não encontrada";
            } else if (nclips == MAX_CLIPS) {
                bad = "frases demais";
            } else {
                strlcpy(s_req.clips[nclips++], t, NAME_LEN + 1);
            }
        }
    }
    bool pause = form_value(body, "pause", val, BODY_CAP) && strcmp(val, "1") == 0;
    free(body);
    free(val);
    if (!bad && (!engines || !nclips)) {
        bad = "escolha ao menos um motor e uma frase";
    }
    if (bad) {
        return send_error(req, "400 Bad Request", bad);
    }
    s_req.engines = engines;
    s_req.nclips = nclips;
    s_req.pause = pause;
    if (!claim(REQ_RUN)) {
        return send_error(req, "409 Conflict", "já tem uma medição ou gravação rodando");
    }
    ESP_LOGI(TAG, "medição: %d frase(s)%s", nclips, pause ? ", IA de rostos parada" : "");
    return send_json(req, NULL, "{\"ok\":true}");
}

static esp_err_t stop_handler(httpd_req_t *req)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_job.stop = s_job.running;
    xSemaphoreGive(s_lock);
    return send_json(req, NULL, "{\"ok\":true}");
}

static esp_err_t record_handler(httpd_req_t *req)
{
    if (!s_ready) {
        return send_error(req, "503 Service Unavailable", "sem /clips (veja o log)");
    }
    if (!audio_ok()) {
        return send_error(req, "503 Service Unavailable", "o microfone não iniciou");
    }
    char body[512], val[REF_CAP * 3];
    if (read_body(req, body, sizeof(body)) < 0) {
        return send_error(req, "400 Bad Request", "corpo grande demais");
    }
    int secs = form_value(body, "secs", val, sizeof(val)) ? atoi(val) : 0;
    if (secs < 1 || secs > MAX_REC_S) {
        return send_error(req, "400 Bad Request", "secs fora de 1..15");
    }
    if (is_busy()) {
        return send_error(req, "409 Conflict", "já tem uma medição ou gravação rodando");
    }
    if (!form_value(body, "ref", val, sizeof(val))) {
        val[0] = '\0';
    }
    clean_ref(val);
    strlcpy(s_req.ref, val, sizeof(s_req.ref));
    s_req.secs = secs;
    if (!claim(REQ_REC)) {
        return send_error(req, "409 Conflict", "já tem uma medição ou gravação rodando");
    }
    return send_json(req, NULL, "{\"ok\":true}");
}

static esp_err_t upload_handler(httpd_req_t *req)
{
    if (!s_ready) {
        return send_error(req, "503 Service Unavailable", "sem /clips (veja o log)");
    }
    if (is_busy()) {
        return send_error(req, "409 Conflict", "espere a medição ou a gravação terminar");
    }
    if (req->content_len <= WAV_HEADER || req->content_len > WAV_HEADER + (size_t)MAX_CLIP_S * RATE * 2) {
        return send_error(req, "400 Bad Request", "o WAV precisa ter de 1 amostra a 30 s");
    }
    char query[REF_CAP * 3 + 16], ref[REF_CAP * 3] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        form_value(query, "ref", ref, sizeof(ref));
    }
    clean_ref(ref);
    ref[REF_CAP - 1] = '\0';

    uint8_t h[WAV_HEADER];
    size_t got = 0;
    int stalls = 0;
    while (got < sizeof(h)) {
        int r = httpd_req_recv(req, (char *)h + got, sizeof(h) - got);
        if (r <= 0 && (r != HTTPD_SOCK_ERR_TIMEOUT || ++stalls >= 5)) {
            return ESP_FAIL;
        }
        got += r > 0 ? r : 0;
    }
    if (!wav_header_ok(h)) {
        return send_error(req, "400 Bad Request", "o corpo precisa ser um WAV PCM de 16 kHz, mono, 16 bits");
    }
    char name[NAME_LEN + 1], path[32];
    next_name("wav", name);
    clip_path(path, sizeof(path), name, "wav");
    FILE *f = fopen(path, "wb");
    char *buf = malloc(CHUNK_CAP);
    if (!f || !buf) {
        if (f) {
            fclose(f);
        }
        free(buf);
        return send_error(req, "500 Internal Server Error", "não consegui criar o arquivo");
    }
    /* O cabeçalho é refeito com o tamanho real do que chegou. */
    size_t left = req->content_len - WAV_HEADER, data = 0;
    bool ok = fwrite(h, 1, sizeof(h), f) == sizeof(h);
    int timeouts = 0;
    while (ok && left > 0) {
        int r = httpd_req_recv(req, buf, left < CHUNK_CAP ? left : CHUNK_CAP);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
            continue;
        }
        ok = r > 0 && fwrite(buf, 1, r, f) == (size_t)r;
        left -= ok ? r : 0;
        data += ok ? r : 0;
    }
    data &= ~(size_t)1;
    wav_header(h, (uint32_t)(data / 2));
    ok = ok && fseek(f, 0, SEEK_SET) == 0 && fwrite(h, 1, sizeof(h), f) == sizeof(h);
    ok = fclose(f) == 0 && ok;
    free(buf);
    if (!ok || write_ref(name, ref) != ESP_OK) {
        unlink(path);
        return send_error(req, "500 Internal Server Error", "não consegui gravar o arquivo (flash cheia?)");
    }
    ESP_LOGI(TAG, "arquivo %s: %.1f s", name, data / (2.0 * RATE));
    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":true,\"name\":\"%s\"}", name);
    return send_json(req, NULL, body);
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    char query[48], name[16], path[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK || !valid_name(name) ||
        (strncmp(name, "mic-", 4) != 0 && strncmp(name, "wav-", 4) != 0)) {
        return send_error(req, "400 Bad Request", "use /asr/delete?name=mic-NNN ou wav-NNN");
    }
    if (is_busy()) {
        return send_error(req, "409 Conflict", "espere a medição ou a gravação terminar");
    }
    clip_path(path, sizeof(path), name, "wav");
    if (unlink(path) != 0) {
        return send_error(req, "404 Not Found", "frase não encontrada");
    }
    clip_path(path, sizeof(path), name, "txt");
    unlink(path);
    return send_json(req, NULL, "{\"ok\":true}");
}

static esp_err_t clear_handler(httpd_req_t *req)
{
    if (is_busy()) {
        return send_error(req, "409 Conflict", "espere a medição terminar");
    }
    FILE *f = fopen(RESULTS_PATH, "w");
    if (!f) {
        return send_error(req, "500 Internal Server Error", strerror(errno));
    }
    fclose(f);
    return send_json(req, NULL, "{\"ok\":true}");
}

static esp_err_t commands_handler(httpd_req_t *req)
{
    if (!s_ready) {
        return send_error(req, "503 Service Unavailable", "sem /clips (veja o log)");
    }
    if (is_busy()) {
        return send_error(req, "409 Conflict", "espere a medição terminar");
    }
    char *body = malloc(COMMANDS_BODY);
    int len = body ? read_body(req, body, COMMANDS_BODY) : -1;
    if (len < 0) {
        free(body);
        return send_error(req, "400 Bad Request", "lista grande demais");
    }
    /* Confere linha a linha antes de gravar: a linha ruim volta na mensagem. */
    int count = 0;
    char line[CMD_LINE_CAP], shown[64], bad[CMD_LINE_CAP + 64] = "";
    for (const char *p = body; *p && !bad[0];) {
        const char *nl = strchr(p, '\n');
        size_t n = nl ? (size_t)(nl - p) : strlen(p);
        if (n >= sizeof(line)) {
            snprintf(bad, sizeof(bad), "linha longa demais (máximo %d caracteres)", CMD_LINE_CAP - 1);
            break;
        }
        memcpy(line, p, n);
        line[n] = '\0';
        p += n + (nl ? 1 : 0);
        trim(line);
        if (!line[0] || line[0] == '#') {
            continue;
        }
        strlcpy(shown, line, sizeof(shown));
        int id;
        char *text, *ph;
        if (!parse_command(line, &id, &text, &ph)) {
            snprintf(bad, sizeof(bad), "comando inválido (só letras sem acento, espaço e '): %s", shown);
            break;
        }
        count++;
    }
    if (!bad[0] && (count == 0 || count > MAX_COMMANDS)) {
        snprintf(bad, sizeof(bad), "a lista precisa ter de 1 a %d comandos", MAX_COMMANDS);
    }
    if (bad[0]) {
        free(body);
        return send_error(req, "400 Bad Request", bad);
    }
    FILE *f = fopen(COMMANDS_PATH, "w");
    bool ok = f && fwrite(body, 1, len, f) == (size_t)len && fputc('\n', f) != EOF;
    ok = f && fclose(f) == 0 && ok;
    free(body);
    if (!ok) {
        return send_error(req, "500 Internal Server Error", "não consegui gravar commands.txt");
    }
    ESP_LOGI(TAG, "%d comando(s) do MultiNet salvos", count);
    char resp[48];
    snprintf(resp, sizeof(resp), "{\"ok\":true,\"count\":%d}", count);
    return send_json(req, NULL, resp);
}

void asr_register(httpd_handle_t server)
{
    static const httpd_uri_t uris[] = {
        { .uri = "/asr",          .method = HTTP_GET,  .handler = page_handler },
        { .uri = "/asr/state",    .method = HTTP_GET,  .handler = state_handler },
        { .uri = "/asr/clip",     .method = HTTP_GET,  .handler = clip_handler },
        { .uri = "/asr/run",      .method = HTTP_POST, .handler = run_handler },
        { .uri = "/asr/stop",     .method = HTTP_POST, .handler = stop_handler },
        { .uri = "/asr/record",   .method = HTTP_POST, .handler = record_handler },
        { .uri = "/asr/upload",   .method = HTTP_POST, .handler = upload_handler },
        { .uri = "/asr/delete",   .method = HTTP_POST, .handler = delete_handler },
        { .uri = "/asr/clear",    .method = HTTP_POST, .handler = clear_handler },
        { .uri = "/asr/commands", .method = HTTP_POST, .handler = commands_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
}

/* ---- Início ------------------------------------------------------------------ */

static void scan_runs(void)
{
    FILE *f = fopen(RESULTS_PATH, "r");
    char line[LINE_CAP];
    while (f && fgets(line, sizeof(line), f)) {
        const char *p = strstr(line, "\"run\":");
        long run = p ? strtol(p + 6, NULL, 10) : 0;
        s_run_seq = run > s_run_seq ? run : s_run_seq;
    }
    if (f) {
        fclose(f);
    }
}

esp_err_t asr_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    esp_vfs_fat_mount_config_t mount = {
        .format_if_mount_failed = true,
        .max_files = 4,
        .allocation_unit_size = CONFIG_WL_SECTOR_SIZE,
    };
    wl_handle_t wl;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(CLIPS_MOUNT, CLIPS_PART, &mount, &wl);
    if (err == ESP_OK) {
        s_ready = true;
        scan_runs();
    } else {
        ESP_LOGE(TAG, "sem a partição \"clips\" (%s): a página Fala fica sem frases - grave a tabela de "
                 "partições nova (idf.py flash)", esp_err_to_name(err));
    }

    s_models = esp_srmodel_init(MODEL_PART);
    if (s_models) {
        s_wn_name = esp_srmodel_filter(s_models, ESP_WN_PREFIX, "olaesp");
        s_mn_name = esp_srmodel_filter(s_models, ESP_MN_PREFIX, ESP_MN_ENGLISH);
        for (int i = 0; i < s_models->num; i++) {
            ESP_LOGI(TAG, "modelo: %s", s_models->model_name[i]);
        }
    } else {
        ESP_LOGE(TAG, "partição \"model\" sem modelos do ESP-SR: o idf.py flash grava o srmodels.bin nela");
    }

    if (xTaskCreatePinnedToCore(asr_task, "asr", ASR_TASK_STACK, NULL, ASR_TASK_PRIO, &s_task, tskNO_AFFINITY) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
