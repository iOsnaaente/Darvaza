/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * IMX708 (Pi Camera Module 3) + reconhecimento facial, ao vivo no navegador.
 * Placa: Waveshare ESP32-P4-Module-DEV-KIT. O P4 não tem rádio: o Wi-Fi é o
 * ESP32-C6 da placa, falando com o P4 por SDIO (esp_hosted + esp_wifi_remote).
 *
 *   GET  /              página: caixas ao vivo, recorte de cada rosto, dados
 *   GET  /events        Server-Sent Events (text/event-stream):
 *                         faces - caixas e IDs, a cada quadro analisado
 *                         crop  - JPEG (base64) só da caixa de cada rosto
 *                         stats - o mesmo JSON do /status, 1 por segundo
 *                         level - nível do microfone (dBFS), 20 por segundo
 *   GET  /events?view=full  o mesmo, sem "crop" (a imagem vem do /frame.jpg)
 *   GET  /frame.jpg?after=N  o próximo quadro inteiro depois do N, em JPEG;
 *                         as caixas dele vão no cabeçalho X-Faces
 *   GET  /capture.jpg   uma foto do quadro inteiro, com as caixas (sob demanda)
 *   GET  /status        contadores, rostos na imagem, IDs cadastrados e o estado
 *                       da exposição pelo rosto (JSON)
 *   GET  /quality?q=N   qualidade JPEG ao vivo (1..100)
 *   POST /enroll        cadastra o maior rosto da imagem; devolve o ID
 *   POST /delete?id=N   apaga um cadastro (id=all apaga todos)
 *   GET  /isp           ajuste do ISP: valores, faixas e dados do JSON (isp_tune.h)
 *   POST /isp?k=v&...   muda e aplica na hora; /isp?save grava na NVS,
 *                       /isp?reset volta ao tuning do firmware
 *   POST /audio?gain=N  ganho do microfone, 0..42 dB (passos de 6)
 *   GET  /asr ...       página de fala: wake word e comandos do ESP-SR (asr.h)
 *
 * Sensor: IMX708 (1280x720) ou OV5647 (1280x960) - o esp_video usa o que
 * responder no I2C. Pipeline: CSI -> ISP (RGB565) -> IA (no próprio buffer da
 * câmera) -> recorte de cada caixa -> encoder JPEG de hardware -> "últimos
 * recortes" publicados. Por padrão o quadro inteiro não vai para a rede: a
 * página recebe as caixas (poucos bytes, a cada quadro) e desenha ela mesma,
 * e recebe a imagem só de dentro das caixas. Quem quer a imagem inteira pede
 * um quadro por vez ao /frame.jpg, e só pede o próximo quando o anterior
 * chegou: nunca há mais de um quadro no Wi-Fi, então a imagem não acumula
 * atraso nos buffers do caminho, e cada quadro só é codificado quando pedido.
 * Cada espectador do /events envia no ritmo do seu link; se ele for mais
 * lento que a IA, resultados intermediários são pulados.
 *
 * A IA (face.cpp) roda no core 1, no ritmo dela; a captura nunca espera por
 * ela e usa sempre o resultado mais recente.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "linux/videodev2.h"
#include "esp_video_init.h"
#include "esp_video_device.h"
#include "driver/jpeg_encode.h"
#include "driver/i2c_master.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "face.h"
#include "overlay.h"
#include "wifi.h"
#include "isp_tune.h"
#include "audio.h"
#include "asr.h"

/* ---- Pinos da câmera ----------------------------------------------------- *
 * SCCB da câmera no I2C interno: porta 0, SCL=GPIO8, SDA=GPIO7. O conector
 * CSI de 15 pinos não roteia reset/pwdn; o sensor roda livre.               */
#define CAM_SCCB_I2C_PORT   0
#define CAM_SCCB_SCL_PIN    8
#define CAM_SCCB_SDA_PIN    7
#define CAM_SCCB_FREQ_HZ    100000
#define CAM_RESET_PIN       (-1)
#define CAM_PWDN_PIN        (-1)
/* -------------------------------------------------------------------------- */

#define CAM_DEV_PATH        ESP_VIDEO_MIPI_CSI_DEVICE_NAME  /* /dev/video0 */
/* 5: a detecção segura um buffer (~20 ms, sem cópia), o reconhecimento pode
 * segurar outro (~120 ms), a captura um terceiro pelos recortes (~2 ms por
 * rosto), e o sensor precisa de pelo menos mais um livre para não perder
 * quadro. */
#define BUFFER_COUNT        5

/* Vale para os recortes e para a foto. Medido no quadro inteiro (1280x960):
 * ~115 KB em q60, ~60 KB em q35. Um recorte de 256x256 tem 1/19 dessa área,
 * então fica em poucos KB mesmo em q60. */
#define JPEG_QUALITY_DEFAULT 60

/* Recortes por segundo para a web. As caixas vão a cada quadro analisado; a
 * imagem de dentro delas não precisa de tanto, e cada recorte custa banda. */
#define CROP_MAX_FPS        15
/* Lado maior do recorte enviado (múltiplo de 16). Rosto maior que isso vai
 * reduzido pulando pixels: a banda fica limitada mesmo com o rosto colado. */
#define CROP_MAX_PX         256

/* Imagem inteira: no máximo isto de quadros por segundo (cada JPEG ocupa a
 * task de captura enquanto o encoder trabalha). O ritmo real é o do Wi-Fi: a
 * página pede o próximo quando o anterior chega. */
#define FRAME_MAX_FPS       15
/* Quadro pronto mais velho que isto não serve para quem pede: espera um novo. */
#define FRAME_FRESH_MS      300
#define FRAME_FACES_CAP     640     /* caixas de um quadro em JSON (cabeçalho X-Faces) */
#define FRAME_QUEUE_LEN     4       /* pedidos do /frame.jpg esperando a vez */
#define FRAME_TASK_STACK    6144

/* Cada espectador custa uma task, um buffer em PSRAM e um socket; e todos
 * dividem a mesma banda. */
#define MAX_VIEWERS         2
#define EVENTS_TASK_STACK   6144
#define EVENTS_POLL_MS      5       /* de quanto em quanto o espectador olha se há novidade */
#define EVENT_TEXT_CAP      2048    /* evento faces ou stats */
#define STATS_PERIOD_US     (1000 * 1000)
/* Nível do microfone para a página: 20/s basta para o slider andar liso, e
 * cada evento tem uns 40 bytes. */
#define LEVEL_PERIOD_US     (50 * 1000)

#define ISP_JSON_CAP        3072    /* resposta do /isp */

/* Quanto a foto (/capture.jpg) e a imagem inteira (/frame.jpg) esperam um
 * quadro novo. */
#define FRAME_WAIT_US       (3 * 1000 * 1000)

/* ---- Caixas (as desenhadas na foto; a página desenha as dela) --------------- */
#define BOX_THICK           3
#define LABEL_SCALE         3       /* fonte 5x7 x3 = 21 px de altura */
#define BOX_MAX_AGE_MS      600     /* IA parada há mais que isso: sem caixas */
#define COLOR_KNOWN         OVERLAY_RGB565(0, 220, 0)
#define COLOR_UNKNOWN       OVERLAY_RGB565(255, 140, 0)
#define COLOR_TEXT          OVERLAY_RGB565(0, 0, 0)

/* Quanto o /enroll espera um rosto aparecer. */
#define ENROLL_TIMEOUT_MS   4000

static const char *TAG = "imx708_web";

static const esp_video_init_csi_config_t csi_config[] = {{
    .sccb_config = {
        .init_sccb = true,
        .i2c_config = { .port = CAM_SCCB_I2C_PORT, .scl_pin = CAM_SCCB_SCL_PIN, .sda_pin = CAM_SCCB_SDA_PIN },
        .freq = CAM_SCCB_FREQ_HZ,
    },
    .reset_pin = CAM_RESET_PIN,
    .pwdn_pin  = CAM_PWDN_PIN,
}};

/* Autofoco: o motor (DW9807, I2C 0x0c) é outro chip no mesmo barramento, e o
 * esp_video só procura por ele se houver esta entrada. Sem AF a lente fica
 * parada e a imagem sai borrada - e o reconhecimento piora junto. */
static const esp_video_init_cam_motor_config_t motor_config[] = {{
    .sccb_config = {
        .init_sccb = true,
        .i2c_config = { .port = CAM_SCCB_I2C_PORT, .scl_pin = CAM_SCCB_SCL_PIN, .sda_pin = CAM_SCCB_SDA_PIN },
        .freq = CAM_SCCB_FREQ_HZ,
    },
    .reset_pin  = -1,
    .pwdn_pin   = -1,
    .signal_pin = -1,
}};

static const esp_video_init_config_t cam_config = {
    .csi = csi_config,
    .cam_motor = motor_config,
};

/* ---- Estado ---------------------------------------------------------------- */

static bool s_camera_ok;
static int s_fd = -1;
static uint8_t *s_buf[BUFFER_COUNT];
static uint32_t s_w, s_h;
static size_t s_frame_len;

static jpeg_encoder_handle_t s_jpeg;

/*
 * Foto do quadro inteiro, dois buffers JPEG: o encoder escreve em s_work e,
 * pronto, troca com s_latest sob o lock. Quem pediu copia s_latest sob o
 * mesmo lock e envia a cópia sem segurar nada - um envio lento nunca trava a
 * câmera. s_work é só da captura: o JPEG da imagem inteira também passa por
 * ele antes de ir para s_frame_latest.
 */
static uint8_t *s_work, *s_latest;
static size_t s_jpeg_cap;
static size_t s_latest_len;
static volatile uint32_t s_latest_seq;
static SemaphoreHandle_t s_lock;

/*
 * Recortes: cada caixa é copiada (contígua, como o encoder pede) para
 * s_crop_rgb, vira JPEG em s_crop_jpg e entra em s_crops_work como um evento
 * SSE pronto ("event: crop", JPEG em base64). O conjunto de todos os rostos
 * do quadro troca com s_crops_latest sob s_crops_lock, como a foto acima: o
 * base64 é feito uma vez, não uma vez por espectador.
 */
static uint16_t *s_crop_rgb;
static uint8_t *s_crop_jpg;
static size_t s_crop_jpg_cap;
static char *s_crops_work, *s_crops_latest;
static size_t s_crops_cap;
static size_t s_crops_len;
static volatile uint32_t s_crops_seq;
static SemaphoreHandle_t s_crops_lock;

/*
 * Imagem inteira. s_frame_want = alguém pediu um quadro novo: a captura
 * codifica o próximo quadro analisado e o copia, com as caixas dele, para
 * s_frame_latest sob s_frame_lock. A task do /frame.jpg copia de lá para
 * s_frame_send e envia sem segurar nada.
 */
static atomic_bool s_frame_want;
static uint8_t *s_frame_latest, *s_frame_send;
static size_t s_frame_cap;
static size_t s_frame_latest_len;
static char s_frame_faces[FRAME_FACES_CAP];
static int64_t s_frame_t_us;        /* captura do quadro */
static int64_t s_frame_pub_us;      /* quando ficou pronto */
static volatile uint32_t s_frame_seq;
static SemaphoreHandle_t s_frame_lock;
static TaskHandle_t s_frame_task;   /* acordada a cada quadro novo */
static QueueHandle_t s_frame_reqs;  /* pedidos do /frame.jpg (frame_req_t) */

static atomic_int s_quality = JPEG_QUALITY_DEFAULT;
/* Só faz JPEG quando alguém quer: recortes com espectadores no /events, a
 * foto quando pedida. A câmera e a IA rodam sempre: AE/AWB/AF ficam
 * convergidos e o terminal continua mostrando os rostos sem ninguém na
 * página. */
static atomic_int s_photo_waiters;
static atomic_int s_viewers;
static atomic_int s_full_viewers;   /* dos s_viewers, os da imagem inteira */
static atomic_bool s_ai_ok;         /* face_init deu certo */

static struct {
    uint32_t captured;
    uint32_t crop_sets;       /* conjuntos de recortes publicados */
    uint32_t crop_bytes;      /* tamanho do último conjunto (eventos com base64) */
    uint32_t crop_us;         /* último conjunto: cópia + JPEG + base64 */
    uint32_t frame_sets;      /* quadros inteiros codificados */
    uint32_t frame_bytes;     /* JPEG do último */
    uint32_t frame_us;        /* último: JPEG + cópia */
    uint32_t frames_sent;     /* respostas do /frame.jpg */
    uint32_t photos;
    uint32_t encode_failed;
    uint64_t bytes_sent;      /* tudo que saiu pelo /events */
} s_stats;

/* ---- Captura, recortes e foto --------------------------------------------- */

static void draw_faces(uint8_t *frame)
{
    face_result_t res;
    face_get_result(&res, BOX_MAX_AGE_MS);
    overlay_fb_t fb = { .px = (uint16_t *)frame, .w = (int)s_w, .h = (int)s_h };
    const int pad = LABEL_SCALE * 2;
    const int th = overlay_text_height(LABEL_SCALE);

    for (size_t i = 0; i < res.n; i++) {
        const face_box_t *b = &res.box[i];
        uint16_t color = b->id >= 0 ? COLOR_KNOWN : COLOR_UNKNOWN;
        char label[24];
        if (b->id >= 0) {
            snprintf(label, sizeof(label), "ID %d %u%%", b->id, b->sim_pct);
        } else {
            snprintf(label, sizeof(label), "DESCONHECIDO");
        }

        overlay_rect(&fb, b->x0, b->y0, b->x1, b->y1, BOX_THICK, color);
        /* Etiqueta em cima da caixa; sem espaço acima, dentro dela. */
        int lh = th + 2 * pad;
        int ly = b->y0 - lh >= 0 ? b->y0 - lh : b->y0;
        overlay_fill(&fb, b->x0, ly, b->x0 + overlay_text_width(label, LABEL_SCALE) + 2 * pad - 1,
                     ly + lh - 1, color);
        overlay_text(&fb, b->x0 + pad, ly + pad, label, LABEL_SCALE, COLOR_TEXT);
    }
}

/* Buffers que a IA terminou de usar, para a captura devolver à câmera. */
static QueueHandle_t s_released;

#define RELEASED_STALE 0x100   /* marca na fila: quadro velho, não vai para a web */

static void face_release(int buf_index, bool fresh)
{
    int v = buf_index | (fresh ? 0 : RELEASED_STALE);
    xQueueSend(s_released, &v, portMAX_DELAY);
}

static void requeue(int buf_index)
{
    struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE, .memory = V4L2_MEMORY_MMAP, .index = buf_index };
    ioctl(s_fd, VIDIOC_QBUF, &b);
}

/* snprintf que acumula num buffer de cap bytes e nunca passa do fim; used
 * >= cap depois = não coube. */
#define APPEND(buf, cap, used, ...)                                                   \
    do {                                                                              \
        if ((used) < (cap)) {                                                         \
            int _n = snprintf((buf) + (used), (cap) - (used), __VA_ARGS__);           \
            (used) += _n > 0 ? (size_t)_n : 0;                                        \
        }                                                                             \
    } while (0)

#define ALIGN_DOWN_16(v)    ((v) & ~15)
#define ALIGN_UP_16(v)      ALIGN_DOWN_16((v) + 15)

/* Região do quadro que vira o recorte de uma caixa. A saída tem w/step x
 * h/step pixels: múltiplos de 16 (o encoder lê blocos de 16x8 em YUV422) e no
 * máximo CROP_MAX_PX de lado. Arredondar cresce a região em volta do centro
 * da caixa; na borda do quadro ela desliza para dentro. */
typedef struct {
    int x, y, w, h, step;
} crop_t;

static int clampi(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static crop_t crop_region(const face_box_t *b)
{
    int bw = b->x1 - b->x0 + 1, bh = b->y1 - b->y0 + 1;
    int step = ((bw > bh ? bw : bh) + CROP_MAX_PX - 1) / CROP_MAX_PX;
    int ow = ALIGN_UP_16((bw + step - 1) / step), oh = ALIGN_UP_16((bh + step - 1) / step);
    if (ow > ALIGN_DOWN_16((int)s_w / step)) {
        ow = ALIGN_DOWN_16((int)s_w / step);
    }
    if (oh > ALIGN_DOWN_16((int)s_h / step)) {
        oh = ALIGN_DOWN_16((int)s_h / step);
    }
    crop_t c = { .w = ow * step, .h = oh * step, .step = step };
    c.x = clampi(b->x0 - (c.w - bw) / 2, 0, (int)s_w - c.w);
    c.y = clampi(b->y0 - (c.h - bh) / 2, 0, (int)s_h - c.h);
    return c;
}

static void crop_copy(const uint8_t *frame, const crop_t *c, uint16_t *dst)
{
    const int ow = c->w / c->step, oh = c->h / c->step;
    for (int r = 0; r < oh; r++) {
        const uint16_t *row = (const uint16_t *)frame + (size_t)(c->y + r * c->step) * s_w + c->x;
        if (c->step == 1) {
            memcpy(dst, row, ow * sizeof(*dst));
        } else {
            for (int i = 0; i < ow; i++) {
                dst[i] = row[i * c->step];
            }
        }
        dst += ow;
    }
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
static void maybe_send_crops(const uint8_t *frame, int64_t *last_us)
{
    int64_t now = esp_timer_get_time();
    if (atomic_load(&s_viewers) - atomic_load(&s_full_viewers) <= 0 || now - *last_us < 1000000 / CROP_MAX_FPS) {
        return;
    }
    face_result_t res;
    face_get_result(&res, BOX_MAX_AGE_MS);
    if (res.n == 0) {
        return;
    }
    *last_us = now;

    jpeg_encode_cfg_t cfg = {
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
        .image_quality = atomic_load(&s_quality),
    };
    size_t used = 0;
    for (size_t i = 0; i < res.n; i++) {
        const face_box_t *b = &res.box[i];
        crop_t c = crop_region(b);
        cfg.width = c.w / c.step;
        cfg.height = c.h / c.step;
        crop_copy(frame, &c, s_crop_rgb);
        uint32_t len = 0;
        if (jpeg_encoder_process(s_jpeg, &cfg, (const uint8_t *)s_crop_rgb, cfg.width * cfg.height * 2,
                                 s_crop_jpg, s_crop_jpg_cap, &len) != ESP_OK) {
            s_stats.encode_failed++;
            continue;
        }

        size_t start = used, b64 = 0;
        APPEND(s_crops_work, s_crops_cap, used,
               "event: crop\ndata: {\"trk\":%" PRIu32 ",\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
               "\"cx\":%d,\"cy\":%d,\"jpg\":\"",
               b->track, c.x, c.y, c.w, c.h, (b->x0 + b->x1) / 2, (b->y0 + b->y1) / 2);
        if (used >= s_crops_cap ||
            mbedtls_base64_encode((unsigned char *)s_crops_work + used, s_crops_cap - used, &b64, s_crop_jpg, len) != 0) {
            used = start;
            break;
        }
        used += b64;
        APPEND(s_crops_work, s_crops_cap, used, "\"}\n\n");
        if (used >= s_crops_cap) {
            used = start;
            break;
        }
    }
    s_stats.crop_us = (uint32_t)(esp_timer_get_time() - now);
    if (used == 0) {
        return;
    }

    xSemaphoreTake(s_crops_lock, portMAX_DELAY);
    char *done = s_crops_work;
    s_crops_work = s_crops_latest;
    s_crops_latest = done;
    s_crops_len = used;
    s_crops_seq++;
    xSemaphoreGive(s_crops_lock);
    s_stats.crop_sets++;
    s_stats.crop_bytes = used;
}

static size_t faces_json(char *buf, size_t cap, size_t used, const face_result_t *r);

/*
 * Imagem inteira, se alguém pediu um quadro novo e já deu o intervalo de
 * FRAME_MAX_FPS: o quadro todo em JPEG (por s_work) e as caixas dele, para o
 * /frame.jpg. analyzed = a IA acabou de analisar este quadro e as caixas são
 * as dele; false = sem IA, vai sem caixas. Antes da foto: desenhar as caixas
 * altera o quadro.
 */
static void maybe_encode_frame(const uint8_t *frame, bool analyzed, jpeg_encode_cfg_t *cfg, int64_t *last_us)
{
    int64_t now = esp_timer_get_time();
    if (!atomic_load(&s_frame_want) || now - *last_us < 1000000 / FRAME_MAX_FPS) {
        return;
    }
    *last_us = now;
    face_result_t res;
    face_get_result(&res, BOX_MAX_AGE_MS);
    int64_t t_us = res.frame_us;
    if (!analyzed) {
        res.n = 0;
        t_us = now;
    }
    char faces[FRAME_FACES_CAP];
    if (faces_json(faces, sizeof(faces), 0, &res) >= sizeof(faces)) {
        strcpy(faces, "[]");
    }

    cfg->image_quality = atomic_load(&s_quality);
    uint32_t len = 0;
    if (jpeg_encoder_process(s_jpeg, cfg, frame, s_frame_len, s_work, s_jpeg_cap, &len) != ESP_OK ||
        len > s_frame_cap) {
        s_stats.encode_failed++;    /* quem pediu continua esperando: tenta o próximo */
        return;
    }
    atomic_store(&s_frame_want, false);

    xSemaphoreTake(s_frame_lock, portMAX_DELAY);
    memcpy(s_frame_latest, s_work, len);
    s_frame_latest_len = len;
    strcpy(s_frame_faces, faces);
    s_frame_t_us = t_us;
    s_frame_pub_us = esp_timer_get_time();
    s_frame_seq++;
    xSemaphoreGive(s_frame_lock);
    if (s_frame_task) {
        xTaskNotifyGive(s_frame_task);
    }
    s_stats.frame_us = (uint32_t)(esp_timer_get_time() - now);
    s_stats.frame_sets++;
    s_stats.frame_bytes = len;
}

/* Foto do quadro inteiro, com as caixas desenhadas, se alguém pediu. Depois
 * dos recortes e da imagem inteira: desenhar as caixas altera o quadro. */
static void maybe_encode_photo(uint8_t *frame, jpeg_encode_cfg_t *cfg)
{
    if (atomic_load(&s_photo_waiters) <= 0) {
        return;
    }
    draw_faces(frame);

    cfg->image_quality = atomic_load(&s_quality);
    uint32_t len = 0;
    if (jpeg_encoder_process(s_jpeg, cfg, frame, s_frame_len, s_work, s_jpeg_cap, &len) != ESP_OK) {
        s_stats.encode_failed++;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint8_t *done = s_work;
    s_work = s_latest;
    s_latest = done;
    s_latest_len = len;
    s_latest_seq++;
    xSemaphoreGive(s_lock);
    s_stats.photos++;
}

/*
 * Prioridade é a IA: cada quadro novo vai primeiro para ela, sem cópia. Se ela
 * aceita, o buffer só volta (por s_released) quando ela termina - e aí, se for
 * a vez da web, as caixas dele são recortadas e ele vira a imagem inteira
 * antes de voltar para a câmera. Quadros que a IA recusou (ocupada) só servem
 * para a foto; sem IA, também para a imagem inteira.
 */
static void capture_task(void *arg)
{
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    /* YUV422 funciona com qualquer altura múltipla de 8 (720, 960 e 1080). */
    jpeg_encode_cfg_t full_cfg = {
        .width = s_w,
        .height = s_h,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
    };
    int64_t last_crop_us = 0, last_frame_us = 0;

    while (1) {
        int v;
        while (xQueueReceive(s_released, &v, 0) == pdTRUE) {
            int idx = v & ~RELEASED_STALE;
            if (!(v & RELEASED_STALE)) {
                maybe_send_crops(s_buf[idx], &last_crop_us);
                maybe_encode_frame(s_buf[idx], true, &full_cfg, &last_frame_us);
                maybe_encode_photo(s_buf[idx], &full_cfg);
            }
            requeue(idx);
        }

        struct v4l2_buffer buf = { .type = type, .memory = V4L2_MEMORY_MMAP };
        if (ioctl(s_fd, VIDIOC_DQBUF, &buf) != 0) {
            ESP_LOGE(TAG, "DQBUF falhou - captura parada");
            break;
        }
        s_stats.captured++;

        if (face_offer_frame(s_buf[buf.index], buf.index)) {
            continue;   /* está com a IA; volta por s_released */
        }
        if (!atomic_load(&s_ai_ok) || face_paused()) {
            maybe_encode_frame(s_buf[buf.index], false, &full_cfg, &last_frame_us);
        }
        maybe_encode_photo(s_buf[buf.index], &full_cfg);
        ioctl(s_fd, VIDIOC_QBUF, &buf);
    }
    vTaskDelete(NULL);
}

/* Espera uma foto mais nova que *seq e copia para dst (capacidade s_jpeg_cap). */
static esp_err_t grab_photo(uint8_t *dst, size_t *len, uint32_t *seq)
{
    int64_t deadline = esp_timer_get_time() + FRAME_WAIT_US;
    while (s_latest_seq == *seq) {
        if (esp_timer_get_time() > deadline) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memcpy(dst, s_latest, s_latest_len);
    *len = s_latest_len;
    *seq = s_latest_seq;
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

/* ---- HTTP ------------------------------------------------------------------ */

/* main/index.html, embutido pelo CMakeLists (EMBED_TXTFILES, que põe um \0 no fim). */
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start - 1);
}

/* O navegador sempre pede; 204 evita um warning 404 no log a cada visita. */
static esp_err_t favicon_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    if (status) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

/* [{"trk","id","sim","x","y","dx","dy","age"}, ...]: x,y,dx,dy como no
 * terminal (face.h); age = ms desde que o rosto apareceu. */
static size_t faces_json(char *buf, size_t cap, size_t used, const face_result_t *r)
{
    APPEND(buf, cap, used, "[");
    for (size_t i = 0; i < r->n; i++) {
        const face_box_t *b = &r->box[i];
        APPEND(buf, cap, used,
               "%s{\"trk\":%" PRIu32 ",\"id\":%d,\"sim\":%u,\"x\":%d,\"y\":%d,\"dx\":%d,\"dy\":%d,\"age\":%" PRId64 "}",
               i ? "," : "", b->track, b->id, b->sim_pct, (b->x0 + b->x1) / 2, (b->y0 + b->y1) / 2,
               b->x1 - b->x0, b->y1 - b->y0, (r->frame_us - b->since_us) / 1000);
    }
    APPEND(buf, cap, used, "]");
    return used;
}

/* O JSON do /status (sem \n no fim: também vai numa linha "data:" do SSE). */
static size_t status_json(char *buf, size_t cap, size_t used)
{
    face_stats_t ai;
    face_get_stats(&ai);
    face_result_t res;
    face_get_result(&res, BOX_MAX_AGE_MS);

    APPEND(buf, cap, used,
           "{\"width\":%" PRIu32 ",\"height\":%" PRIu32 ",\"quality\":%d,\"viewers\":%d,\"viewers_full\":%d,"
           "\"uptime_ms\":%" PRId64 ","
           "\"captured\":%" PRIu32 ",\"crop_sets\":%" PRIu32 ",\"crop_bytes\":%" PRIu32 ",\"crop_us\":%" PRIu32 ","
           "\"frame_sets\":%" PRIu32 ",\"frame_bytes\":%" PRIu32 ",\"frame_us\":%" PRIu32 ","
           "\"frames_sent\":%" PRIu32 ","
           "\"photos\":%" PRIu32 ",\"encode_failed\":%" PRIu32 ",\"bytes_sent\":%" PRIu64 ","
           "\"ai_cycles\":%" PRIu32 ",\"detect_us\":%" PRIu32 ",\"latency_us\":%" PRIu32 ","
           "\"recogs\":%" PRIu32 ",\"recog_us\":%" PRIu32 ",\"mic_gain\":%d,"
           "\"heap_internal\":%u,\"heap_psram\":%u,\"t\":%" PRId64 ",\"faces\":",
           s_w, s_h, atomic_load(&s_quality), atomic_load(&s_viewers), atomic_load(&s_full_viewers),
           esp_timer_get_time() / 1000,
           s_stats.captured, s_stats.crop_sets, s_stats.crop_bytes, s_stats.crop_us,
           s_stats.frame_sets, s_stats.frame_bytes, s_stats.frame_us, s_stats.frames_sent,
           s_stats.photos, s_stats.encode_failed, s_stats.bytes_sent,
           ai.cycles, ai.detect_us, ai.latency_us, ai.recogs, ai.recog_us, audio_gain(),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM), res.frame_us / 1000);
    used = faces_json(buf, cap, used, &res);

    int ids[64];
    size_t total = face_list(ids, sizeof(ids) / sizeof(ids[0]));
    APPEND(buf, cap, used, ",\"enrolled_total\":%u,\"enrolled\":[", (unsigned)total);
    for (size_t i = 0; i < total && i < sizeof(ids) / sizeof(ids[0]); i++) {
        APPEND(buf, cap, used, "%s%d", i ? "," : "", ids[i]);
    }
    isp_ae_status_t ae;
    isp_tune_ae_status(&ae);
    APPEND(buf, cap, used, "],\"ae\":{\"mode\":\"%s\",\"now\":%d,\"luma\":%d,\"target\":%d}}",
           ae.mode, ae.sensor_ae, ae.face_luma, ae.target);
    return used;
}

/* Bytes crus no socket (sem chunked): cada evento sai num send() só - com
 * TCP_NODELAY, um segmento por evento em vez dos três do chunked. */
static bool send_all(httpd_req_t *req, const char *buf, size_t len)
{
    while (len > 0) {
        int n = httpd_send(req, buf, len);
        if (n <= 0) {
            return false;
        }
        buf += n;
        len -= n;
        s_stats.bytes_sent += n;
    }
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

typedef struct {
    httpd_req_t *req;
    bool full;      /* /events?view=full: quadro inteiro em vez dos recortes */
} viewer_t;

/*
 * Um espectador do /events. Olha a cada EVENTS_POLL_MS se há:
 *   - resultado novo da IA com rosto (ou o primeiro sem rosto depois de ter
 *     tido): evento "faces" - a caixa de cada rosto, a cada quadro analisado;
 *   - conjunto novo de recortes: os eventos "crop" já prontos (não com
 *     ?view=full: aí a imagem vem do /frame.jpg);
 *   - 1 s desde o último "stats";
 *   - 50 ms desde o último "level" (se o microfone iniciou).
 * Se o link for mais lento que a IA, só o mais recente de cada um sai.
 */
static void events_task(void *arg)
{
    viewer_t *viewer = arg;
    httpd_req_t *req = viewer->req;
    const bool full = viewer->full;
    free(viewer);
    if (full) {
        atomic_fetch_add(&s_full_viewers, 1);
    }
    char *text = heap_caps_malloc(EVENT_TEXT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *crops = full ? NULL : heap_caps_malloc(s_crops_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t events = 0;
    int64_t t_start = esp_timer_get_time();

    if (!text || (!full && !crops)) {
        ESP_LOGE(TAG, "sem PSRAM para o espectador");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria");
        goto done;
    }

    /* Sem isto, o Nagle segura cada evento até o ACK do anterior, que o
     * navegador atrasa (delayed ACK): as caixas chegariam aos trancos. */
    int one = 1;
    setsockopt(httpd_req_to_sockfd(req), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    ESP_LOGI(TAG, "espectador entrou (%d assistindo; %s)", atomic_load(&s_viewers),
             full ? "imagem inteira" : "só os rostos");

    uint32_t face_seq = 0, crop_seq = s_crops_seq;
    bool had_faces = true;      /* o primeiro "faces" sai mesmo sem rosto */
    int64_t next_stats = 0, next_level = 0;
    bool ok = send_all(req, SSE_HEADERS, sizeof(SSE_HEADERS) - 1);

    while (ok) {
        face_result_t res;
        face_get_result(&res, BOX_MAX_AGE_MS);
        if (res.n > 0 ? res.seq != face_seq : had_faces) {
            face_seq = res.seq;
            had_faces = res.n > 0;
            size_t used = 0;
            APPEND(text, EVENT_TEXT_CAP, used, "event: faces\ndata: {\"seq\":%" PRIu32 ",\"t\":%" PRId64 ",\"f\":",
                   res.seq, res.frame_us / 1000);
            used = faces_json(text, EVENT_TEXT_CAP, used, &res);
            APPEND(text, EVENT_TEXT_CAP, used, "}\n\n");
            if (used < EVENT_TEXT_CAP) {
                ok = send_all(req, text, used);
                events++;
            }
        }

        if (ok && !full && s_crops_seq != crop_seq) {
            xSemaphoreTake(s_crops_lock, portMAX_DELAY);
            size_t len = s_crops_len;
            memcpy(crops, s_crops_latest, len);
            crop_seq = s_crops_seq;
            xSemaphoreGive(s_crops_lock);
            ok = send_all(req, crops, len);
        }

        int64_t now = esp_timer_get_time();
        if (ok && now >= next_stats) {
            next_stats = now + STATS_PERIOD_US;
            size_t used = 0;
            APPEND(text, EVENT_TEXT_CAP, used, "event: stats\ndata: ");
            used = status_json(text, EVENT_TEXT_CAP, used);
            APPEND(text, EVENT_TEXT_CAP, used, "\n\n");
            if (used < EVENT_TEXT_CAP) {
                ok = send_all(req, text, used);
            }
        }

        if (ok && audio_ok() && now >= next_level) {
            next_level = now + LEVEL_PERIOD_US;
            audio_level_t lv;
            audio_get_level(&lv);
            size_t used = 0;
            APPEND(text, EVENT_TEXT_CAP, used, "event: level\ndata: {\"rms\":%.1f,\"peak\":%.1f,\"ref\":%.1f}\n\n",
                   lv.rms_db, lv.peak_db, lv.ref_db);
            ok = send_all(req, text, used);
        }
        vTaskDelay(pdMS_TO_TICKS(EVENTS_POLL_MS));
    }

    uint32_t ms = (uint32_t)((esp_timer_get_time() - t_start) / 1000);
    ESP_LOGI(TAG, "espectador saiu depois de %" PRIu32 " s (%" PRIu32 " eventos de caixas)", ms / 1000, events);

done:
    free(text);
    free(crops);
    httpd_req_async_handler_complete(req);
    if (full) {
        atomic_fetch_sub(&s_full_viewers, 1);
    }
    atomic_fetch_sub(&s_viewers, 1);
    vTaskDelete(NULL);
}

/*
 * O httpd atende uma requisição por vez numa única task, e o /events nunca
 * termina. Por isso cada espectador vai para uma task própria (handler
 * assíncrono) e o servidor continua livre para página, foto e status.
 */
static esp_err_t camera_down(httpd_req_t *req)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "a camera nao iniciou - veja o log serial");
}

static esp_err_t events_handler(httpd_req_t *req)
{
    if (!s_camera_ok) {
        return camera_down(req);
    }
    if (atomic_fetch_add(&s_viewers, 1) >= MAX_VIEWERS) {
        atomic_fetch_sub(&s_viewers, 1);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "limite de espectadores atingido");
    }

    viewer_t *viewer = malloc(sizeof(*viewer));
    if (!viewer) {
        atomic_fetch_sub(&s_viewers, 1);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria");
    }
    char query[32], val[8];
    viewer->full = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                   httpd_query_key_value(query, "view", val, sizeof(val)) == ESP_OK && strcmp(val, "full") == 0;

    if (httpd_req_async_handler_begin(req, &viewer->req) != ESP_OK) {
        free(viewer);
        atomic_fetch_sub(&s_viewers, 1);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async falhou");
    }
    if (xTaskCreate(events_task, "events", EVENTS_TASK_STACK, viewer, 5, NULL) != pdPASS) {
        httpd_req_async_handler_complete(viewer->req);
        free(viewer);
        atomic_fetch_sub(&s_viewers, 1);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria para a task");
    }
    return ESP_OK;
}

static esp_err_t capture_handler(httpd_req_t *req)
{
    if (!s_camera_ok) {
        return camera_down(req);
    }
    uint8_t *frame = heap_caps_malloc(s_jpeg_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria");
    }

    /* Espera uma foto tirada depois desta requisição: a anterior pode ser velha. */
    atomic_fetch_add(&s_photo_waiters, 1);
    uint32_t seq = s_latest_seq;
    size_t len = 0;
    esp_err_t err = grab_photo(frame, &len, &seq);
    atomic_fetch_sub(&s_photo_waiters, 1);

    if (err != ESP_OK) {
        free(frame);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "camera sem quadros");
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, (const char *)frame, len);
    free(frame);
    return err;
}

/*
 * Imagem inteira, um quadro por pedido: o próximo depois de ?after=N (o
 * número do último que a página recebeu), em JPEG, com as caixas dele no
 * cabeçalho. O envio leva ~100 ms no Wi-Fi: para o httpd não ficar preso,
 * cada pedido vira assíncrono e entra na fila de uma task só, que os atende
 * em ordem.
 */
typedef struct {
    httpd_req_t *req;
    uint32_t after;
} frame_req_t;

/* Espera em s_frame_latest um quadro novo (≠ after) e recente, pedindo um à
 * captura enquanto não há. Copia para s_frame_send; false = não veio. */
static bool frame_wait(uint32_t after, size_t *len, char *faces, uint32_t *seq, int64_t *t_us)
{
    int64_t deadline = esp_timer_get_time() + FRAME_WAIT_US;
    while (1) {
        xSemaphoreTake(s_frame_lock, portMAX_DELAY);
        bool ready = s_frame_seq != after && esp_timer_get_time() - s_frame_pub_us < FRAME_FRESH_MS * 1000LL;
        if (ready) {
            memcpy(s_frame_send, s_frame_latest, s_frame_latest_len);
            *len = s_frame_latest_len;
            strcpy(faces, s_frame_faces);
            *seq = s_frame_seq;
            *t_us = s_frame_t_us;
        }
        xSemaphoreGive(s_frame_lock);
        if (ready) {
            return true;
        }
        if (esp_timer_get_time() > deadline) {
            return false;
        }
        atomic_store(&s_frame_want, true);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));
    }
}

static void frame_task(void *arg)
{
    char faces[FRAME_FACES_CAP], seq_s[12], t_s[24];
    while (1) {
        frame_req_t fr;
        xQueueReceive(s_frame_reqs, &fr, portMAX_DELAY);
        size_t len = 0;
        uint32_t seq = 0;
        int64_t t_us = 0;
        if (!frame_wait(fr.after, &len, faces, &seq, &t_us)) {
            httpd_resp_set_status(fr.req, "503 Service Unavailable");
            httpd_resp_sendstr(fr.req, "camera sem quadros");
            httpd_req_async_handler_complete(fr.req);
            continue;
        }
        /* Já pede o seguinte: ele fica pronto enquanto este viaja, e a página
         * o recebe sem esperar a codificação. */
        atomic_store(&s_frame_want, true);

        snprintf(seq_s, sizeof(seq_s), "%" PRIu32, seq);
        snprintf(t_s, sizeof(t_s), "%" PRId64, t_us / 1000);
        httpd_resp_set_type(fr.req, "image/jpeg");
        httpd_resp_set_hdr(fr.req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(fr.req, "X-Frame-Seq", seq_s);
        httpd_resp_set_hdr(fr.req, "X-Frame-T", t_s);
        httpd_resp_set_hdr(fr.req, "X-Faces", faces);
        httpd_resp_set_hdr(fr.req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(fr.req, "Access-Control-Expose-Headers", "X-Frame-Seq, X-Frame-T, X-Faces");
        if (httpd_resp_send(fr.req, (const char *)s_frame_send, len) == ESP_OK) {
            s_stats.frames_sent++;
        }
        httpd_req_async_handler_complete(fr.req);
    }
}

static esp_err_t frame_handler(httpd_req_t *req)
{
    if (!s_camera_ok) {
        return camera_down(req);
    }
    /* Só o httpd põe na fila: se há vaga agora, há vaga depois do begin. */
    if (uxQueueSpacesAvailable(s_frame_reqs) == 0) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "pedidos demais");
    }
    frame_req_t fr = { 0 };
    char query[32], val[12];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "after", val, sizeof(val)) == ESP_OK) {
        fr.after = strtoul(val, NULL, 10);
    }
    if (httpd_req_async_handler_begin(req, &fr.req) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async falhou");
    }
    xQueueSend(s_frame_reqs, &fr, portMAX_DELAY);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req)
{
    char body[1536];
    size_t used = status_json(body, sizeof(body), 0);
    APPEND(body, sizeof(body), used, "\n");
    if (used >= sizeof(body)) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "status grande demais");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, used);
}

static esp_err_t quality_handler(httpd_req_t *req)
{
    char query[32], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "q", val, sizeof(val)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use /quality?q=1..100");
    }
    int q = atoi(val);
    if (q < 1 || q > 100) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "q fora de 1..100");
    }
    atomic_store(&s_quality, q);
    ESP_LOGI(TAG, "qualidade JPEG = %d", q);
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t enroll_handler(httpd_req_t *req)
{
    face_enroll_result_t r = face_enroll(ENROLL_TIMEOUT_MS);
    char body[128];
    switch (r.status) {
    case FACE_ENROLL_NEW:
    case FACE_ENROLL_EXISTING:
        snprintf(body, sizeof(body), "{\"ok\":true,\"new\":%s,\"id\":%d,\"sim\":%u}",
                 r.status == FACE_ENROLL_NEW ? "true" : "false", r.id, r.sim_pct);
        return send_json(req, NULL, body);
    case FACE_ENROLL_NO_FACE:
        return send_json(req, "409 Conflict", "{\"ok\":false,\"error\":\"nenhum rosto na imagem\"}");
    case FACE_ENROLL_TOO_SMALL:
        return send_json(req, "409 Conflict", "{\"ok\":false,\"error\":\"rosto longe demais - chegue mais perto\"}");
    case FACE_ENROLL_BUSY:
        return send_json(req, "409 Conflict", "{\"ok\":false,\"error\":\"outro cadastro em andamento\"}");
    default:
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"falha ao gravar o cadastro\"}");
    }
}

static esp_err_t delete_handler(httpd_req_t *req)
{
    char query[32], val[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", val, sizeof(val)) != ESP_OK) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"use /delete?id=N ou id=all\"}");
    }
    int id = -1;
    if (strcmp(val, "all") != 0) {
        id = atoi(val);
        if (id <= 0) {
            return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"ID inválido\"}");
        }
    }
    esp_err_t err = face_delete(id);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_json(req, "404 Not Found", "{\"ok\":false,\"error\":\"ID não cadastrado\"}");
    }
    if (err != ESP_OK) {
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"falha ao gravar\"}");
    }
    return send_json(req, NULL, "{\"ok\":true}");
}

/* Ajuste do ISP: o estado inteiro, também em resposta a cada mudança. */
static esp_err_t send_isp_state(httpd_req_t *req)
{
    char *body = malloc(ISP_JSON_CAP);
    if (!body) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria");
    }
    esp_err_t err = isp_tune_json(body, ISP_JSON_CAP) < ISP_JSON_CAP
                    ? send_json(req, NULL, body)
                    : httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "estado grande demais");
    free(body);
    return err;
}

static esp_err_t isp_get_handler(httpd_req_t *req)
{
    return send_isp_state(req);
}

static esp_err_t isp_post_handler(httpd_req_t *req)
{
    char query[256];
    esp_err_t err = httpd_req_get_url_query_str(req, query, sizeof(query));
    if (err == ESP_ERR_NOT_FOUND) {
        return send_isp_state(req);
    }
    if (err != ESP_OK) {
        err = ESP_ERR_INVALID_ARG;
    } else if (strcmp(query, "save") == 0) {
        err = isp_tune_save();
    } else if (strcmp(query, "reset") == 0) {
        err = isp_tune_reset();
    } else {
        err = isp_tune_set(query);
    }
    if (err == ESP_ERR_INVALID_ARG) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"parâmetro desconhecido ou fora da faixa\"}");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_json(req, "503 Service Unavailable", "{\"ok\":false,\"error\":\"ajuste do ISP indisponível\"}");
    }
    if (err != ESP_OK) {
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"o ISP ou a NVS recusou - veja o log\"}");
    }
    return send_isp_state(req);
}

static esp_err_t audio_handler(httpd_req_t *req)
{
    char query[32], val[8];
    if (!audio_ok()) {
        return send_json(req, "503 Service Unavailable", "{\"ok\":false,\"error\":\"o microfone não iniciou - veja o log\"}");
    }
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "gain", val, sizeof(val)) != ESP_OK) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"use /audio?gain=0..42\"}");
    }
    char *end = NULL;
    long db = strtol(val, &end, 10);
    if (end == val || *end || audio_set_gain((int)db) != ESP_OK) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"error\":\"ganho fora de 0..42\"}");
    }
    char body[48];
    snprintf(body, sizeof(body), "{\"ok\":true,\"gain\":%d}", audio_gain());
    return send_json(req, NULL, body);
}

static bool start_server(void)
{
    /* Os pedidos do /frame.jpg, atendidos em ordem por uma task só. */
    s_frame_reqs = xQueueCreate(FRAME_QUEUE_LEN, sizeof(frame_req_t));
    if (!s_frame_reqs || xTaskCreate(frame_task, "frame", FRAME_TASK_STACK, NULL, 5, &s_frame_task) != pdPASS) {
        ESP_LOGE(TAG, "sem memória para a task da imagem inteira");
        return false;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    /* Cada espectador segura o socket do /events e o do /frame.jpg (o
     * navegador o reaproveita a cada quadro); sobram 3 para página, foto e
     * status. */
    cfg.max_open_sockets = MAX_VIEWERS * 2 + 3;
    cfg.max_uri_handlers = 24;      /* 12 daqui + 10 da página de fala */
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start falhou");
        return false;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = index_handler },
        { .uri = "/events",      .method = HTTP_GET,  .handler = events_handler },
        { .uri = "/capture.jpg", .method = HTTP_GET,  .handler = capture_handler },
        { .uri = "/frame.jpg",   .method = HTTP_GET,  .handler = frame_handler },
        { .uri = "/status",      .method = HTTP_GET,  .handler = status_handler },
        { .uri = "/quality",     .method = HTTP_GET,  .handler = quality_handler },
        { .uri = "/enroll",      .method = HTTP_POST, .handler = enroll_handler },
        { .uri = "/delete",      .method = HTTP_POST, .handler = delete_handler },
        { .uri = "/isp",         .method = HTTP_GET,  .handler = isp_get_handler },
        { .uri = "/isp",         .method = HTTP_POST, .handler = isp_post_handler },
        { .uri = "/audio",       .method = HTTP_POST, .handler = audio_handler },
        { .uri = "/favicon.ico", .method = HTTP_GET,  .handler = favicon_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    asr_register(server);
    return true;
}

/* ---- Inicialização --------------------------------------------------------- */

/*
 * Quem responde no I2C da câmera. Na DEV-KIT o mesmo barramento tem o codec
 * ES8311 (0x18) e o touch GT911 (0x14/0x5d): se eles aparecem e a IMX708
 * (0x1a) não, o barramento está bom e o problema é cabo/conector da câmera.
 */
/* O barramento I2C da placa (câmera, codec, touch). Quem o cria é o esp_video,
 * ao iniciar a câmera; se ela falhou antes disso, abre aqui. */
static i2c_master_bus_handle_t board_i2c_bus(void)
{
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_master_get_bus_handle(CAM_SCCB_I2C_PORT, &bus) == ESP_OK) {
        return bus;
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port = CAM_SCCB_I2C_PORT,
        .sda_io_num = CAM_SCCB_SDA_PIN,
        .scl_io_num = CAM_SCCB_SCL_PIN,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "não consegui abrir o barramento I2C");
        return NULL;
    }
    return bus;
}

/* Lê 2 bytes do registrador de 16 bits reg (o chip-ID dos sensores). */
static bool i2c_read_id(i2c_master_bus_handle_t bus, uint16_t addr, uint16_t reg, uint8_t id[2])
{
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = CAM_SCCB_FREQ_HZ,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        return false;
    }
    uint8_t r[2] = { reg >> 8, reg & 0xff };
    bool ok = i2c_master_transmit_receive(dev, r, 2, id, 2, 50) == ESP_OK;
    i2c_master_bus_rm_device(dev);
    return ok;
}

/* O nome do sensor no JSON de tuning: o esp_video escolhe sozinho entre a
 * IMX708 e a OV5647 e não conta qual. */
static const char *sensor_name(i2c_master_bus_handle_t bus)
{
    uint8_t id[2];
    if (bus && i2c_read_id(bus, 0x36, 0x300a, id) && id[0] == 0x56 && id[1] == 0x47) {
        return "OV5647";
    }
    if (bus && i2c_read_id(bus, 0x1a, 0x0016, id) && id[0] == 0x07 && id[1] == 0x08) {
        return "IMX708";
    }
    return NULL;
}

static void i2c_scan(void)
{
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) {
        return;
    }
    char found[160] = "";
    size_t used = 0;
    bool has_0x36 = false;
    for (uint16_t addr = 0x08; addr < 0x78; addr++) {
        if (i2c_master_probe(bus, addr, 20) == ESP_OK && used < sizeof(found)) {
            used += snprintf(found + used, sizeof(found) - used, " 0x%02x", addr);
            has_0x36 |= addr == 0x36;
        }
    }
    ESP_LOGW(TAG, "varredura I2C (SDA=%d SCL=%d):%s", CAM_SCCB_SDA_PIN, CAM_SCCB_SCL_PIN,
             used ? found : " ninguém respondeu");
    ESP_LOGW(TAG, "esperado: 0x1a = IMX708 (0x0c = seu motor de foco), 0x36 = OV5647, 0x18 = codec da placa");

    /* 0x36 é o endereço das OmniVision (OV5647 & cia): lê o chip-ID. */
    uint8_t id[2];
    if (has_0x36 && i2c_read_id(bus, 0x36, 0x300a, id)) {
        ESP_LOGW(TAG, "0x36 chip-ID (0x300a) = 0x%02x%02x%s", id[0], id[1],
                 id[0] == 0x56 && id[1] == 0x47 ? " (OV5647)" : "");
    }
}

static esp_err_t camera_start(void)
{
    /* esp_video_init devolve OK mesmo sem achar o sensor; quem denuncia é o
     * open() do /dev/video0, que só existe se a IMX708 respondeu. */
    if (esp_video_init(&cam_config) != ESP_OK) {
        ESP_LOGE(TAG, "esp_video_init falhou");
        i2c_scan();
        return ESP_FAIL;
    }

    s_fd = open(CAM_DEV_PATH, O_RDONLY);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "%s não existe: nenhum sensor respondeu no I2C", CAM_DEV_PATH);
        i2c_scan();
        return ESP_FAIL;
    }

    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    struct v4l2_format fmt = { .type = type };
    if (ioctl(s_fd, VIDIOC_G_FMT, &fmt) != 0) {
        ESP_LOGE(TAG, "VIDIOC_G_FMT falhou");
        return ESP_FAIL;
    }
    /* O encoder JPEG e os modelos esperam RGB565; pede ao ISP explicitamente. */
    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
        if (ioctl(s_fd, VIDIOC_S_FMT, &fmt) != 0 || fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_RGB565) {
            ESP_LOGE(TAG, "o ISP não aceitou RGB565");
            return ESP_FAIL;
        }
    }
    s_w = fmt.fmt.pix.width;
    s_h = fmt.fmt.pix.height;
    s_frame_len = (size_t)s_w * s_h * 2;
    ESP_LOGI(TAG, "formato %" PRIu32 "x%" PRIu32 " RGB565, %u bytes/quadro",
             s_w, s_h, (unsigned)s_frame_len);

    struct v4l2_requestbuffers req = { .count = BUFFER_COUNT, .type = type, .memory = V4L2_MEMORY_MMAP };
    if (ioctl(s_fd, VIDIOC_REQBUFS, &req) != 0) {
        ESP_LOGE(TAG, "VIDIOC_REQBUFS falhou");
        return ESP_FAIL;
    }
    for (int i = 0; i < BUFFER_COUNT; i++) {
        struct v4l2_buffer b = { .type = type, .memory = V4L2_MEMORY_MMAP, .index = i };
        if (ioctl(s_fd, VIDIOC_QUERYBUF, &b) != 0) {
            ESP_LOGE(TAG, "VIDIOC_QUERYBUF %d falhou", i);
            return ESP_FAIL;
        }
        s_buf[i] = mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, s_fd, b.m.offset);
        if (s_buf[i] == MAP_FAILED || ioctl(s_fd, VIDIOC_QBUF, &b) != 0) {
            ESP_LOGE(TAG, "mmap/QBUF %d falhou", i);
            return ESP_FAIL;
        }
    }

    jpeg_encode_engine_cfg_t eng = { .timeout_ms = 1000 };
    if (jpeg_new_encoder_engine(&eng, &s_jpeg) != ESP_OK) {
        ESP_LOGE(TAG, "encoder JPEG de hardware indisponível");
        return ESP_FAIL;
    }
    /* Saída com metade do tamanho em RGB565 - um JPEG mesmo a q95 fica bem
     * abaixo. Vale para a foto e para o recorte. */
    jpeg_encode_memory_alloc_cfg_t out = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    jpeg_encode_memory_alloc_cfg_t in = { .buffer_direction = JPEG_ENC_ALLOC_INPUT_BUFFER };
    size_t cap_a = 0, cap_b = 0, cap_rgb = 0;
    s_work = jpeg_alloc_encoder_mem((size_t)s_w * s_h, &out, &cap_a);
    s_latest = jpeg_alloc_encoder_mem((size_t)s_w * s_h, &out, &cap_b);
    s_lock = xSemaphoreCreateMutex();
    s_crop_rgb = jpeg_alloc_encoder_mem(CROP_MAX_PX * CROP_MAX_PX * 2, &in, &cap_rgb);
    s_crop_jpg = jpeg_alloc_encoder_mem(CROP_MAX_PX * CROP_MAX_PX, &out, &s_crop_jpg_cap);
    /* Por rosto: o JPEG em base64 (4 a cada 3 bytes, mais o \0 do mbedtls)
     * e o resto do evento. */
    s_crops_cap = FACE_MAX_BOXES * (4 * ((s_crop_jpg_cap + 2) / 3) + 1 + 160);
    s_crops_work = heap_caps_malloc(s_crops_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_crops_latest = heap_caps_malloc(s_crops_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_crops_lock = xSemaphoreCreateMutex();
    /* Imagem inteira: o JPEG pronto e a cópia que está sendo enviada, até 3/8
     * de byte por pixel (~460 KB em 1280x960; q60 dá ~115 KB). JPEG maior é
     * descartado e conta em encode_failed. */
    s_frame_cap = (size_t)s_w * s_h * 3 / 8;
    s_frame_latest = heap_caps_malloc(s_frame_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_frame_send = heap_caps_malloc(s_frame_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_frame_lock = xSemaphoreCreateMutex();
    if (!s_work || !s_latest || !s_lock || !s_crop_rgb || !s_crop_jpg || !s_crops_work || !s_crops_latest ||
        !s_crops_lock || !s_frame_latest || !s_frame_send || !s_frame_lock) {
        ESP_LOGE(TAG, "sem PSRAM para os buffers JPEG");
        return ESP_ERR_NO_MEM;
    }
    s_jpeg_cap = cap_a < cap_b ? cap_a : cap_b;

    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON falhou");
        return ESP_FAIL;
    }
    /* Core 0: o core 1 é da IA. Prioridade acima dos espectadores, para o
     * sensor nunca ficar sem buffer por causa de um envio. */
    s_released = xQueueCreate(BUFFER_COUNT, sizeof(int));
    if (!s_released) {
        ESP_LOGE(TAG, "sem memória para a fila de buffers");
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(capture_task, "capture", 6144, NULL, 6, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "falha ao criar a task de captura");
        return ESP_FAIL;
    }
    return ESP_OK;
}

void app_main(void)
{
    ESP_LOGI(TAG, "IMX708 + reconhecimento facial -> caixas e recortes via Wi-Fi");

    /* Sem câmera, segue mesmo assim: Wi-Fi e página sobem (o vídeo responde
     * 503), o que já testa o link do C6 e deixa /status acessível. */
    s_camera_ok = camera_start() == ESP_OK;
    if (!s_camera_ok) {
        ESP_LOGE(TAG, "==== câmera falhou - seguindo sem vídeo ====");
    } else if (face_init((int)s_w, (int)s_h, face_release) != ESP_OK) {
        /* Sem IA o vídeo continua, só sem caixas. */
        ESP_LOGE(TAG, "==== reconhecimento facial falhou - seguindo só com vídeo ====");
    } else {
        atomic_store(&s_ai_ok, true);
    }

    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (s_camera_ok) {
        const char *sensor = sensor_name(bus);
        if (sensor) {
            isp_tune_init(s_fd, sensor);
        } else {
            ESP_LOGW(TAG, "sensor não identificado pelo chip-ID: ajuste do ISP desligado");
        }
    }
    /* Sem microfone a câmera segue igual; o slider da página fica parado. */
    if (audio_start(bus) != ESP_OK) {
        ESP_LOGE(TAG, "==== microfone (ES8311) falhou - seguindo sem áudio ====");
    }
    /* Página de fala: sem microfone ela ainda mede as frases enviadas. */
    if (asr_init() != ESP_OK) {
        ESP_LOGE(TAG, "==== página de fala não iniciou ====");
    }

    /* Wi-Fi depois: AE/AWB/AF convergem enquanto associa. */
    char ip[16];
    if (wifi_connect(ip, sizeof(ip)) != ESP_OK) {
        ESP_LOGE(TAG, "==== câmera ok, Wi-Fi falhou ====");
        return;
    }

    if (!start_server()) {
        ESP_LOGE(TAG, "==== câmera e Wi-Fi ok, servidor falhou ====");
        return;
    }

    ESP_LOGI(TAG, "==== abra no navegador: http://%s/ ====", ip);
    ESP_LOGI(TAG, "  eventos: http://%s/events   foto inteira: http://%s/capture.jpg", ip, ip);
}
