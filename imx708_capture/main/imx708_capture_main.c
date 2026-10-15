/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * IMX708 (Pi Camera Module 3) + reconhecimento facial, ao vivo no navegador.
 * Placa: Waveshare ESP32-P4-Module-DEV-KIT. O P4 não tem rádio: o Wi-Fi é o
 * ESP32-C6 da placa, falando com o P4 por SDIO (esp_hosted + esp_wifi_remote).
 *
 *   GET  /              página: vídeo com caixas e IDs, cadastro, apagar
 *   GET  /stream        MJPEG (multipart/x-mixed-replace): <img>, VLC, ffplay
 *   GET  /capture.jpg   um quadro JPEG, com as caixas
 *   GET  /status        contadores, rostos na imagem e IDs cadastrados (JSON)
 *   GET  /quality?q=N   qualidade JPEG ao vivo (1..100)
 *   POST /enroll        cadastra o maior rosto da imagem; devolve o ID
 *   POST /delete?id=N   apaga um cadastro (id=all apaga todos)
 *
 * Sensor: IMX708 (1280x720) ou OV5647 (1280x960) - o esp_video usa o que
 * responder no I2C. Pipeline: CSI -> ISP (RGB565) -> [cópia p/ IA, se livre] ->
 * caixas + IDs desenhados no quadro -> encoder JPEG de hardware -> "último
 * quadro" publicado. Cada espectador copia o último quadro e envia no ritmo do
 * seu link; se o link for mais lento que a câmera, quadros são pulados.
 *
 * A IA (face.cpp) roda no core 1, no ritmo dela; o vídeo nunca espera por ela
 * e usa sempre o resultado mais recente. Por isso as caixas seguem um rosto em
 * movimento com um atraso de um ciclo de IA.
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
#include "face.h"
#include "overlay.h"
#include "wifi.h"

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
 * segurar outro (~120 ms), a captura um terceiro pelo JPEG da web (~17 ms), e o
 * sensor precisa de pelo menos mais um livre para não perder quadro. */
#define BUFFER_COUNT        5

/* Medido com a OV5647 em 1280x960: q60 ~115 KB/quadro, q35 ~60 KB, q25 ~45 KB.
 * O link do C6 entrega uns 6-8 Mbit/s, então q35 dá ~2x os fps de q60 e a
 * imagem continua boa para ver e reconhecer rostos. */
#define JPEG_QUALITY_DEFAULT 35

/* Teto de quadros JPEG por segundo para a web. A captura, a cópia para a IA
 * e o JPEG rodam no mesmo laço: sem teto, o JPEG (~17 ms por quadro) segura a
 * câmera em ~25 fps e a IA passa a receber quadros mais velhos. Com o teto, a
 * captura fica na taxa da câmera e o tracking ganha - a web é secundária. */
#define STREAM_MAX_FPS      10

/* Cada espectador custa uma task, um buffer em PSRAM e um socket; e todos
 * dividem a mesma banda. */
#define MAX_VIEWERS         2
#define STREAM_TASK_STACK   6144

/* Sem quadro novo por esse tempo, o espectador é desconectado. */
#define FRAME_WAIT_US       (3 * 1000 * 1000)

#define PART_BOUNDARY       "imx708frame"

/* ---- Desenho das caixas ---------------------------------------------------- */
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
 * Dois buffers JPEG: o encoder escreve em s_work e, pronto, troca com
 * s_latest sob o lock. Leitores copiam s_latest sob o mesmo lock (uma cópia
 * de ~50 KB, < 1 ms) e enviam a cópia sem segurar nada - um envio lento
 * nunca trava a câmera.
 */
static uint8_t *s_work, *s_latest;
static size_t s_jpeg_cap;
static size_t s_latest_len;
static volatile uint32_t s_latest_seq;
static SemaphoreHandle_t s_lock;

static atomic_int s_quality = JPEG_QUALITY_DEFAULT;
/* Só codifica JPEG quando alguém quer quadros (stream ou foto). A câmera e a
 * IA rodam sempre: AE/AWB/AF ficam convergidos e o log serial continua
 * mostrando quem foi reconhecido mesmo sem ninguém na página. */
static atomic_int s_consumers;
static atomic_int s_viewers;

static struct {
    uint32_t captured;
    uint32_t encoded;
    uint32_t encode_failed;
    uint32_t encode_us;
    uint32_t jpeg_bytes;
    uint32_t sent;
    uint64_t bytes_sent;
} s_stats;

/* ---- Captura + caixas + JPEG ---------------------------------------------- */

static void draw_faces(uint8_t *frame)
{
    face_box_t boxes[FACE_MAX_BOXES];
    size_t n = face_get_boxes(boxes, FACE_MAX_BOXES, BOX_MAX_AGE_MS);
    overlay_fb_t fb = { .px = (uint16_t *)frame, .w = (int)s_w, .h = (int)s_h };
    const int pad = LABEL_SCALE * 2;
    const int th = overlay_text_height(LABEL_SCALE);

    for (size_t i = 0; i < n; i++) {
        const face_box_t *b = &boxes[i];
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

/* Desenha as caixas e codifica o JPEG da web neste quadro, se alguém assiste e
 * já deu o intervalo de STREAM_MAX_FPS. */
static void maybe_encode(uint8_t *frame, jpeg_encode_cfg_t *cfg, int64_t *last_encode_us)
{
    int64_t now = esp_timer_get_time();
    if (atomic_load(&s_consumers) <= 0 || now - *last_encode_us < 1000000 / STREAM_MAX_FPS) {
        return;
    }
    *last_encode_us = now;
    draw_faces(frame);

    cfg->image_quality = atomic_load(&s_quality);
    uint32_t len = 0;
    int64_t t0 = esp_timer_get_time();
    esp_err_t err = jpeg_encoder_process(s_jpeg, cfg, frame, s_frame_len, s_work, s_jpeg_cap, &len);
    s_stats.encode_us = (uint32_t)(esp_timer_get_time() - t0);

    if (err == ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint8_t *done = s_work;
        s_work = s_latest;
        s_latest = done;
        s_latest_len = len;
        s_latest_seq++;
        xSemaphoreGive(s_lock);
        s_stats.encoded++;
        s_stats.jpeg_bytes = len;
    } else {
        s_stats.encode_failed++;
    }
}

/*
 * Prioridade é a IA: cada quadro novo vai primeiro para ela, sem cópia. Se ela
 * aceita, o buffer só volta (por s_released) quando ela termina - e aí, se for
 * a vez da web, recebe as caixas e vira JPEG antes de voltar para a câmera.
 * Quadros que a IA recusou (ocupada) também podem ir para a web.
 */
static void capture_task(void *arg)
{
    const int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    /* YUV422 funciona com qualquer altura múltipla de 8 (720, 960 e 1080). */
    jpeg_encode_cfg_t cfg = {
        .width = s_w,
        .height = s_h,
        .src_type = JPEG_ENCODE_IN_FORMAT_RGB565,
        .sub_sample = JPEG_DOWN_SAMPLING_YUV422,
    };
    int64_t last_encode_us = 0;

    while (1) {
        int v;
        while (xQueueReceive(s_released, &v, 0) == pdTRUE) {
            int idx = v & ~RELEASED_STALE;
            if (!(v & RELEASED_STALE)) {
                maybe_encode(s_buf[idx], &cfg, &last_encode_us);
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
        maybe_encode(s_buf[buf.index], &cfg, &last_encode_us);
        ioctl(s_fd, VIDIOC_QBUF, &buf);
    }
    vTaskDelete(NULL);
}

/* Espera um quadro mais novo que *seq e copia para dst (capacidade s_jpeg_cap). */
static esp_err_t grab_frame(uint8_t *dst, size_t *len, uint32_t *seq)
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

static const char INDEX_HTML[] =
    "<!doctype html><html lang=pt-br><meta charset=utf-8>"
    "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
    "<title>IMX708 rostos</title>"
    "<style>"
    "body{margin:0;background:#111;color:#ddd;font:14px system-ui,sans-serif}"
    "main{max-width:1280px;margin:auto;padding:12px}"
    "img{display:block;width:100%;height:auto;aspect-ratio:auto 16/9;background:#000}"
    ".bar{display:flex;flex-wrap:wrap;gap:12px;align-items:center;margin:10px 0}"
    "button{font:inherit;padding:6px 12px;border:0;border-radius:4px;background:#2a6;color:#fff;cursor:pointer}"
    "button.del{background:#a33}input[type=number]{width:5em;font:inherit}"
    "a{color:#6cf}#st,#ai{font-family:monospace;color:#9b9}#msg{font-weight:bold}"
    "</style><main>"
    "<img id=v src=\"/stream\" alt=\"vídeo da câmera\">"
    "<div class=bar>"
    "<button id=en>Cadastrar rosto</button><span id=msg></span>"
    "</div><div class=bar>"
    "<label>apagar ID <input id=did type=number min=1></label>"
    "<button class=del id=del>Apagar</button>"
    "<button class=del id=clr>Apagar todos</button>"
    "<span>cadastrados: <b id=ids>-</b></span>"
    "</div><div class=bar>"
    "<label>qualidade JPEG <input id=q type=range min=10 max=95 step=5> <b id=qv></b></label>"
    "<a href=\"/capture.jpg\" download=\"rosto.jpg\">salvar foto</a>"
    "<a href=\"/status\">status</a>"
    "</div><div id=ai></div><div id=st>conectando...</div></main>"
    "<script>"
    "const $=id=>document.getElementById(id),v=$('v'),q=$('q'),msg=$('msg');"
    "let last=null;"
    "v.onerror=()=>setTimeout(()=>v.src='/stream?'+Date.now(),1000);"
    "q.oninput=()=>$('qv').textContent=q.value;"
    "q.onchange=()=>fetch('/quality?q='+q.value);"
    "async function post(url){const r=await fetch(url,{method:'POST'});return r.json();}"
    "$('en').onclick=async()=>{msg.textContent='olhe para a câmera...';"
    "try{const j=await post('/enroll');"
    "msg.textContent=j.ok?(j.new?'Cadastrado: ID '+j.id:'Já cadastrado: ID '+j.id+' ('+j.sim+'%)'):j.error;}"
    "catch(e){msg.textContent='sem resposta da placa';}};"
    "$('del').onclick=async()=>{const id=$('did').value;if(!id)return;"
    "const j=await post('/delete?id='+id);msg.textContent=j.ok?'ID '+id+' apagado':j.error;};"
    "$('clr').onclick=async()=>{if(!confirm('Apagar todos os cadastros?'))return;"
    "const j=await post('/delete?id=all');msg.textContent=j.ok?'todos apagados':j.error;};"
    "function faces(f){return f.length?f.map(x=>x.id>=0?'ID '+x.id+' ('+x.sim+'%)':'desconhecido').join(', '):'nenhum rosto';}"
    "async function poll(){"
    "try{const s=await(await fetch('/status')).json();"
    "$('ids').textContent=s.enrolled.length?s.enrolled.join(', '):'nenhum';"
    "if(!last){q.value=s.quality;$('qv').textContent=s.quality;}"
    "else{const dt=(s.uptime_ms-last.uptime_ms)/1000;"
    "$('ai').textContent='na imagem: '+faces(s.faces)+' · IA '+((s.ai_cycles-last.ai_cycles)/dt).toFixed(1)+' fps'"
    "+' (detecção '+(s.detect_us/1000).toFixed(0)+' ms, reconhecimento '+(s.recog_us/1000).toFixed(0)+' ms)';"
    "$('st').textContent=s.width+'x'+s.height+' · '+((s.encoded-last.encoded)/dt).toFixed(1)+' fps'"
    "+' · '+(s.jpeg_bytes/1024).toFixed(0)+' KB/quadro'"
    "+' · '+((s.bytes_sent-last.bytes_sent)*8/dt/1e6).toFixed(2)+' Mbit/s'"
    "+' · '+s.viewers+' assistindo';}"
    "last=s;}catch(e){$('st').textContent='sem resposta da placa';last=null;}"
    "setTimeout(poll,1000);}"
    "poll();"
    "</script>";

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, INDEX_HTML, sizeof(INDEX_HTML) - 1);
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

static void stream_task(void *arg)
{
    httpd_req_t *req = arg;
    uint8_t *frame = heap_caps_malloc(s_jpeg_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t frames = 0;
    uint64_t bytes = 0;
    int64_t t_start = esp_timer_get_time();

    if (!frame) {
        ESP_LOGE(TAG, "sem PSRAM para o espectador");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria");
        goto done;
    }

    /* Sem isto, o Nagle segura o final de cada quadro até o ACK do anterior,
     * que o navegador atrasa (delayed ACK): a taxa despenca sem motivo. */
    int one = 1;
    setsockopt(httpd_req_to_sockfd(req), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" PART_BOUNDARY);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    atomic_fetch_add(&s_consumers, 1);
    ESP_LOGI(TAG, "espectador entrou (%d assistindo)", atomic_load(&s_viewers));

    uint32_t seq = s_latest_seq;
    size_t len;
    while (grab_frame(frame, &len, &seq) == ESP_OK) {
        char part[128];
        int n = snprintf(part, sizeof(part),
                         "\r\n--" PART_BOUNDARY "\r\nContent-Type: image/jpeg\r\n"
                         "Content-Length: %u\r\n\r\n", (unsigned)len);
        if (httpd_resp_send_chunk(req, part, n) != ESP_OK ||
            httpd_resp_send_chunk(req, (const char *)frame, len) != ESP_OK) {
            break;
        }
        frames++;
        bytes += len;
        s_stats.sent++;
        s_stats.bytes_sent += len;
    }
    atomic_fetch_sub(&s_consumers, 1);

    uint32_t ms = (uint32_t)((esp_timer_get_time() - t_start) / 1000);
    ESP_LOGI(TAG, "espectador saiu: %" PRIu32 " quadros em %" PRIu32 " ms (%" PRIu32 ".%" PRIu32
                  " fps, %" PRIu32 " kbit/s)",
             frames, ms, ms ? frames * 1000 / ms : 0, ms ? (frames * 10000 / ms) % 10 : 0,
             ms ? (uint32_t)(bytes * 8 / ms) : 0);

done:
    free(frame);
    httpd_req_async_handler_complete(req);
    atomic_fetch_sub(&s_viewers, 1);
    vTaskDelete(NULL);
}

/*
 * O httpd atende uma requisição por vez numa única task, e um stream nunca
 * termina. Por isso cada stream vai para uma task própria (handler
 * assíncrono) e o servidor continua livre para página, foto e status.
 */
static esp_err_t camera_down(httpd_req_t *req)
{
    httpd_resp_set_status(req, "503 Service Unavailable");
    return httpd_resp_sendstr(req, "a camera nao iniciou - veja o log serial");
}

static esp_err_t stream_handler(httpd_req_t *req)
{
    if (!s_camera_ok) {
        return camera_down(req);
    }
    if (atomic_fetch_add(&s_viewers, 1) >= MAX_VIEWERS) {
        atomic_fetch_sub(&s_viewers, 1);
        httpd_resp_set_status(req, "503 Service Unavailable");
        return httpd_resp_sendstr(req, "limite de espectadores atingido");
    }

    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        atomic_fetch_sub(&s_viewers, 1);
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "async falhou");
    }
    if (xTaskCreate(stream_task, "stream", STREAM_TASK_STACK, copy, 5, NULL) != pdPASS) {
        httpd_req_async_handler_complete(copy);
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

    /* Espera um quadro codificado depois desta requisição: sem stream aberto o
     * último JPEG pode ser velho. */
    atomic_fetch_add(&s_consumers, 1);
    uint32_t seq = s_latest_seq;
    size_t len = 0;
    esp_err_t err = grab_frame(frame, &len, &seq);
    atomic_fetch_sub(&s_consumers, 1);

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

/* snprintf que acumula num buffer e nunca passa do fim. */
#define APPEND(buf, used, ...)                                                        \
    do {                                                                              \
        if ((used) < sizeof(buf)) {                                                   \
            int _n = snprintf((buf) + (used), sizeof(buf) - (used), __VA_ARGS__);     \
            (used) += _n > 0 ? (size_t)_n : 0;                                        \
        }                                                                             \
    } while (0)

static esp_err_t status_handler(httpd_req_t *req)
{
    char body[1024];
    size_t used = 0;
    face_stats_t ai;
    face_get_stats(&ai);

    APPEND(body, used,
           "{\"width\":%" PRIu32 ",\"height\":%" PRIu32 ",\"quality\":%d,\"viewers\":%d,"
           "\"captured\":%" PRIu32 ",\"encoded\":%" PRIu32 ",\"encode_failed\":%" PRIu32 ","
           "\"encode_us\":%" PRIu32 ",\"jpeg_bytes\":%" PRIu32 ",\"sent\":%" PRIu32 ","
           "\"bytes_sent\":%" PRIu64 ",\"uptime_ms\":%" PRId64 ","
           "\"ai_cycles\":%" PRIu32 ",\"detect_us\":%" PRIu32 ",\"recog_us\":%" PRIu32 ","
           "\"heap_internal\":%u,\"heap_psram\":%u,\"faces\":[",
           s_w, s_h, atomic_load(&s_quality), atomic_load(&s_viewers),
           s_stats.captured, s_stats.encoded, s_stats.encode_failed,
           s_stats.encode_us, s_stats.jpeg_bytes, s_stats.sent,
           s_stats.bytes_sent, esp_timer_get_time() / 1000,
           ai.cycles, ai.detect_us, ai.recog_us,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    face_box_t boxes[FACE_MAX_BOXES];
    size_t n = face_get_boxes(boxes, FACE_MAX_BOXES, BOX_MAX_AGE_MS);
    for (size_t i = 0; i < n; i++) {
        APPEND(body, used, "%s{\"id\":%d,\"sim\":%u,\"box\":[%d,%d,%d,%d]}", i ? "," : "",
               boxes[i].id, boxes[i].sim_pct, boxes[i].x0, boxes[i].y0, boxes[i].x1, boxes[i].y1);
    }

    int ids[64];
    size_t total = face_list(ids, sizeof(ids) / sizeof(ids[0]));
    APPEND(body, used, "],\"enrolled_total\":%u,\"enrolled\":[", (unsigned)total);
    for (size_t i = 0; i < total && i < sizeof(ids) / sizeof(ids[0]); i++) {
        APPEND(body, used, "%s%d", i ? "," : "", ids[i]);
    }
    APPEND(body, used, "]}\n");

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

static bool start_server(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 6144;
    /* Os streams seguram seus sockets; sobram 3 para página, foto e status. */
    cfg.max_open_sockets = MAX_VIEWERS + 3;
    cfg.max_uri_handlers = 10;
    cfg.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start falhou");
        return false;
    }

    static const httpd_uri_t uris[] = {
        { .uri = "/",            .method = HTTP_GET,  .handler = index_handler },
        { .uri = "/stream",      .method = HTTP_GET,  .handler = stream_handler },
        { .uri = "/capture.jpg", .method = HTTP_GET,  .handler = capture_handler },
        { .uri = "/status",      .method = HTTP_GET,  .handler = status_handler },
        { .uri = "/quality",     .method = HTTP_GET,  .handler = quality_handler },
        { .uri = "/enroll",      .method = HTTP_POST, .handler = enroll_handler },
        { .uri = "/delete",      .method = HTTP_POST, .handler = delete_handler },
        { .uri = "/favicon.ico", .method = HTTP_GET,  .handler = favicon_handler },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    return true;
}

/* ---- Inicialização --------------------------------------------------------- */

/*
 * Quem responde no I2C da câmera. Na DEV-KIT o mesmo barramento tem o codec
 * ES8311 (0x18) e o touch GT911 (0x14/0x5d): se eles aparecem e a IMX708
 * (0x1a) não, o barramento está bom e o problema é cabo/conector da câmera.
 */
static void i2c_scan(void)
{
    i2c_master_bus_handle_t bus = NULL;
    if (i2c_master_get_bus_handle(CAM_SCCB_I2C_PORT, &bus) != ESP_OK) {
        i2c_master_bus_config_t cfg = {
            .i2c_port = CAM_SCCB_I2C_PORT,
            .sda_io_num = CAM_SCCB_SDA_PIN,
            .scl_io_num = CAM_SCCB_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 7,
            .flags.enable_internal_pullup = true,
        };
        if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
            ESP_LOGE(TAG, "varredura I2C: não consegui abrir o barramento");
            return;
        }
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
    if (has_0x36) {
        i2c_device_config_t dev_cfg = {
            .dev_addr_length = I2C_ADDR_BIT_LEN_7,
            .device_address = 0x36,
            .scl_speed_hz = CAM_SCCB_FREQ_HZ,
        };
        i2c_master_dev_handle_t dev;
        if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) == ESP_OK) {
            uint8_t reg[2] = {0x30, 0x0a}, id[2] = {0};
            if (i2c_master_transmit_receive(dev, reg, 2, id, 2, 50) == ESP_OK) {
                ESP_LOGW(TAG, "0x36 chip-ID (0x300a) = 0x%02x%02x%s", id[0], id[1],
                         id[0] == 0x56 && id[1] == 0x47 ? " (OV5647)" : "");
            }
            i2c_master_bus_rm_device(dev);
        }
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
    /* Metade do quadro RGB565 - um JPEG mesmo a q95 fica bem abaixo. */
    jpeg_encode_memory_alloc_cfg_t mem = { .buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER };
    size_t cap_a = 0, cap_b = 0;
    s_work = jpeg_alloc_encoder_mem((size_t)s_w * s_h, &mem, &cap_a);
    s_latest = jpeg_alloc_encoder_mem((size_t)s_w * s_h, &mem, &cap_b);
    s_lock = xSemaphoreCreateMutex();
    if (!s_work || !s_latest || !s_lock) {
        ESP_LOGE(TAG, "sem PSRAM para os buffers JPEG");
        return ESP_ERR_NO_MEM;
    }
    s_jpeg_cap = cap_a < cap_b ? cap_a : cap_b;

    if (ioctl(s_fd, VIDIOC_STREAMON, &type) != 0) {
        ESP_LOGE(TAG, "VIDIOC_STREAMON falhou");
        return ESP_FAIL;
    }
    /* Core 0: o core 1 é da IA. Prioridade acima dos streams, para o sensor
     * nunca ficar sem buffer por causa de um envio. */
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
    ESP_LOGI(TAG, "IMX708 + reconhecimento facial -> MJPEG via Wi-Fi");

    /* Sem câmera, segue mesmo assim: Wi-Fi e página sobem (o vídeo responde
     * 503), o que já testa o link do C6 e deixa /status acessível. */
    s_camera_ok = camera_start() == ESP_OK;
    if (!s_camera_ok) {
        ESP_LOGE(TAG, "==== câmera falhou - seguindo sem vídeo ====");
    } else if (face_init((int)s_w, (int)s_h, face_release) != ESP_OK) {
        /* Sem IA o vídeo continua, só sem caixas. */
        ESP_LOGE(TAG, "==== reconhecimento facial falhou - seguindo só com vídeo ====");
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
    ESP_LOGI(TAG, "  stream direto: http://%s/stream   foto: http://%s/capture.jpg", ip, ip);
}
