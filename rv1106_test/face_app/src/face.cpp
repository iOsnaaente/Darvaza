/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ver face.h. Porta do face.cpp do imx708_capture (ESP32-P4): mesmo tracking,
 * mesmo agendamento do reconhecimento e mesmo arquivo de cadastros. A
 * diferença é o custo: aqui o reconhecimento leva ~5 ms na NPU (no P4 eram
 * ~120 ms), então roda na própria thread da IA, até MAX_RECOG_PER_FRAME
 * rostos por quadro, em vez de numa task separada.
 */
#include "face.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>
#include <unistd.h>
#include <vector>

#include "models.h"
#include "util.h"

static const char *TAG = "face";

#define DB_MAGIC            0x45434146u   /* "FACE" */
#define DB_VERSION          1

#define MIN_RECOG_FACE_PX   48     /* menor que isso nem tenta reconhecer */
#define MIN_ENROLL_FACE_PX  80     /* cadastro pede rosto razoavelmente perto */

/* Tracking: ver o bloco "Tracking" abaixo. */
#define MAX_TRACKS          8
#define TRACK_IOU           0.30f              /* sobreposição para ser "o mesmo rosto" */
#define MISS_HOLD           1                  /* quadros sem detecção mantendo o track */
#define UNKNOWN_HOLD        3                  /* verificações falhas antes de perder o ID */
#define REVERIFY_US         (500 * 1000)       /* conhecido: re-verifica a cada */
#define RETRY_UNKNOWN_US    (200 * 1000)       /* desconhecido: tenta de novo a cada */
#define MAX_RECOG_PER_FRAME 2
#define SUMMARY_US          (5 * 1000 * 1000)  /* linha "#" de resumo no terminal */

static float s_thr;

/* ---- Cadastros --------------------------------------------------------------- */

struct person_t {
    uint16_t id;
    std::vector<float> feat;
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
static std::string s_db_path, s_db_tmp;
static std::mutex s_db_lock;

static bool db_read(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    db_header_t h;
    if (fread(&h, sizeof(h), 1, f) != 1 || h.magic != DB_MAGIC || h.version != DB_VERSION ||
        h.feat_len != s_feat_len) {
        LOGW(TAG, "%s inválido ou de outro modelo - ignorado", path.c_str());
        fclose(f);
        return false;
    }
    for (int i = 0; i < h.count; i++) {
        person_t p;
        p.feat.resize(s_feat_len);
        if (fread(&p.id, sizeof(p.id), 1, f) != 1 ||
            fread(p.feat.data(), sizeof(float), s_feat_len, f) != (size_t)s_feat_len) {
            LOGW(TAG, "%s truncado: %d de %u cadastros lidos", path.c_str(), i, h.count);
            break;
        }
        s_next_id = std::max<uint16_t>(s_next_id, p.id + 1);
        s_people.push_back(std::move(p));
    }
    s_next_id = std::max(s_next_id, h.next_id);
    fclose(f);
    return true;
}

/* Grava num arquivo temporário e troca (rename é atômico): faltar energia no
 * meio da escrita não perde o banco antigo. Chamar com s_db_lock. */
static bool db_save()
{
    FILE *f = fopen(s_db_tmp.c_str(), "wb");
    if (!f) {
        LOGE(TAG, "não consegui criar %s: %s", s_db_tmp.c_str(), strerror(errno));
        return false;
    }
    db_header_t h = {DB_MAGIC, DB_VERSION, (uint16_t)s_feat_len, s_next_id, (uint16_t)s_people.size()};
    bool ok = fwrite(&h, sizeof(h), 1, f) == 1;
    for (const auto &p : s_people) {
        ok = ok && fwrite(&p.id, sizeof(p.id), 1, f) == 1 &&
             fwrite(p.feat.data(), sizeof(float), s_feat_len, f) == (size_t)s_feat_len;
    }
    ok = ok && fflush(f) == 0 && fsync(fileno(f)) == 0;
    ok = (fclose(f) == 0) && ok;
    if (!ok || rename(s_db_tmp.c_str(), s_db_path.c_str()) != 0) {
        remove(s_db_tmp.c_str());
        LOGE(TAG, "falha gravando os cadastros em %s", s_db_path.c_str());
        return false;
    }
    return true;
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
    float best = s_thr;
    for (const auto &p : s_people) {
        float sim = dot(p.feat.data(), feat, s_feat_len);
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
    uint16_t id = s_next_id++;
    s_people.push_back({id, std::vector<float>(feat, feat + s_feat_len)});
    if (!db_save()) {
        s_people.pop_back();
        s_next_id--;
        return -1;
    }
    return id;
}

/* ---- Modelos e quadro --------------------------------------------------------- */

static RetinaFace s_detect;
static FaceNet s_facenet;
static face_release_fn s_release;
static std::atomic<bool> s_running{false};

/* Próximo quadro a analisar: o próprio buffer da câmera (sem cópia). Se a IA
 * está ocupada, cada quadro novo substitui o que esperava (o velho volta para
 * a câmera); ao terminar, ela pega o mais novo na hora, sem esperar o sensor.
 * Fica com a IA até ela publicar o resultado e volta à captura por s_release. */
static std::mutex s_frame_lock;
static std::condition_variable s_frame_cv;
static FramePtr s_img;
static std::atomic<uint32_t> s_frames_seen{0};   /* quadros da câmera (oferecidos à IA) */

/* ---- Tracking ------------------------------------------------------------------
 *
 * A detecção roda em todo quadro; cada rosto vira um track que herda a
 * identidade de um quadro para o outro por sobreposição de caixa (IoU). Um
 * track vai para o reconhecimento quando:
 *   - é novo (identifica na hora);
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
    int64_t last_recog_us;  /* último reconhecimento; 0 = nunca */
    int64_t since_us;       /* quadro em que o rosto apareceu */
    uint32_t gen;           /* muda quando o slot é reaproveitado por outro rosto */
    bool live;
};

static track_t s_tracks[MAX_TRACKS];
static uint32_t s_next_gen = 1;
static std::atomic<bool> s_reset_tracks{false};

static float box_iou(const int *a, const int *b)
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
static void associate(const std::vector<Det> &dets, int *track_of, int64_t frame_us)
{
    bool matched[MAX_TRACKS] = {};

    for (size_t i = 0; i < dets.size(); i++) {
        const int *box = dets[i].box;
        int best = -1;
        float best_iou = TRACK_IOU;
        for (int j = 0; j < MAX_TRACKS; j++) {
            if (s_tracks[j].live && !matched[j]) {
                float v = box_iou(s_tracks[j].box, box);
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
                s_tracks[best].since_us = frame_us;
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
    if (t.last_recog_us == 0) {
        return true;
    }
    int64_t period = (t.id < 0 || t.unknown > 0) ? RETRY_UNKNOWN_US : REVERIFY_US;
    return now - t.last_recog_us >= period;
}

static void apply_recog(track_t &t, bool ok, int id, float sim)
{
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

static std::mutex s_res_lock;
static face_result_t s_res;
static int64_t s_res_time;
static face_stats_t s_stats;

static void publish(int64_t frame_us)
{
    face_result_t out = {};
    for (int j = 0; j < MAX_TRACKS && out.n < FACE_MAX_BOXES; j++) {
        const track_t &t = s_tracks[j];
        if (t.live) {
            out.box[out.n++] = {(int16_t)t.box[0], (int16_t)t.box[1], (int16_t)t.box[2], (int16_t)t.box[3],
                                (int16_t)t.id, (uint8_t)std::clamp((int)(t.sim * 100), 0, 100), t.gen,
                                t.since_us};
        }
    }
    out.frame_us = frame_us;
    std::lock_guard<std::mutex> lock(s_res_lock);
    out.seq = s_res.seq + 1;
    s_res = out;
    s_res_time = now_us();
}

/* ---- Pedido de cadastro ------------------------------------------------------- */

static std::mutex s_enroll_lock;   /* um cadastro por vez */
static std::mutex s_enroll_mx;
static std::condition_variable s_enroll_cv;
static bool s_enroll_done;
static face_enroll_result_t s_enroll_result;
static std::atomic<bool> s_enroll_pending{false};

static void finish_enroll(face_enroll_result_t r)
{
    std::lock_guard<std::mutex> lock(s_enroll_mx);
    s_enroll_result = r;
    s_enroll_done = true;
    s_enroll_cv.notify_all();
}

/* ---- Reconhecimento ----------------------------------------------------------- */

static uint32_t s_recog_count;
static uint64_t s_recog_total_us;

static void recognize(const Frame &img, const Det &det, track_t &t, bool enroll, int64_t now)
{
    std::vector<float> feat(s_feat_len);
    int64_t t0 = now_us();
    bool ok = s_facenet.extract(img, det, feat.data());
    int64_t t1 = now_us();
    t.last_recog_us = now;

    int id = -1;
    float sim = 0;
    if (ok) {
        std::lock_guard<std::mutex> lock(s_db_lock);
        id = db_match(feat.data(), &sim);
        if (enroll) {
            if (id >= 0) {
                finish_enroll({FACE_ENROLL_EXISTING, id, (uint8_t)(sim * 100)});
            } else {
                int nid = db_enroll(feat.data());
                if (nid >= 0) {
                    id = nid;
                    sim = 1.0f;
                    LOGI(TAG, "cadastrado: ID %d (%u no total)", nid, (unsigned)s_people.size());
                    finish_enroll({FACE_ENROLL_NEW, nid, 100});
                } else {
                    finish_enroll({FACE_ENROLL_FAILED, -1, 0});
                }
            }
        }
    } else if (enroll) {
        finish_enroll({FACE_ENROLL_FAILED, -1, 0});
    }
    apply_recog(t, ok, id, sim);

    s_recog_count++;
    s_recog_total_us += t1 - t0;
    std::lock_guard<std::mutex> lock(s_res_lock);
    s_stats.recogs = s_recog_count;
    s_stats.recog_us = (uint32_t)(t1 - t0);
}

/* Escolhe quem reconhecer agora: o maior rosto se há cadastro pedido; senão o
 * track "vencido" há mais tempo. Devolve o índice em dets, ou -1. */
static int pick_recog(const std::vector<Det> &dets, const int *track_of, int64_t now, bool *enroll)
{
    *enroll = false;
    if (s_enroll_pending.load() && !dets.empty() && track_of[0] >= 0) {
        if (dets[0].box[2] - dets[0].box[0] < MIN_ENROLL_FACE_PX) {
            if (s_enroll_pending.exchange(false)) {
                finish_enroll({FACE_ENROLL_TOO_SMALL, -1, 0});
            }
        } else if (s_enroll_pending.exchange(false)) {
            /* Pegou o pedido: daqui em diante o lado HTTP espera o resultado
             * em vez de cancelar. */
            *enroll = true;
            return 0;
        }
    }
    int best = -1;
    for (size_t i = 0; i < dets.size(); i++) {
        int j = track_of[i];
        if (j < 0 || dets[i].box[2] - dets[i].box[0] < MIN_RECOG_FACE_PX || !recog_due(s_tracks[j], now)) {
            continue;
        }
        if (best < 0 || s_tracks[j].last_recog_us < s_tracks[track_of[best]].last_recog_us) {
            best = (int)i;
        }
    }
    return best;
}

/* ---- Detecção + tracking ------------------------------------------------------ */

/*
 * Saída no terminal, uma linha por rosto em cada quadro analisado:
 *     [ID,X,Y,dX,dY,time_detected]
 * ID = -1 se desconhecido; X,Y = centro da caixa e dX,dY = largura e altura,
 * em pixels do quadro; time_detected = ms desde o boot em que o quadro foi
 * capturado (a diferença entre duas linhas é o período do tracking). A cada
 * SUMMARY_US, uma linha "#" com taxas e tempos médios.
 */
static void ai_thread()
{
    out_line("# rostos: [ID,X,Y,dX,dY,time_detected]  ID -1 = desconhecido | X,Y = centro (px) | "
             "dX,dY = largura,altura (px) | time_detected = ms desde o boot\n");
    int64_t sum_start = now_us();
    uint32_t sum_cycles = 0, sum_faces = 0, sum_seen0 = s_frames_seen.load();
    uint32_t sum_recog0 = 0;
    uint64_t sum_recog_us0 = 0, sum_detect_us = 0, sum_latency_us = 0;
    std::vector<Det> dets;
    std::string lines;

    while (true) {
        FramePtr img;
        {
            std::unique_lock<std::mutex> lock(s_frame_lock);
            s_frame_cv.wait(lock, [] { return s_img != nullptr; });
            img = std::move(s_img);
        }
        const int64_t frame_us = img->t_us;

        if (s_reset_tracks.exchange(false)) {
            for (auto &t : s_tracks) {
                t.live = false;
            }
        }

        int64_t t0 = now_us();
        if (!s_detect.detect(*img, dets, MAX_TRACKS)) {
            dets.clear();
        }
        int64_t t1 = now_us();

        int track_of[MAX_TRACKS];
        associate(dets, track_of, frame_us);

        for (int n = 0; n < MAX_RECOG_PER_FRAME; n++) {
            bool enroll;
            int i = pick_recog(dets, track_of, t1, &enroll);
            if (i < 0) {
                break;
            }
            recognize(*img, dets[i], s_tracks[track_of[i]], enroll, t1);
        }

        /* Publica antes de devolver o quadro: a captura recorta as caixas
         * deste mesmo quadro para a web, então elas têm de ser as dele. */
        publish(frame_us);
        s_release(img);
        img.reset();
        int64_t done_us = now_us();

        lines.clear();
        for (size_t i = 0; i < dets.size(); i++) {
            if (track_of[i] < 0) {
                continue;
            }
            const track_t &t = s_tracks[track_of[i]];
            appendf(lines, "[%d,%d,%d,%d,%d,%lld]\n", t.id, (t.box[0] + t.box[2]) / 2, (t.box[1] + t.box[3]) / 2,
                    t.box[2] - t.box[0], t.box[3] - t.box[1], (long long)(frame_us / 1000));
        }
        if (!lines.empty()) {
            out_line("%s", lines.c_str());
        }

        {
            std::lock_guard<std::mutex> lock(s_res_lock);
            s_stats.cycles++;
            s_stats.detect_us = (uint32_t)(t1 - t0);
            s_stats.latency_us = (uint32_t)(done_us - frame_us);
        }

        sum_cycles++;
        sum_faces += dets.size();
        sum_detect_us += t1 - t0;
        sum_latency_us += done_us - frame_us;
        if (done_us - sum_start >= SUMMARY_US) {
            float secs = (done_us - sum_start) / 1e6f;
            uint32_t seen = s_frames_seen.load();
            uint32_t recogs = s_recog_count - sum_recog0;
            uint64_t recog_us = s_recog_total_us - sum_recog_us0;
            out_line("# IA %.1f quadros/s (camera %.1f) | deteccao %.1f ms | latencia %.0f ms | "
                     "rostos/quadro %.1f | reconhecimento %.0f ms cada, %.1f/s\n",
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
    }
}

/* ---- API ---------------------------------------------------------------------- */

bool face_init(int width, int height, const std::string &model_dir, const std::string &db_path, float thr,
               face_release_fn release)
{
    (void)width;
    (void)height;
    s_release = std::move(release);
    s_thr = thr;
    s_db_path = db_path;
    s_db_tmp = db_path + ".tmp";

    int64_t t0 = now_us();
    if (!s_detect.load(model_dir + "RetinaFace.rknn") || !s_facenet.load(model_dir + "mobilefacenet.rknn")) {
        return false;
    }
    s_feat_len = s_facenet.feat_len();
    LOGI(TAG, "modelos carregados em %d ms (vetor de %d floats)", (int)((now_us() - t0) / 1000), s_feat_len);

    if (!db_read(s_db_path) && db_read(s_db_tmp)) {
        /* Caiu entre gravar o temporário e renomear: o temporário vale. */
        rename(s_db_tmp.c_str(), s_db_path.c_str());
    }
    LOGI(TAG, "%u rosto(s) cadastrado(s) em %s, próximo ID %u, limiar %.2f", (unsigned)s_people.size(),
         s_db_path.c_str(), s_next_id, s_thr);

    std::thread(ai_thread).detach();
    s_running = true;
    return true;
}

bool face_offer_frame(const FramePtr &frame)
{
    s_frames_seen.fetch_add(1);
    if (!s_running) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s_frame_lock);
        s_img = frame;
    }
    s_frame_cv.notify_one();
    return true;
}

void face_get_result(face_result_t *out, uint32_t max_age_ms)
{
    std::lock_guard<std::mutex> lock(s_res_lock);
    *out = s_res;
    if (now_us() - s_res_time > (int64_t)max_age_ms * 1000) {
        out->n = 0;
    }
}

face_enroll_result_t face_enroll(uint32_t timeout_ms)
{
    face_enroll_result_t r = {FACE_ENROLL_FAILED, -1, 0};
    if (!s_running) {
        return r;   /* face_init falhou */
    }
    std::unique_lock<std::mutex> one(s_enroll_lock, std::try_to_lock);
    if (!one.owns_lock()) {
        r.status = FACE_ENROLL_BUSY;
        return r;
    }
    std::unique_lock<std::mutex> lock(s_enroll_mx);
    s_enroll_done = false;
    s_enroll_pending.store(true);

    if (s_enroll_cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [] { return s_enroll_done; })) {
        r = s_enroll_result;
    } else if (s_enroll_pending.exchange(false)) {
        r.status = FACE_ENROLL_NO_FACE;     /* cancelado antes da IA pegar */
    } else {
        s_enroll_cv.wait(lock, [] { return s_enroll_done; });   /* a IA já está gravando */
        r = s_enroll_result;
    }
    return r;
}

int face_delete(int id)
{
    std::lock_guard<std::mutex> lock(s_db_lock);
    size_t before = s_people.size();
    s_people.erase(std::remove_if(s_people.begin(), s_people.end(),
                                  [id](const person_t &p) { return id < 0 || p.id == id; }),
                   s_people.end());
    bool found = s_people.size() != before;
    if (id < 0) {
        s_next_id = 1;
    } else if (!found) {
        return -1;
    }
    if (!db_save()) {
        return -2;
    }
    s_reset_tracks.store(true);
    if (id < 0) {
        LOGI(TAG, "todos os cadastros apagados");
    } else {
        LOGI(TAG, "ID %d apagado (%u restantes)", id, (unsigned)s_people.size());
    }
    return 0;
}

size_t face_list(int *ids, size_t max)
{
    std::lock_guard<std::mutex> lock(s_db_lock);
    size_t n = s_people.size();
    for (size_t i = 0; i < n && i < max; i++) {
        ids[i] = s_people[i].id;
    }
    return n;
}

void face_get_stats(face_stats_t *out)
{
    std::lock_guard<std::mutex> lock(s_res_lock);
    *out = s_stats;
}
