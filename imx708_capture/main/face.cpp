/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ver face.h.
 *
 * Do esp-dl só usamos os dois modelos (HumanFaceDetect e HumanFaceFeat). O
 * banco de rostos é próprio: o dl::recognition::DataBase do esp-dl 3.3 devolve
 * em query_feat() a *posição* do cadastro na lista, não o ID gravado (a linha
 * certa está comentada lá). Depois de apagar alguém, os IDs mostrados ficariam
 * errados - justamente o que este projeto precisa que seja estável.
 */
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <list>
#include <vector>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "human_face_detect.hpp"
#include "human_face_recognition.hpp"
#include "face.h"

static const char *TAG = "face";

#define MOUNT_POINT         "/spiflash"
#define DB_PATH             MOUNT_POINT "/faces.bin"
#define DB_TMP_PATH         MOUNT_POINT "/faces.tmp"
#define DB_MAGIC            0x45434146u   /* "FACE" */
#define DB_VERSION          1

/* Semelhança de cosseno mínima para dizer "é o ID x" (mesmo limiar padrão do
 * HumanFaceRecognizer). Subir reduz confusão entre pessoas e aumenta
 * "desconhecido" para a pessoa certa. */
#define RECOG_THR           0.50f
#define MIN_RECOG_FACE_PX   48     /* menor que isso nem tenta reconhecer */
#define MIN_ENROLL_FACE_PX  80     /* cadastro pede rosto razoavelmente perto */

/* Tracking: ver o bloco "Tracking" abaixo. */
#define MAX_TRACKS          8
#define TRACK_IOU           0.30f              /* sobreposição para ser "o mesmo rosto" */
#define MISS_HOLD           1                  /* quadros sem detecção mantendo o track */
#define UNKNOWN_HOLD        3                  /* verificações falhas antes de perder o ID */
#define REVERIFY_US         (500 * 1000)       /* conhecido: re-verifica a cada */
#define RETRY_UNKNOWN_US    (200 * 1000)       /* desconhecido: tenta de novo a cada */
#define SUMMARY_US          (5 * 1000 * 1000)  /* linha "#" de resumo no terminal */

#define AI_TASK_STACK       6144
#define AI_TASK_PRIO        4
#define AI_TASK_CORE        1
/* Reconhecimento no outro core, abaixo da captura (6) e dos streams (5): usa
 * o tempo que sobra no core 0 e nunca atrasa a detecção. */
#define RECOG_TASK_STACK    6144
#define RECOG_TASK_PRIO     3
#define RECOG_TASK_CORE     0

/* ---- Cadastros --------------------------------------------------------------- */

struct person_t {
    uint16_t id;
    float *feat;
};

struct db_header_t {
    uint32_t magic;
    uint16_t version;
    uint16_t feat_len;
    uint16_t next_id;
    uint16_t count;
};

static std::vector<person_t> s_people;
static uint16_t s_next_id = 1;
static int s_feat_len;
static SemaphoreHandle_t s_db_lock;

static bool db_read(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        return false;
    }
    db_header_t h;
    if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != DB_MAGIC || h.version != DB_VERSION ||
        h.feat_len != s_feat_len) {
        ESP_LOGW(TAG, "%s inválido ou de outro modelo - ignorado", path);
        fclose(f);
        return false;
    }
    for (int i = 0; i < h.count; i++) {
        uint16_t id;
        float *feat = (float *)heap_caps_malloc(s_feat_len * sizeof(float), MALLOC_CAP_SPIRAM);
        if (!feat || fread(&id, sizeof(id), 1, f) != 1 ||
            fread(feat, sizeof(float), s_feat_len, f) != (size_t)s_feat_len) {
            heap_caps_free(feat);
            ESP_LOGW(TAG, "%s truncado: %d de %u cadastros lidos", path, i, h.count);
            break;
        }
        s_people.push_back({id, feat});
        s_next_id = std::max<uint16_t>(s_next_id, id + 1);
    }
    s_next_id = std::max(s_next_id, h.next_id);
    fclose(f);
    return true;
}

/* Grava num arquivo temporário e troca: um reset no meio da escrita não perde
 * o banco antigo. Chamar com s_db_lock. */
static esp_err_t db_save()
{
    FILE *f = fopen(DB_TMP_PATH, "wb");
    if (!f) {
        ESP_LOGE(TAG, "não consegui criar %s", DB_TMP_PATH);
        return ESP_FAIL;
    }
    db_header_t h = {DB_MAGIC, DB_VERSION, (uint16_t)s_feat_len, s_next_id, (uint16_t)s_people.size()};
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
    for (const auto &p : s_people) {
        ok = ok && fwrite(&p.id, sizeof(p.id), 1, f) == 1 &&
             fwrite(p.feat, sizeof(float), s_feat_len, f) == (size_t)s_feat_len;
    }
    ok = (fclose(f) == 0) && ok;
    if (!ok) {
        remove(DB_TMP_PATH);
        ESP_LOGE(TAG, "falha gravando os cadastros");
        return ESP_FAIL;
    }
    remove(DB_PATH);
    if (rename(DB_TMP_PATH, DB_PATH) != 0) {
        ESP_LOGE(TAG, "falha renomeando %s", DB_TMP_PATH);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static float dot(const float *a, const float *b, int n)
{
    float s = 0;
    for (int i = 0; i < n; i++) {
        s += a[i] * b[i];
    }
    return s;
}

/* Melhor cadastro acima do limiar, ou -1. Chamar com s_db_lock. */
static int db_match(const float *feat, float *sim_out)
{
    int best_id = -1;
    float best = RECOG_THR;
    for (const auto &p : s_people) {
        float sim = dot(p.feat, feat, s_feat_len);
        if (sim > best) {
            best = sim;
            best_id = p.id;
        }
    }
    *sim_out = best_id >= 0 ? best : 0.0f;
    return best_id;
}

/* Chamar com s_db_lock. Devolve o ID novo ou -1. */
static int db_enroll(const float *feat)
{
    float *copy = (float *)heap_caps_malloc(s_feat_len * sizeof(float), MALLOC_CAP_SPIRAM);
    if (!copy) {
        return -1;
    }
    memcpy(copy, feat, s_feat_len * sizeof(float));
    uint16_t id = s_next_id++;
    s_people.push_back({id, copy});
    if (db_save() != ESP_OK) {
        s_people.pop_back();
        heap_caps_free(copy);
        s_next_id--;
        return -1;
    }
    return id;
}

/* ---- Modelos e quadro --------------------------------------------------------- */

static HumanFaceDetect *s_detect;
static HumanFaceFeat *s_feat;
static int s_w, s_h;
static TaskHandle_t s_task;          /* detecção + tracking (core 1) */
static TaskHandle_t s_recog_task;    /* reconhecimento (core 0) */
static face_release_fn s_release;

/* Quadro em detecção: é o próprio buffer da câmera (sem cópia - copiar 2,4 MB
 * custava ~32 ms por quadro). Fica com a detecção até ela terminar; se o
 * quadro for usado para reconhecimento, passa para a task de reconhecimento,
 * que o devolve. Sempre volta à captura por s_release. */
static uint8_t *s_img;
static int s_img_index;
static std::atomic<bool> s_busy{false};

static dl::image::img_t make_img(uint8_t *data)
{
    dl::image::img_t img;
    img.data = data;
    img.width = (uint16_t)s_w;
    img.height = (uint16_t)s_h;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565LE;
    return img;
}

/* O pós-processamento do esp-dl já entrega o vetor normalizado (o DataBase
 * dele compara só por produto escalar); normalizar de novo é barato e não
 * depende disso. */
static bool extract(const dl::image::img_t &img, const std::vector<int> &keypoint, float *out)
{
    dl::TensorBase *t = s_feat->run(img, keypoint);
    if (!t || t->dtype != dl::DATA_TYPE_FLOAT || t->get_size() != s_feat_len) {
        return false;
    }
    memcpy(out, t->data, s_feat_len * sizeof(float));
    float norm = std::sqrt(dot(out, out, s_feat_len));
    if (norm <= 0) {
        return false;
    }
    for (int i = 0; i < s_feat_len; i++) {
        out[i] /= norm;
    }
    return true;
}

static int box_w(const dl::detect::result_t &d)
{
    return d.box[2] - d.box[0];
}

/* ---- Tracking ------------------------------------------------------------------
 *
 * A detecção roda em todo quadro; o reconhecimento (~120 ms por rosto neste
 * chip, contra ~16-21 ms da detecção) roda em outra task, no outro core, um
 * rosto por vez. Cada rosto vira um track que herda a identidade de um quadro
 * para o outro por sobreposição de caixa (IoU); o ID chega ao track quando o
 * reconhecimento termina. Um track entra na fila de reconhecimento quando:
 *   - é novo (identifica assim que o reconhecimento estiver livre);
 *   - é desconhecido, ou falhou uma verificação: a cada RETRY_UNKNOWN_US;
 *   - é conhecido: re-verifica a cada REVERIFY_US (pega troca de pessoa);
 *   - há um cadastro pedido (o maior rosto, com prioridade).
 */

struct track_t {
    int box[4];
    int id;                 /* -1 = desconhecido */
    float sim;
    int unknown;            /* verificações seguidas sem reconhecer um conhecido */
    int missed;             /* quadros seguidos sem detecção */
    int64_t last_recog_us;  /* último envio para reconhecimento; 0 = nunca */
    uint32_t gen;           /* muda quando o slot é reaproveitado por outro rosto */
    bool pending;           /* reconhecimento em andamento */
    bool live;
};

static track_t s_tracks[MAX_TRACKS];
static uint32_t s_next_gen = 1;
static std::atomic<bool> s_reset_tracks{false};

static float iou(const int *a, const int *b)
{
    int ix = std::min(a[2], b[2]) - std::max(a[0], b[0]);
    int iy = std::min(a[3], b[3]) - std::max(a[1], b[1]);
    if (ix <= 0 || iy <= 0) {
        return 0;
    }
    float inter = (float)ix * iy;
    float area_a = (float)(a[2] - a[0]) * (a[3] - a[1]);
    float area_b = (float)(b[2] - b[0]) * (b[3] - b[1]);
    return inter / (area_a + area_b - inter);
}

/* Liga cada detecção a um track (o de maior IoU, ou um novo). track_of[i] = -1
 * se não sobrou track livre. Tracks sem detecção envelhecem e morrem. */
static void associate(const std::vector<dl::detect::result_t> &dets, int *track_of)
{
    bool matched[MAX_TRACKS] = {};

    for (size_t i = 0; i < dets.size(); i++) {
        const int *box = dets[i].box.data();
        int best = -1;
        float best_iou = TRACK_IOU;
        for (int j = 0; j < MAX_TRACKS; j++) {
            if (s_tracks[j].live && !matched[j]) {
                float v = iou(s_tracks[j].box, box);
                if (v > best_iou) {
                    best_iou = v;
                    best = j;
                }
            }
        }
        if (best < 0) {
            for (int j = 0; j < MAX_TRACKS && best < 0; j++) {
                if (!s_tracks[j].live) {
                    best = j;
                }
            }
            if (best >= 0) {
                s_tracks[best] = {};
                s_tracks[best].live = true;
                s_tracks[best].id = -1;
                s_tracks[best].gen = s_next_gen++;
            }
        }
        track_of[i] = best;
        if (best >= 0) {
            matched[best] = true;
            memcpy(s_tracks[best].box, box, sizeof(s_tracks[best].box));
            s_tracks[best].missed = 0;
        }
    }

    for (int j = 0; j < MAX_TRACKS; j++) {
        if (s_tracks[j].live && !matched[j] && ++s_tracks[j].missed > MISS_HOLD) {
            s_tracks[j].live = false;
        }
    }
}

static bool recog_due(const track_t &t, int64_t now)
{
    if (t.pending) {
        return false;
    }
    if (t.last_recog_us == 0) {
        return true;
    }
    int64_t period = (t.id < 0 || t.unknown > 0) ? RETRY_UNKNOWN_US : REVERIFY_US;
    return now - t.last_recog_us >= period;
}

static void apply_recog(track_t &t, bool ok, int id, float sim)
{
    t.pending = false;
    if (!ok) {
        return;
    }
    if (id >= 0) {
        t.id = id;
        t.sim = sim;
        t.unknown = 0;
    } else if (t.id >= 0 && ++t.unknown > UNKNOWN_HOLD) {
        t.id = -1;
        t.sim = 0;
    }
}

/* ---- Resultado publicado ------------------------------------------------------ */

static SemaphoreHandle_t s_res_lock;
static face_box_t s_res[FACE_MAX_BOXES];
static size_t s_res_n;
static int64_t s_res_time;
static face_stats_t s_stats;

static void publish()
{
    face_box_t out[FACE_MAX_BOXES];
    size_t n = 0;
    for (int j = 0; j < MAX_TRACKS && n < FACE_MAX_BOXES; j++) {
        const track_t &t = s_tracks[j];
        if (t.live) {
            out[n++] = {(int16_t)t.box[0], (int16_t)t.box[1], (int16_t)t.box[2], (int16_t)t.box[3],
                        (int16_t)t.id, (uint8_t)std::clamp((int)(t.sim * 100), 0, 100)};
        }
    }
    xSemaphoreTake(s_res_lock, portMAX_DELAY);
    memcpy(s_res, out, n * sizeof(out[0]));
    s_res_n = n;
    s_res_time = esp_timer_get_time();
    xSemaphoreGive(s_res_lock);
}

/* ---- Pedido de cadastro ------------------------------------------------------- */

static SemaphoreHandle_t s_enroll_lock;   /* um cadastro por vez */
static SemaphoreHandle_t s_enroll_done;
static std::atomic<bool> s_enroll_pending{false};
static face_enroll_result_t s_enroll_result;

static void finish_enroll(face_enroll_result_t r)
{
    s_enroll_result = r;
    xSemaphoreGive(s_enroll_done);
}

/* ---- Reconhecimento (core 0) -------------------------------------------------- */

#define MAX_KEYPOINTS 10   /* 5 pontos (olhos, nariz, cantos da boca) x (x, y) */

struct recog_job_t {
    uint8_t *img;
    int buf_index;
    int keypoint[MAX_KEYPOINTS];
    int keypoint_n;
    int track;
    uint32_t gen;
    bool enroll;
};

struct recog_result_t {
    int track;
    uint32_t gen;
    bool ok;
    int id;
    float sim;
};

static recog_job_t s_job;                       /* escrito pela detecção com s_recog_busy=false */
static std::atomic<bool> s_recog_busy{false};
static QueueHandle_t s_results;                 /* recog_result_t, lidos pela detecção */
static std::atomic<uint32_t> s_recog_count{0};
static std::atomic<uint64_t> s_recog_total_us{0};

static void recog_task(void *)
{
    std::vector<float> feat(s_feat_len);
    std::vector<int> keypoint;

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        recog_job_t job = s_job;
        keypoint.assign(job.keypoint, job.keypoint + job.keypoint_n);

        int64_t t0 = esp_timer_get_time();
        bool ok = extract(make_img(job.img), keypoint, feat.data());
        int64_t t1 = esp_timer_get_time();
        /* A imagem não é mais usada: o buffer volta para a câmera já. É um
         * quadro de ~120 ms atrás, então não serve para a web (fresh=false). */
        s_release(job.buf_index, false);

        recog_result_t r = {job.track, job.gen, ok, -1, 0.0f};
        if (ok) {
            xSemaphoreTake(s_db_lock, portMAX_DELAY);
            r.id = db_match(feat.data(), &r.sim);
            if (job.enroll) {
                if (r.id >= 0) {
                    finish_enroll({FACE_ENROLL_EXISTING, r.id, (uint8_t)(r.sim * 100)});
                } else {
                    int id = db_enroll(feat.data());
                    if (id >= 0) {
                        r.id = id;
                        r.sim = 1.0f;
                        ESP_LOGI(TAG, "cadastrado: ID %d (%u no total)", id, (unsigned)s_people.size());
                        finish_enroll({FACE_ENROLL_NEW, id, 100});
                    } else {
                        finish_enroll({FACE_ENROLL_FAILED, -1, 0});
                    }
                }
            }
            xSemaphoreGive(s_db_lock);
        } else if (job.enroll) {
            finish_enroll({FACE_ENROLL_FAILED, -1, 0});
        }

        s_recog_count.fetch_add(1);
        s_recog_total_us.fetch_add(t1 - t0);
        s_stats.recog_us = (uint32_t)(t1 - t0);
        xQueueSend(s_results, &r, 0);
        s_recog_busy.store(false);
    }
}

/* Entrega o rosto dets[i] (track j) ao reconhecimento. O buffer do quadro vai
 * junto - quem devolve agora é a task de reconhecimento. */
static void submit_recog(const dl::detect::result_t &det, int j, bool enroll, int64_t now)
{
    s_job.img = s_img;
    s_job.buf_index = s_img_index;
    s_job.keypoint_n = std::min((int)det.keypoint.size(), MAX_KEYPOINTS);
    std::copy(det.keypoint.begin(), det.keypoint.begin() + s_job.keypoint_n, s_job.keypoint);
    s_job.track = j;
    s_job.gen = s_tracks[j].gen;
    s_job.enroll = enroll;
    s_tracks[j].pending = true;
    s_tracks[j].last_recog_us = now;
    s_recog_busy.store(true);
    xTaskNotifyGive(s_recog_task);
}

/* ---- Detecção + tracking (core 1) --------------------------------------------- */

static std::atomic<int64_t> s_frame_us{0};     /* quando o quadro em s_img foi capturado */
static std::atomic<uint32_t> s_frames_seen{0}; /* quadros da câmera (oferecidos à IA) */

/* Escolhe quem mandar ao reconhecimento neste quadro (se ele estiver livre):
 * o maior rosto se há cadastro pedido; senão o track "vencido" mais antigo.
 * Devolve o índice em dets, ou -1. */
static int pick_recog(const std::vector<dl::detect::result_t> &dets, const int *track_of, int64_t now,
                      bool *enroll)
{
    *enroll = false;
    if (s_enroll_pending.load() && !dets.empty() && track_of[0] >= 0) {
        if (box_w(dets[0]) < MIN_ENROLL_FACE_PX) {
            if (s_enroll_pending.exchange(false)) {
                finish_enroll({FACE_ENROLL_TOO_SMALL, -1, 0});
            }
        } else if (s_enroll_pending.exchange(false)) {
            /* Pegou o pedido: daqui em diante o timeout do lado HTTP espera o
             * resultado em vez de cancelar. */
            *enroll = true;
            return 0;
        }
    }
    int best = -1;
    for (size_t i = 0; i < dets.size(); i++) {
        int j = track_of[i];
        if (j < 0 || box_w(dets[i]) < MIN_RECOG_FACE_PX || !recog_due(s_tracks[j], now)) {
            continue;
        }
        if (best < 0 || s_tracks[j].last_recog_us < s_tracks[track_of[best]].last_recog_us) {
            best = (int)i;
        }
    }
    return best;
}

/*
 * Saída no terminal, uma linha por rosto em cada quadro analisado:
 *     [id,x,y,last_time_frame]
 * id = -1 se desconhecido; x,y = centro da caixa em pixels do quadro;
 * last_time_frame = ms desde o quadro analisado anterior (o período do
 * tracking). A cada SUMMARY_US, uma linha "#" com taxas e tempos médios.
 */
static void ai_task(void *)
{
    int64_t prev_frame_us = 0;
    int64_t sum_start = esp_timer_get_time();
    uint32_t sum_cycles = 0, sum_faces = 0, sum_seen0 = s_frames_seen.load();
    uint32_t sum_recog0 = s_recog_count.load();
    uint64_t sum_recog_us0 = s_recog_total_us.load();
    uint64_t sum_detect_us = 0, sum_latency_us = 0;
    int no_block = 0;

    while (true) {
        int64_t w0 = esp_timer_get_time();
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        /* Esperar o próximo quadro é o que deixa a IDLE1 rodar. Se a detecção
         * ficar mais lenta que a câmera, o quadro já estará sempre pronto e a
         * task nunca bloqueia: aí cede 1 tick por segundo, pelo watchdog. */
        if (esp_timer_get_time() - w0 < 200) {
            if (++no_block >= 45) {
                vTaskDelay(1);
                no_block = 0;
            }
        } else {
            no_block = 0;
        }
        int64_t frame_us = s_frame_us.load();

        if (s_reset_tracks.exchange(false)) {
            for (auto &t : s_tracks) {
                t.live = false;
            }
        }

        /* Resultados de reconhecimento que chegaram desde o último quadro. O
         * gen descarta resultados de um track que morreu e teve o slot
         * reaproveitado. */
        recog_result_t r;
        while (xQueueReceive(s_results, &r, 0) == pdTRUE) {
            if (r.track >= 0 && r.track < MAX_TRACKS && s_tracks[r.track].gen == r.gen) {
                apply_recog(s_tracks[r.track], r.ok, r.id, r.sim);
            }
        }

        int64_t t0 = esp_timer_get_time();
        std::list<dl::detect::result_t> &found = s_detect->run(make_img(s_img));
        std::vector<dl::detect::result_t> dets(found.begin(), found.end());
        int64_t t1 = esp_timer_get_time();

        for (auto &d : dets) {
            d.limit_box(s_w, s_h);
            d.limit_keypoint(s_w, s_h);
        }
        std::sort(dets.begin(), dets.end(),
                  [](const dl::detect::result_t &a, const dl::detect::result_t &b) { return a.box_area() > b.box_area(); });
        if (dets.size() > MAX_TRACKS) {
            dets.resize(MAX_TRACKS);
        }

        int track_of[MAX_TRACKS];
        associate(dets, track_of);

        bool gave_frame = false;
        if (!s_recog_busy.load()) {
            bool enroll;
            int i = pick_recog(dets, track_of, t1, &enroll);
            if (i >= 0) {
                submit_recog(dets[i], track_of[i], enroll, t1);
                gave_frame = true;
            }
        }

        /* Publica antes de devolver o buffer: se a captura for desenhar as
         * caixas neste mesmo quadro para a web, desenha as dele. */
        publish();
        if (!gave_frame) {
            s_release(s_img_index, true);
        }
        int64_t done_us = esp_timer_get_time();

        int dt_ms = prev_frame_us ? (int)((frame_us - prev_frame_us) / 1000) : 0;
        prev_frame_us = frame_us;
        for (size_t i = 0; i < dets.size(); i++) {
            if (track_of[i] < 0) {
                continue;
            }
            const track_t &t = s_tracks[track_of[i]];
            printf("[%d,%d,%d,%d]\n", t.id, (t.box[0] + t.box[2]) / 2, (t.box[1] + t.box[3]) / 2, dt_ms);
        }

        s_stats.cycles++;
        s_stats.detect_us = (uint32_t)(t1 - t0);

        sum_cycles++;
        sum_faces += dets.size();
        sum_detect_us += t1 - t0;
        sum_latency_us += done_us - frame_us;
        if (done_us - sum_start >= SUMMARY_US) {
            float secs = (done_us - sum_start) / 1e6f;
            uint32_t seen = s_frames_seen.load();
            uint32_t recogs = s_recog_count.load() - sum_recog0;
            uint64_t recog_us = s_recog_total_us.load() - sum_recog_us0;
            printf("# IA %.1f quadros/s (camera %.1f) | deteccao %.1f ms | latencia %.0f ms | "
                   "rostos/quadro %.1f | reconhecimento %.0f ms cada, %.1f/s (em paralelo)\n",
                   sum_cycles / secs, (seen - sum_seen0) / secs, sum_detect_us / 1000.0f / sum_cycles,
                   sum_latency_us / 1000.0f / sum_cycles, (float)sum_faces / sum_cycles,
                   recogs ? recog_us / 1000.0f / recogs : 0.0f, recogs / secs);
            sum_start = done_us;
            sum_cycles = sum_faces = 0;
            sum_detect_us = sum_latency_us = 0;
            sum_seen0 = seen;
            sum_recog0 += recogs;
            sum_recog_us0 += recog_us;
        }

        s_busy.store(false);
    }
}

/* ---- API ---------------------------------------------------------------------- */

esp_err_t face_init(int width, int height, face_release_fn release)
{
    esp_vfs_fat_mount_config_t mount = {};
    mount.format_if_mount_failed = true;
    mount.max_files = 4;
    mount.allocation_unit_size = CONFIG_WL_SECTOR_SIZE;
    wl_handle_t wl;
    ESP_RETURN_ON_ERROR(esp_vfs_fat_spiflash_mount_rw_wl(MOUNT_POINT, "storage", &mount, &wl), TAG,
                        "falha ao montar a partição storage");

    ESP_RETURN_ON_FALSE(release, ESP_ERR_INVALID_ARG, TAG, "release é obrigatório");
    s_w = width;
    s_h = height;
    s_release = release;

    s_db_lock = xSemaphoreCreateMutex();
    s_res_lock = xSemaphoreCreateMutex();
    s_enroll_lock = xSemaphoreCreateMutex();
    s_enroll_done = xSemaphoreCreateBinary();
    ESP_RETURN_ON_FALSE(s_db_lock && s_res_lock && s_enroll_lock && s_enroll_done, ESP_ERR_NO_MEM, TAG,
                        "sem memória para os locks");

    /* Carrega os modelos agora (lazy_load=false): erro aparece no boot, e o
     * primeiro quadro não paga a carga. */
    int64_t t0 = esp_timer_get_time();
    s_detect = new HumanFaceDetect(static_cast<HumanFaceDetect::model_type_t>(CONFIG_DEFAULT_HUMAN_FACE_DETECT_MODEL),
                                   false);
    s_feat = new HumanFaceFeat(static_cast<HumanFaceFeat::model_type_t>(CONFIG_DEFAULT_HUMAN_FACE_FEAT_MODEL), false);
    s_feat_len = s_feat->get_feat_len();
    ESP_LOGI(TAG, "modelos carregados em %d ms (vetor de %d floats)",
             (int)((esp_timer_get_time() - t0) / 1000), s_feat_len);

    if (!db_read(DB_PATH) && db_read(DB_TMP_PATH)) {
        /* Reset entre apagar o antigo e renomear o novo: o temporário vale. */
        rename(DB_TMP_PATH, DB_PATH);
    }
    ESP_LOGI(TAG, "%u rosto(s) cadastrado(s), próximo ID %u", (unsigned)s_people.size(), s_next_id);

    s_results = xQueueCreate(4, sizeof(recog_result_t));
    ESP_RETURN_ON_FALSE(s_results, ESP_ERR_NO_MEM, TAG, "sem memória para a fila de resultados");
    BaseType_t ok = xTaskCreatePinnedToCore(recog_task, "face_recog", RECOG_TASK_STACK, nullptr, RECOG_TASK_PRIO,
                                            &s_recog_task, RECOG_TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "falha ao criar a task de reconhecimento");
    ok = xTaskCreatePinnedToCore(ai_task, "face_ai", AI_TASK_STACK, nullptr, AI_TASK_PRIO, &s_task, AI_TASK_CORE);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "falha ao criar a task da IA");
    return ESP_OK;
}

bool face_offer_frame(uint8_t *rgb565, int buf_index)
{
    s_frames_seen.fetch_add(1);
    if (!s_task || s_busy.load()) {
        return false;
    }
    s_frame_us.store(esp_timer_get_time());
    s_img = rgb565;
    s_img_index = buf_index;
    s_busy.store(true);
    xTaskNotifyGive(s_task);
    return true;
}

size_t face_get_boxes(face_box_t *out, size_t max, uint32_t max_age_ms)
{
    if (!s_res_lock) {
        return 0;
    }
    xSemaphoreTake(s_res_lock, portMAX_DELAY);
    size_t n = 0;
    if (esp_timer_get_time() - s_res_time <= (int64_t)max_age_ms * 1000) {
        n = std::min(max, s_res_n);
        memcpy(out, s_res, n * sizeof(out[0]));
    }
    xSemaphoreGive(s_res_lock);
    return n;
}

face_enroll_result_t face_enroll(uint32_t timeout_ms)
{
    face_enroll_result_t r = {FACE_ENROLL_FAILED, -1, 0};
    if (!s_task) {
        return r;   /* face_init falhou */
    }
    if (xSemaphoreTake(s_enroll_lock, 0) != pdTRUE) {
        r.status = FACE_ENROLL_BUSY;
        return r;
    }
    xSemaphoreTake(s_enroll_done, 0);   /* descarta sinal de um pedido anterior */
    s_enroll_pending.store(true);

    if (xSemaphoreTake(s_enroll_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        r = s_enroll_result;
    } else if (s_enroll_pending.exchange(false)) {
        r.status = FACE_ENROLL_NO_FACE;     /* cancelado antes da IA pegar */
    } else {
        xSemaphoreTake(s_enroll_done, portMAX_DELAY);   /* a IA já está gravando */
        r = s_enroll_result;
    }
    xSemaphoreGive(s_enroll_lock);
    return r;
}

esp_err_t face_delete(int id)
{
    if (!s_db_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_db_lock, portMAX_DELAY);
    bool found = false;
    for (auto it = s_people.begin(); it != s_people.end();) {
        if (id < 0 || it->id == id) {
            heap_caps_free(it->feat);
            it = s_people.erase(it);
            found = true;
        } else {
            ++it;
        }
    }
    if (id < 0) {
        s_next_id = 1;
    }
    esp_err_t err = (found || id < 0) ? db_save() : ESP_ERR_NOT_FOUND;
    size_t left = s_people.size();
    xSemaphoreGive(s_db_lock);

    if (err == ESP_OK) {
        s_reset_tracks.store(true);
        if (id < 0) {
            ESP_LOGI(TAG, "todos os cadastros apagados");
        } else {
            ESP_LOGI(TAG, "ID %d apagado (%u restantes)", id, (unsigned)left);
        }
    }
    return err;
}

size_t face_list(int *ids, size_t max)
{
    if (!s_db_lock) {
        return 0;
    }
    xSemaphoreTake(s_db_lock, portMAX_DELAY);
    size_t n = s_people.size();
    for (size_t i = 0; i < n && i < max; i++) {
        ids[i] = s_people[i].id;
    }
    xSemaphoreGive(s_db_lock);
    return n;
}

void face_get_stats(face_stats_t *out)
{
    *out = s_stats;
}
