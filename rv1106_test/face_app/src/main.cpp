/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * MIS5001 + reconhecimento facial na Luckfox Pico Max (RV1106), ao vivo no
 * navegador. Porta do imx708_capture do ESP32-P4: mesma página, mesmos
 * endpoints, mesma saída no terminal.
 *
 *   GET  /              página: caixas ao vivo, recorte de cada rosto, dados
 *   GET  /events        Server-Sent Events (text/event-stream):
 *                         faces - caixas e IDs, a cada quadro analisado
 *                         crop  - JPEG (base64) só da caixa de cada rosto
 *                         stats - o mesmo JSON do /status, 1 por segundo
 *   GET  /events?view=full  o mesmo, sem "crop" (a imagem vem do /frame.jpg)
 *   GET  /frame.jpg?after=N  o próximo quadro inteiro depois do N, em JPEG;
 *                         as caixas dele vão no cabeçalho X-Faces
 *   GET  /capture.jpg   uma foto do quadro inteiro, com as caixas (sob demanda)
 *   GET  /status        contadores, rostos na imagem e IDs cadastrados (JSON)
 *   GET  /quality?q=N   qualidade JPEG ao vivo (1..100)
 *   POST /enroll        cadastra o maior rosto da imagem; devolve o ID
 *   POST /delete?id=N   apaga um cadastro (id=all apaga todos)
 *   POST /gaze/calib    o maior rosto, olhando para a câmera, vira o olhar 0,0
 *                       (?reset=1 tira a calibração)
 *   GET  /asr ...       medição de reconhecimento de fala (asr.h)
 *
 * Pipeline: sensor -> ISP + 3A (rkaiq) -> VI NV12 1280x960 (camera.cpp) -> IA
 * (no próprio buffer da câmera; o RGA reduz para a NPU) -> recorte de cada
 * caixa -> JPEG por hardware (MPP) -> "últimos recortes" publicados. Por
 * padrão o quadro inteiro não vai para a rede: a página recebe as caixas
 * (poucos bytes, a cada quadro) e desenha ela mesma, e recebe a imagem só de
 * dentro das caixas. Quem quer a imagem inteira pede um quadro por vez ao
 * /frame.jpg, e só pede o próximo quando o anterior chegou: nunca há mais de
 * um quadro na rede, então a imagem não acumula atraso nos buffers do caminho
 * (TCP, adb). Cada quadro é codificado só quando pedido, numa thread própria
 * para não atrasar a captura.
 *
 * A IA (face.cpp) roda numa thread própria, no ritmo dela; a captura nunca
 * espera por ela e usa sempre o resultado mais recente.
 */
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <ifaddrs.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>

#include "asr.h"
#include "camera.h"
#include "face.h"
#include "http.h"
#include "overlay.h"
#include "util.h"

static const char *TAG = "face_app";

/* Vale para os recortes e para a foto. */
#define JPEG_QUALITY_DEFAULT 60

/* Recortes por segundo para a web. As caixas vão a cada quadro analisado; a
 * imagem de dentro delas não precisa de tanto, e cada recorte custa banda. */
#define CROP_MAX_FPS        15
/* Lado maior do recorte enviado (múltiplo de 16). Rosto maior que isso vai
 * reduzido pulando pixels: a banda fica limitada mesmo com o rosto colado. */
#define CROP_MAX_PX         256

/* Imagem inteira: no máximo isto de quadros por segundo (cada um custa ~30 ms
 * de RGA + JPEG; o resto do tempo é da IA). O ritmo real é o da rede: a
 * página pede o próximo quando o anterior chega. */
#define FRAME_MAX_FPS       15
/* Quadro pronto mais velho que isto não serve para quem pede: espera um novo. */
#define FRAME_FRESH_MS      300

#define MAX_VIEWERS         2
#define EVENTS_POLL_MS      5       /* de quanto em quanto o espectador olha se há novidade */
#define STATS_PERIOD_US     (1000 * 1000)

/* Quanto a foto (/capture.jpg) e a imagem inteira (/frame.jpg) esperam um quadro novo. */
#define FRAME_WAIT_MS       3000

/* ---- Caixas (as desenhadas na foto; a página desenha as dela) --------------- */
#define BOX_THICK           3
#define LABEL_SCALE         3       /* fonte 5x7 x3 = 21 px de altura */
#define BOX_MAX_AGE_MS      600     /* IA parada há mais que isso: sem caixas */
static const cv::Scalar COLOR_KNOWN(0, 220, 0);      /* BGR */
static const cv::Scalar COLOR_UNKNOWN(0, 140, 255);
static const cv::Scalar COLOR_TEXT(0, 0, 0);

/* Quanto o /enroll espera um rosto aparecer. */
#define ENROLL_TIMEOUT_MS   4000

struct options_t {
    int port = 80;
    int width = 1280;
    int height = 960;
    int power_hz = 60;
    int fps = 30;
    float thr = 0.60f;
    std::string db = "/userdata/faces.bin";
    std::string models;   /* padrão: <pasta do executável>/model/ */
    std::string asr = "/root/asr_bench";
};

/* ---- Estado ---------------------------------------------------------------- */

static std::atomic<bool> s_stop{false};
static bool s_camera_ok;
/* Medição de fala pediu a CPU só para ela: a captura segue, a IA não recebe quadros. */
static std::atomic<bool> s_ai_paused{false};
static int s_w, s_h;

static std::atomic<int> s_quality{JPEG_QUALITY_DEFAULT};
/* Só faz JPEG quando alguém quer: recortes com espectadores no /events, a
 * foto quando pedida. A câmera e a IA rodam sempre: AE/AWB ficam convergidos
 * e o terminal continua mostrando os rostos sem ninguém na página. */
static std::atomic<int> s_photo_waiters{0};
static std::atomic<int> s_viewers{0};
static std::atomic<int> s_full_viewers{0};  /* dos s_viewers, os da imagem inteira */

static struct {
    std::atomic<uint32_t> captured{0};
    std::atomic<uint32_t> crop_sets{0};     /* conjuntos de recortes publicados */
    std::atomic<uint32_t> crop_bytes{0};    /* tamanho do último conjunto (eventos com base64) */
    std::atomic<uint32_t> crop_us{0};       /* último conjunto: cópia + JPEG + base64 */
    std::atomic<uint32_t> frame_sets{0};    /* quadros inteiros codificados */
    std::atomic<uint32_t> frame_bytes{0};   /* JPEG do último */
    std::atomic<uint32_t> frame_us{0};      /* último: conversão + JPEG */
    std::atomic<uint32_t> frames_sent{0};   /* respostas do /frame.jpg */
    std::atomic<uint32_t> photos{0};
    std::atomic<uint32_t> encode_failed{0};
    std::atomic<uint64_t> bytes_sent{0};    /* tudo que saiu pelo /events */
} s_stats;

/* Quadros que a IA terminou de analisar, para a captura recortar. */
static std::mutex s_rel_lock;
static std::deque<FramePtr> s_released;

/* Recortes: o conjunto de eventos SSE "crop" de um quadro, prontos (o base64
 * é feito uma vez, não uma vez por espectador). */
static std::mutex s_crops_lock;
static std::string s_crops_latest;
static std::atomic<uint32_t> s_crops_seq{0};

/* Imagem inteira. s_frame_want = alguém pediu um quadro novo: a captura passa
 * o próximo quadro analisado (com as caixas dele) por s_full_frame, e
 * full_frame_loop o converte, codifica e publica em s_frame_*. */
static std::atomic<bool> s_frame_want{false};
static std::mutex s_full_lock;
static std::condition_variable s_full_cv;
static FramePtr s_full_frame;
static face_result_t s_full_res;

static std::mutex s_frame_lock;
static std::condition_variable s_frame_cv;
static std::vector<uchar> s_frame_jpg;
static std::string s_frame_faces;   /* caixas daquele quadro, JSON como no "faces" */
static int64_t s_frame_t_us;        /* captura do quadro */
static int64_t s_frame_pub_us;      /* quando ficou pronto */
static uint32_t s_frame_seq;

/* Foto do quadro inteiro. */
static std::mutex s_photo_lock;
static std::condition_variable s_photo_cv;
static std::vector<uchar> s_photo;
static uint32_t s_photo_seq;

/* ---- Captura, recortes e foto --------------------------------------------- */

static void faces_json(std::string &s, const face_result_t &r);

/* Chamada pela captura (recortes, foto) e pela thread da imagem inteira: um
 * JPEG por vez no encoder de hardware. */
static bool encode_jpeg(const cv::Mat &img, std::vector<uchar> &out)
{
    static std::mutex s_encoder_lock;
    std::lock_guard<std::mutex> lock(s_encoder_lock);
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, s_quality.load()};
    if (!cv::imencode(".jpg", img, out, params)) {
        s_stats.encode_failed++;
        return false;
    }
    return true;
}

static void draw_faces(cv::Mat &frame)
{
    face_result_t res;
    face_get_result(&res, BOX_MAX_AGE_MS);
    const int pad = LABEL_SCALE * 2;
    const int th = overlay_text_height(LABEL_SCALE);

    for (size_t i = 0; i < res.n; i++) {
        const face_box_t *b = &res.box[i];
        const cv::Scalar &color = b->id >= 0 ? COLOR_KNOWN : COLOR_UNKNOWN;
        char label[24];
        if (b->id >= 0) {
            snprintf(label, sizeof(label), "ID %d %u%%", b->id, b->sim_pct);
        } else {
            snprintf(label, sizeof(label), "DESCONHECIDO");
        }

        overlay_rect(frame, b->x0, b->y0, b->x1, b->y1, BOX_THICK, color);
        /* Etiqueta em cima da caixa; sem espaço acima, dentro dela. */
        int lh = th + 2 * pad;
        int ly = b->y0 - lh >= 0 ? b->y0 - lh : b->y0;
        overlay_fill(frame, b->x0, ly, b->x0 + overlay_text_width(label, LABEL_SCALE) + 2 * pad - 1, ly + lh - 1,
                     color);
        overlay_text(frame, b->x0 + pad, ly + pad, label, LABEL_SCALE, COLOR_TEXT);
    }
}

#define ALIGN_DOWN_16(v)    ((v) & ~15)
#define ALIGN_UP_16(v)      ALIGN_DOWN_16((v) + 15)

/* Região do quadro que vira o recorte de uma caixa. A saída tem w/step x
 * h/step pixels: múltiplos de 16 (blocos do JPEG) e no máximo CROP_MAX_PX de
 * lado. Arredondar cresce a região em volta do centro da caixa; na borda do
 * quadro ela desliza para dentro. */
struct crop_t {
    int x, y, w, h, step;
};

static crop_t crop_region(const face_box_t *b)
{
    int bw = b->x1 - b->x0 + 1, bh = b->y1 - b->y0 + 1;
    int step = (std::max(bw, bh) + CROP_MAX_PX - 1) / CROP_MAX_PX;
    int ow = ALIGN_UP_16((bw + step - 1) / step), oh = ALIGN_UP_16((bh + step - 1) / step);
    ow = std::min(ow, ALIGN_DOWN_16(s_w / step));
    oh = std::min(oh, ALIGN_DOWN_16(s_h / step));
    crop_t c = {0, 0, ow * step, oh * step, step};
    /* Início par: o UV do NV12 tem metade da resolução. */
    c.x = std::clamp(b->x0 - (c.w - bw) / 2, 0, s_w - c.w) & ~1;
    c.y = std::clamp(b->y0 - (c.h - bh) / 2, 0, s_h - c.h) & ~1;
    return c;
}

/*
 * Recorta cada rosto deste quadro, se alguém assiste só os rostos e já deu o
 * intervalo de CROP_MAX_FPS. Só para quadros que a IA acabou de analisar: as
 * caixas publicadas são as deles, então o recorte cai exatamente no rosto.
 *
 * Cada evento leva a região recortada (x,y,w,h, em pixels do quadro) e o
 * centro da caixa naquele quadro (cx,cy): a página desenha o recorte na
 * caixa mais recente deslocando-o do quanto a caixa andou desde então.
 */
static void maybe_send_crops(const Frame &frame, int64_t *last_us)
{
    int64_t now = now_us();
    if (s_viewers.load() - s_full_viewers.load() <= 0 || now - *last_us < 1000000 / CROP_MAX_FPS) {
        return;
    }
    face_result_t res;
    face_get_result(&res, BOX_MAX_AGE_MS);
    if (res.n == 0) {
        return;
    }
    *last_us = now;

    std::string events;
    std::vector<uchar> jpg;
    for (size_t i = 0; i < res.n; i++) {
        const face_box_t *b = &res.box[i];
        crop_t c = crop_region(b);
        cv::Mat small = frame_to_bgr(frame, cv::Rect(c.x, c.y, c.w, c.h), cv::Size(c.w / c.step, c.h / c.step));
        if (small.empty() || !encode_jpeg(small, jpg)) {
            continue;
        }
        appendf(events, "event: crop\ndata: {\"trk\":%u,\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,\"cx\":%d,\"cy\":%d,\"jpg\":\"",
                b->track, c.x, c.y, c.w, c.h, (b->x0 + b->x1) / 2, (b->y0 + b->y1) / 2);
        events += base64(jpg.data(), jpg.size());
        events += "\"}\n\n";
    }
    s_stats.crop_us = (uint32_t)(now_us() - now);
    if (events.empty()) {
        return;
    }
    s_stats.crop_bytes = (uint32_t)events.size();
    {
        std::lock_guard<std::mutex> lock(s_crops_lock);
        s_crops_latest.swap(events);
        s_crops_seq++;
    }
    s_stats.crop_sets++;
}

/* Foto do quadro inteiro, com as caixas desenhadas, se alguém pediu. */
static void maybe_encode_photo(const Frame &frame)
{
    if (s_photo_waiters.load() <= 0) {
        return;
    }
    cv::Mat bgr = frame_to_bgr(frame, cv::Rect(0, 0, frame.w, frame.h));
    draw_faces(bgr);
    std::vector<uchar> jpg;
    if (!encode_jpeg(bgr, jpg)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_photo_lock);
        s_photo.swap(jpg);
        s_photo_seq++;
    }
    s_photo_cv.notify_all();
    s_stats.photos++;
}

/*
 * Passa o quadro para a thread da imagem inteira, se alguém pediu um quadro
 * novo e já deu o intervalo de FRAME_MAX_FPS. analyzed = a IA acabou de
 * analisá-lo: as caixas vão junto e precisam ser as dele (se ela já publicou
 * um quadro mais novo, este fica de fora). Sem IA, vai sem caixas.
 */
static void maybe_offer_full(const FramePtr &frame, bool analyzed, int64_t *last_us)
{
    int64_t now = now_us();
    if (!s_frame_want.load() || now - *last_us < 1000000 / FRAME_MAX_FPS) {
        return;
    }
    face_result_t res;
    face_get_result(&res, BOX_MAX_AGE_MS);
    if (!analyzed) {
        res.n = 0;
    } else if (res.frame_us != frame->t_us) {
        return;
    }
    *last_us = now;
    s_frame_want = false;
    {
        std::lock_guard<std::mutex> lock(s_full_lock);
        s_full_frame = frame;
        s_full_res = res;
    }
    s_full_cv.notify_one();
}

/*
 * Thread da imagem inteira: RGA para BGR e JPEG, e publica o quadro com as
 * caixas dele para o /frame.jpg. Fora da captura, que nunca espera por ela;
 * e com prioridade menor, para não tirar CPU da IA e da captura (o RV1106
 * tem um núcleo só). O buffer da câmera volta logo depois do RGA.
 */
static void full_frame_loop()
{
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
    std::vector<uchar> jpg;
    std::string faces;

    while (!s_stop) {
        FramePtr f;
        face_result_t res;
        {
            std::unique_lock<std::mutex> lock(s_full_lock);
            s_full_cv.wait_for(lock, std::chrono::milliseconds(500), [] { return s_full_frame != nullptr; });
            f = std::move(s_full_frame);
            res = s_full_res;
        }
        if (!f) {
            continue;
        }
        int64_t t0 = now_us();
        int64_t t_us = f->t_us;
        cv::Mat bgr = frame_to_bgr_full(*f);
        f.reset();      /* a imagem já está num buffer nosso */
        if (bgr.empty() || !encode_jpeg(bgr, jpg)) {
            s_frame_want = true;    /* quem pediu continua esperando: tenta o próximo */
            continue;
        }
        faces.clear();
        faces_json(faces, res);
        int64_t done = now_us();
        s_stats.frame_us = (uint32_t)(done - t0);
        s_stats.frame_bytes = (uint32_t)jpg.size();
        {
            std::lock_guard<std::mutex> lock(s_frame_lock);
            s_frame_jpg.swap(jpg);
            s_frame_faces.swap(faces);
            s_frame_t_us = t_us;
            s_frame_pub_us = done;
            s_frame_seq++;
        }
        s_frame_cv.notify_all();
        s_stats.frame_sets++;
    }
}

static void face_release(const FramePtr &frame)
{
    std::lock_guard<std::mutex> lock(s_rel_lock);
    s_released.push_back(frame);
}

/*
 * Prioridade é a IA: cada quadro novo vai para ela, sem cópia, e ela analisa
 * sempre o mais novo. O quadro analisado volta (por s_released) quando ela
 * termina - e aí, se for a vez da web, as caixas dele são recortadas, ele vai
 * para a imagem inteira e a foto sai dele. Sem IA (falhou ao iniciar), a
 * imagem inteira e a foto saem direto da captura.
 */
static void capture_loop()
{
    int64_t last_crop_us = 0, last_full_us = 0;
    int misses = 0;

    while (!s_stop) {
        while (true) {
            FramePtr done;
            {
                std::lock_guard<std::mutex> lock(s_rel_lock);
                if (s_released.empty()) {
                    break;
                }
                done = std::move(s_released.front());
                s_released.pop_front();
            }
            maybe_send_crops(*done, &last_crop_us);
            maybe_offer_full(done, true, &last_full_us);
            maybe_encode_photo(*done);
        }

        FramePtr f = camera_get(1000);
        if (!f) {
            if (++misses == 3) {
                LOGE(TAG, "a câmera parou de entregar quadros");
            }
            continue;
        }
        misses = 0;
        s_stats.captured++;

        if (!s_ai_paused && face_offer_frame(f)) {
            continue;   /* está com a IA; volta por s_released */
        }
        maybe_offer_full(f, false, &last_full_us);
        maybe_encode_photo(*f);
    }
}

/* ---- HTTP ------------------------------------------------------------------ */

static void send_json(int fd, const char *status, const char *body)
{
    http_respond(fd, status ? status : "200 OK", "application/json; charset=utf-8", body);
}

/* [{"trk","id","sim","x","y","dx","dy","age","kp","g","look","eyes","pu"}, ...]:
 * x,y,dx,dy como no terminal (face.h); age = ms desde que o rosto apareceu;
 * kp = os 5 pontos (x,y) do rosto; g = olhar yaw, pitch, cabeça yaw, pitch,
 * olhos yaw, pitch, em graus (gaze.h); look = olhando para a câmera; eyes =
 * achou a pupila; pu = pupilas xe,ye,xd,yd (-1 = não achou). */
static void faces_json(std::string &s, const face_result_t &r)
{
    s += "[";
    for (size_t i = 0; i < r.n; i++) {
        const face_box_t *b = &r.box[i];
        const gaze_t &g = b->gaze;
        appendf(s, "%s{\"trk\":%u,\"id\":%d,\"sim\":%u,\"x\":%d,\"y\":%d,\"dx\":%d,\"dy\":%d,\"age\":%lld,\"kp\":[",
                i ? "," : "", b->track, b->id, b->sim_pct, (b->x0 + b->x1) / 2, (b->y0 + b->y1) / 2, b->x1 - b->x0,
                b->y1 - b->y0, (long long)((r.frame_us - b->since_us) / 1000));
        for (int k = 0; k < 10; k++) {
            appendf(s, "%s%d", k ? "," : "", b->kp[k]);
        }
        appendf(s, "],\"g\":[%.1f,%.1f,%.1f,%.1f,%.1f,%.1f],\"look\":%d,\"eyes\":%d,\"pu\":[%d,%d,%d,%d]}", b->yaw,
                b->pitch, g.head_yaw, g.head_pitch, g.eye_yaw, g.eye_pitch, b->look, g.eyes, g.pupil[0], g.pupil[1],
                g.pupil[2], g.pupil[3]);
    }
    s += "]";
}

/* O JSON do /status (sem \n no fim: também vai numa linha "data:" do SSE). */
static void status_json(std::string &s)
{
    face_stats_t ai;
    face_get_stats(&ai);
    face_result_t res;
    face_get_result(&res, BOX_MAX_AGE_MS);

    appendf(s,
            "{\"width\":%d,\"height\":%d,\"quality\":%d,\"viewers\":%d,\"viewers_full\":%d,\"uptime_ms\":%lld,"
            "\"captured\":%u,\"crop_sets\":%u,\"crop_bytes\":%u,\"crop_us\":%u,"
            "\"frame_sets\":%u,\"frame_bytes\":%u,\"frame_us\":%u,\"frames_sent\":%u,"
            "\"photos\":%u,\"encode_failed\":%u,\"bytes_sent\":%llu,",
            s_w, s_h, s_quality.load(), s_viewers.load(), s_full_viewers.load(), (long long)(now_us() / 1000),
            s_stats.captured.load(), s_stats.crop_sets.load(), s_stats.crop_bytes.load(), s_stats.crop_us.load(),
            s_stats.frame_sets.load(), s_stats.frame_bytes.load(), s_stats.frame_us.load(), s_stats.frames_sent.load(),
            s_stats.photos.load(),
            s_stats.encode_failed.load(), (unsigned long long)s_stats.bytes_sent.load());
    appendf(s,
            "\"ai_cycles\":%u,\"ai_paused\":%s,\"detect_us\":%u,\"latency_us\":%u,\"recogs\":%u,\"recog_us\":%u,"
            "\"mem_free\":%llu,\"t\":%lld,\"faces\":",
            ai.cycles, s_ai_paused ? "true" : "false", ai.detect_us, ai.latency_us, ai.recogs, ai.recog_us,
            (unsigned long long)mem_available(), (long long)(res.frame_us / 1000));
    faces_json(s, res);

    float off[4];
    face_gaze_offsets(off);
    appendf(s, ",\"gaze_cal\":[%.1f,%.1f,%.1f,%.1f]", off[0], off[1], off[2], off[3]);

    int ids[64];
    size_t total = face_list(ids, sizeof(ids) / sizeof(ids[0]));
    appendf(s, ",\"enrolled_total\":%u,\"enrolled\":[", (unsigned)total);
    for (size_t i = 0; i < total && i < sizeof(ids) / sizeof(ids[0]); i++) {
        appendf(s, "%s%d", i ? "," : "", ids[i]);
    }
    s += "]}";
}

static bool sse_send(int fd, const std::string &data)
{
    if (!http_send_all(fd, data.data(), data.size())) {
        return false;
    }
    s_stats.bytes_sent += data.size();
    return true;
}

/* Sem Content-Length e sem chunked: o corpo termina quando a conexão fecha. */
static const char SSE_HEADERS[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream\r\n"
    "Cache-Control: no-store\r\n"
    "Access-Control-Allow-Origin: *\r\n"
    "Connection: close\r\n"
    "\r\n"
    "retry: 1000\n\n";

/*
 * Um espectador do /events (cada conexão já tem sua thread). Olha a cada
 * EVENTS_POLL_MS se há:
 *   - resultado novo da IA com rosto (ou o primeiro sem rosto depois de ter
 *     tido): evento "faces" - a caixa de cada rosto, a cada quadro analisado;
 *   - conjunto novo de recortes: os eventos "crop" já prontos (não com
 *     ?view=full: aí a imagem vem do /frame.jpg);
 *   - 1 s desde o último "stats".
 * Se o link for mais lento que a IA, só o mais recente de cada um sai.
 */
static void events_handler(HttpRequest &req)
{
    if (!s_camera_ok) {
        http_respond(req.fd, "503 Service Unavailable", "text/plain", "a camera nao iniciou - veja o log\n");
        return;
    }
    if (s_viewers.fetch_add(1) >= MAX_VIEWERS) {
        s_viewers.fetch_sub(1);
        http_respond(req.fd, "503 Service Unavailable", "text/plain", "limite de espectadores atingido\n");
        return;
    }
    std::string view;
    const bool full = http_query_value(req.query, "view", view) && view == "full";
    if (full) {
        s_full_viewers++;
    }
    /* Sem isto, o Nagle segura cada evento até o ACK do anterior, que o
     * navegador atrasa (delayed ACK): as caixas chegariam aos trancos. */
    int one = 1;
    setsockopt(req.fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    LOGI(TAG, "espectador entrou (%d assistindo; %s)", s_viewers.load(), full ? "imagem inteira" : "só os rostos");

    int64_t t_start = now_us();
    uint32_t events = 0, face_seq = 0, crop_seq = s_crops_seq.load();
    bool had_faces = true;      /* o primeiro "faces" sai mesmo sem rosto */
    int64_t next_stats = 0;
    std::string text, crops;
    bool ok = http_send_all(req.fd, SSE_HEADERS, sizeof(SSE_HEADERS) - 1);

    while (ok && !s_stop) {
        face_result_t res;
        face_get_result(&res, BOX_MAX_AGE_MS);
        if (res.n > 0 ? res.seq != face_seq : had_faces) {
            face_seq = res.seq;
            had_faces = res.n > 0;
            text.clear();
            appendf(text, "event: faces\ndata: {\"seq\":%u,\"t\":%lld,\"f\":", res.seq, (long long)(res.frame_us / 1000));
            faces_json(text, res);
            text += "}\n\n";
            ok = sse_send(req.fd, text);
            events++;
        }

        if (ok && !full && s_crops_seq.load() != crop_seq) {
            {
                std::lock_guard<std::mutex> lock(s_crops_lock);
                crops = s_crops_latest;
                crop_seq = s_crops_seq.load();
            }
            ok = sse_send(req.fd, crops);
        }

        int64_t now = now_us();
        if (ok && now >= next_stats) {
            next_stats = now + STATS_PERIOD_US;
            text = "event: stats\ndata: ";
            status_json(text);
            text += "\n\n";
            ok = sse_send(req.fd, text);
        }
        usleep(EVENTS_POLL_MS * 1000);
    }

    if (full) {
        s_full_viewers--;
    }
    s_viewers.fetch_sub(1);
    LOGI(TAG, "espectador saiu depois de %lld s (%u eventos de caixas)", (long long)((now_us() - t_start) / 1000000),
         events);
}

static void index_handler(HttpRequest &req)
{
    std::ifstream f(exe_dir() + "index.html", std::ios::binary);
    if (!f) {
        http_respond(req.fd, "500 Internal Server Error", "text/plain", "index.html nao encontrado\n");
        return;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    http_respond(req.fd, "200 OK", "text/html; charset=utf-8", ss.str());
}

static void capture_handler(HttpRequest &req)
{
    if (!s_camera_ok) {
        http_respond(req.fd, "503 Service Unavailable", "text/plain", "a camera nao iniciou - veja o log\n");
        return;
    }
    /* Espera uma foto tirada depois desta requisição: a anterior pode ser velha. */
    std::vector<uchar> jpg;
    s_photo_waiters++;
    {
        std::unique_lock<std::mutex> lock(s_photo_lock);
        uint32_t seq = s_photo_seq;
        if (s_photo_cv.wait_for(lock, std::chrono::milliseconds(FRAME_WAIT_MS), [seq] { return s_photo_seq != seq; })) {
            jpg = s_photo;
        }
    }
    s_photo_waiters--;
    if (jpg.empty()) {
        http_respond(req.fd, "500 Internal Server Error", "text/plain", "camera sem quadros\n");
        return;
    }
    http_respond(req.fd, "200 OK", "image/jpeg", std::string(jpg.begin(), jpg.end()));
}

/*
 * Imagem inteira, um quadro por pedido: o próximo depois de ?after=N (o
 * número do último que a página recebeu), em JPEG binário, com as caixas
 * dele no cabeçalho. Se o pronto já foi visto ou é velho, pede um novo à
 * captura e espera. Ao responder, já pede o seguinte: ele fica pronto
 * enquanto este viaja, e a página o recebe sem esperar a codificação.
 */
static void frame_handler(HttpRequest &req)
{
    if (!s_camera_ok) {
        http_respond(req.fd, "503 Service Unavailable", "text/plain", "a camera nao iniciou - veja o log\n");
        return;
    }
    std::string val;
    const uint32_t after = http_query_value(req.query, "after", val) ? (uint32_t)strtoul(val.c_str(), nullptr, 10) : 0;

    std::vector<uchar> jpg;
    std::string faces;
    uint32_t seq;
    int64_t t_us;
    {
        std::unique_lock<std::mutex> lock(s_frame_lock);
        auto ready = [after] { return s_frame_seq != after && now_us() - s_frame_pub_us < FRAME_FRESH_MS * 1000; };
        if (!ready()) {
            s_frame_want = true;
            if (!s_frame_cv.wait_for(lock, std::chrono::milliseconds(FRAME_WAIT_MS), ready)) {
                lock.unlock();
                http_respond(req.fd, "503 Service Unavailable", "text/plain", "camera sem quadros\n");
                return;
            }
        }
        jpg = s_frame_jpg;
        faces = s_frame_faces;
        seq = s_frame_seq;
        t_us = s_frame_t_us;
    }
    s_frame_want = true;

    std::string extra;
    appendf(extra,
            "X-Frame-Seq: %u\r\nX-Frame-T: %lld\r\nX-Faces: %s\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Expose-Headers: X-Frame-Seq, X-Frame-T, X-Faces\r\n",
            seq, (long long)(t_us / 1000), faces.c_str());
    http_respond(req.fd, "200 OK", "image/jpeg", jpg.data(), jpg.size(), extra.c_str());
    s_stats.frames_sent++;
}

static void status_handler(HttpRequest &req)
{
    std::string body;
    status_json(body);
    body += "\n";
    http_respond(req.fd, "200 OK", "application/json", body);
}

static void quality_handler(HttpRequest &req)
{
    std::string val;
    if (!http_query_value(req.query, "q", val)) {
        http_respond(req.fd, "400 Bad Request", "text/plain", "use /quality?q=1..100\n");
        return;
    }
    int q = atoi(val.c_str());
    if (q < 1 || q > 100) {
        http_respond(req.fd, "400 Bad Request", "text/plain", "q fora de 1..100\n");
        return;
    }
    s_quality = q;
    LOGI(TAG, "qualidade JPEG = %d", q);
    http_respond(req.fd, "200 OK", "text/plain", "ok");
}

static void enroll_handler(HttpRequest &req)
{
    face_enroll_result_t r = face_enroll(ENROLL_TIMEOUT_MS);
    char body[128];
    switch (r.status) {
    case FACE_ENROLL_NEW:
    case FACE_ENROLL_EXISTING:
        snprintf(body, sizeof(body), "{\"ok\":true,\"new\":%s,\"id\":%d,\"sim\":%u}",
                 r.status == FACE_ENROLL_NEW ? "true" : "false", r.id, r.sim_pct);
        return send_json(req.fd, nullptr, body);
    case FACE_ENROLL_NO_FACE:
        return send_json(req.fd, "409 Conflict", "{\"ok\":false,\"error\":\"nenhum rosto na imagem\"}");
    case FACE_ENROLL_TOO_SMALL:
        return send_json(req.fd, "409 Conflict", "{\"ok\":false,\"error\":\"rosto longe demais - chegue mais perto\"}");
    case FACE_ENROLL_BUSY:
        return send_json(req.fd, "409 Conflict", "{\"ok\":false,\"error\":\"outro cadastro em andamento\"}");
    default:
        return send_json(req.fd, "500 Internal Server Error", "{\"ok\":false,\"error\":\"falha ao gravar o cadastro\"}");
    }
}

static void delete_handler(HttpRequest &req)
{
    std::string val;
    if (!http_query_value(req.query, "id", val)) {
        return send_json(req.fd, "400 Bad Request", "{\"ok\":false,\"error\":\"use /delete?id=N ou id=all\"}");
    }
    int id = -1;
    if (val != "all") {
        id = atoi(val.c_str());
        if (id <= 0) {
            return send_json(req.fd, "400 Bad Request", "{\"ok\":false,\"error\":\"ID inválido\"}");
        }
    }
    int err = face_delete(id);
    if (err == -1) {
        return send_json(req.fd, "404 Not Found", "{\"ok\":false,\"error\":\"ID não cadastrado\"}");
    }
    if (err != 0) {
        return send_json(req.fd, "500 Internal Server Error", "{\"ok\":false,\"error\":\"falha ao gravar\"}");
    }
    send_json(req.fd, nullptr, "{\"ok\":true}");
}

static void gaze_calib_handler(HttpRequest &req)
{
    std::string val;
    const bool reset = http_query_value(req.query, "reset", val) && val == "1";
    if (!face_gaze_calibrate(reset)) {
        return send_json(req.fd, "409 Conflict", "{\"ok\":false,\"error\":\"nenhum rosto na imagem\"}");
    }
    send_json(req.fd, nullptr, "{\"ok\":true}");
}

static bool start_server(int port)
{
    static HttpServer server;
    server.on("GET", "/", index_handler);
    server.on("GET", "/events", events_handler);
    server.on("GET", "/capture.jpg", capture_handler);
    server.on("GET", "/frame.jpg", frame_handler);
    server.on("GET", "/status", status_handler);
    server.on("GET", "/quality", quality_handler);
    server.on("POST", "/enroll", enroll_handler);
    server.on("POST", "/delete", delete_handler);
    server.on("POST", "/gaze/calib", gaze_calib_handler);
    asr_routes(server);
    /* O navegador sempre pede; 204 evita um 404 a cada visita. */
    server.on("GET", "/favicon.ico", [](HttpRequest &req) {
        static const char NO_CONTENT[] = "HTTP/1.1 204 No Content\r\nConnection: close\r\n\r\n";
        http_send_all(req.fd, NO_CONTENT, sizeof(NO_CONTENT) - 1);
    });
    return server.start(port);
}

/* ---- Inicialização --------------------------------------------------------- */

/* A imagem da Luckfox sobe o rkipc (câmera + RTSP) no boot, e ele segura a
 * câmera. */
static void stop_rkipc()
{
    if (system("pidof rkipc >/dev/null 2>&1") == 0) {
        LOGI(TAG, "parando o rkipc (app de câmera da imagem) para liberar a câmera");
        const char *cmd = access("/oem/usr/bin/RkLunch-stop.sh", X_OK) == 0 ? "/oem/usr/bin/RkLunch-stop.sh >/dev/null 2>&1"
                                                                             : "RkLunch-stop.sh >/dev/null 2>&1";
        if (system(cmd) != 0) {
            LOGW(TAG, "RkLunch-stop.sh falhou");
        }
    }
}

static bool camera_start(const options_t &opt)
{
    if (!camera_open(opt.width, opt.height, opt.power_hz, opt.fps)) {
        return false;
    }
    FramePtr first = camera_get(3000);
    if (!first) {
        LOGE(TAG, "a câmera abriu mas não entregou quadro");
        return false;
    }
    s_w = first->w;
    s_h = first->h;
    LOGI(TAG, "câmera %dx%d NV12 (linha de %d bytes)", s_w, s_h, first->stride);
    return true;
}

static void print_urls(int port)
{
    std::string suffix = port == 80 ? "" : ":" + std::to_string(port);
    bool any = false;
    ifaddrs *ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs *i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || strcmp(i->ifa_name, "lo") == 0) {
                continue;
            }
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &((sockaddr_in *)i->ifa_addr)->sin_addr, ip, sizeof(ip));
            LOGI(TAG, "==== abra no navegador: http://%s%s/ ==== (%s)", ip, suffix.c_str(), i->ifa_name);
            any = true;
        }
        freeifaddrs(ifs);
    }
    LOGI(TAG, "%spela USB (adb): adb forward tcp:8080 tcp:%d e abra http://127.0.0.1:8080/",
         any ? "ou " : "sem rede; ", port);
}

static void usage(const char *argv0)
{
    printf("uso: %s [--port N] [--width N --height N] [--fps N] [--hz 50|60] [--thr 0..1] [--db arquivo] [--models pasta]\n"
           "       [--asr pasta]\n"
           "  --port    porta HTTP (80)\n"
           "  --width/--height  quadro da câmera (1280x960)\n"
           "  --fps     quadros/s fixos da câmera; 0 = automático (30)\n"
           "  --hz      frequência da rede elétrica, para o antiflicker (60)\n"
           "  --thr     semelhança mínima para reconhecer (0.60)\n"
           "  --db      arquivo dos cadastros (/userdata/faces.bin)\n"
           "  --models  pasta com RetinaFace.rknn e mobilefacenet.rknn (<pasta do app>/model)\n"
           "  --asr     pasta do asr_bench, para a página /asr (/root/asr_bench)\n",
           argv0);
}

static bool parse_args(int argc, char **argv, options_t &opt)
{
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        const char *v = i + 1 < argc ? argv[i + 1] : nullptr;
        if (a == "--help" || a == "-h") {
            return false;
        }
        if (!v) {
            fprintf(stderr, "%s precisa de um valor\n", a.c_str());
            return false;
        }
        if (a == "--port") {
            opt.port = atoi(v);
        } else if (a == "--width") {
            opt.width = atoi(v);
        } else if (a == "--height") {
            opt.height = atoi(v);
        } else if (a == "--fps") {
            opt.fps = atoi(v);
        } else if (a == "--hz") {
            opt.power_hz = atoi(v);
        } else if (a == "--thr") {
            opt.thr = (float)atof(v);
        } else if (a == "--db") {
            opt.db = v;
        } else if (a == "--models") {
            opt.models = v;
        } else if (a == "--asr") {
            opt.asr = v;
        } else {
            fprintf(stderr, "opção desconhecida: %s\n", a.c_str());
            return false;
        }
        i++;
    }
    if (!opt.models.empty() && opt.models.back() != '/') {
        opt.models += '/';
    }
    return true;
}

int main(int argc, char **argv)
{
    options_t opt;
    if (!parse_args(argc, argv, opt)) {
        usage(argv[0]);
        return 2;
    }
    if (opt.models.empty()) {
        opt.models = exe_dir() + "model/";
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);
    /* O rockit (VI) loga em nível 4 por padrão e enche o terminal do tracking;
     * 2 = só avisos e erros. Quem quiser mais exporta rt_log_level antes. */
    setenv("rt_log_level", "2", 0);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, [](int) { s_stop = true; });
    signal(SIGTERM, [](int) { s_stop = true; });

    LOGI(TAG, "MIS5001 + reconhecimento facial -> caixas e recortes pela rede");

    /* Sem câmera, segue mesmo assim: a página e o /status sobem (o vídeo
     * responde 503). */
    stop_rkipc();
    s_camera_ok = camera_start(opt);
    if (!s_camera_ok) {
        LOGE(TAG, "==== câmera falhou - seguindo sem vídeo ====");
    } else if (!face_init(s_w, s_h, opt.models, opt.db, opt.thr, face_release)) {
        /* Sem IA a página continua, só sem caixas. */
        LOGE(TAG, "==== reconhecimento facial falhou - seguindo sem IA ====");
    }

    asr_init(opt.asr, [](bool pause) {
        s_ai_paused = pause;
        LOGI(TAG, "IA de rostos %s", pause ? "parada durante a medição de fala" : "de volta");
    });
    if (!start_server(opt.port)) {
        LOGE(TAG, "==== servidor HTTP falhou ====");
        return 1;
    }
    print_urls(opt.port);

    if (s_camera_ok) {
        std::thread(full_frame_loop).detach();
        capture_loop();
    } else {
        while (!s_stop) {
            usleep(100 * 1000);
        }
    }

    LOGI(TAG, "encerrando");
    camera_close();
    /* As outras threads (IA, HTTP) não param sozinhas: sai sem destrutores. */
    fflush(stdout);
    _exit(0);
}
