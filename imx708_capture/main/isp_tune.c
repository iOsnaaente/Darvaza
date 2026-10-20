/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ajuste do ISP ao vivo (ver isp_tune.h).
 *
 * A configuração do IPA que o esp_video carregou (s_orig, gerada do JSON no
 * build) nunca é alterada. Cada ajuste monta uma cópia dela num de dois
 * "bancos" em PSRAM, com as tabelas trocadas, e pede ao pipeline para usar o
 * banco novo. O IPA guarda o ponteiro e lê as tabelas a cada quadro: por isso
 * o banco que está em uso nunca é editado - o próximo ajuste vai no outro.
 *
 * Tabelas com várias entradas (por ganho do sensor ou temperatura de cor)
 * recebem o mesmo valor em todas. Na OV5647 cada uma tem uma entrada só; e o
 * driver dela nem informa o ganho ao IPA, então tabelas por ganho não
 * mudariam nada.
 *
 * O alvo do AE do sensor tem dois donos: o slider "ae" e a exposição pelo
 * rosto (face_ae_task, mais abaixo). s_lock protege o alvo e s_cur entre a
 * task do httpd e a do rosto.
 */
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "linux/videodev2.h"
#include "esp_video_isp_pipeline.h"
#include "face.h"
#include "isp_tune.h"

static const char *TAG = "isp_tune";

#define TABLE_MAX       16          /* entradas por tabela que dá para ajustar */
#define NVS_NAMESPACE   "isp_tune"
#define NVS_KEY         "v2"        /* troque se tune_t mudar: o blob antigo é ignorado */
#define CCM_LIMIT       3.99f       /* coeficientes da CCM no P4: (-4, 4) */

/* Exposição pelo rosto (face_ae_task) */
#define FACE_AE_TARGET_DEF  120                 /* brilho padrão do miolo do rosto */
#define FACE_AE_TASK_STACK  3072
#define FACE_AE_TASK_PRIO   2
#define FACE_AE_POLL_MS     40                  /* olha o resultado da IA a cada */
#define FACE_AE_SETTLE_US   (300 * 1000)        /* o AE do sensor leva ~10 quadros até o alvo novo */
#define FACE_AE_HOLD_US     (3000 * 1000)       /* rosto sumiu: segura o ajuste por */
#define FACE_AE_BACK_US     (300 * 1000)        /* depois volta ao slider, um passo a cada */
#define FACE_AE_BACK_FRAC   0.3f                /* fração do caminho de volta por passo */
#define FACE_AE_SEEN_US     (500 * 1000)        /* rosto "na imagem" para a página */
#define FACE_AE_TOL         1.08f               /* rosto até 8% fora do alvo: não mexe */
#define FACE_AE_MAX_STEP    1.5f                /* o alvo muda no máximo x1,5 por ajuste */

/* O gerador do esp_ipa (tools/config/isp/aen.py) faz a curva assim:
 * x = 16, 32, ..., 240, 255 e y = 255 * (x / 240.0625)^gamma, saturado. */
#define GAMMA_X_REF     240.0625

typedef struct {
    int ae;                 /* alvo de exposição do sensor (V4L2_CID_EXPOSURE) */
    float gamma;
    int contrast;
    int saturation;
    float ccm;              /* força da matriz de cor: 0 = identidade, 1 = a do JSON */
    float red, blue;        /* ganho do canal antes da matriz (corrige o tom) */
    int denoise;            /* nível do filtro bilateral (bf.level) */
    float sharp_m, sharp_h; /* nitidez: coeficientes médio e alto */
    int sharp_l;            /* abaixo disto a borda é tratada como ruído */
    bool gamma_set;         /* false = curva original do JSON; gamma é só a estimativa */
    int face;               /* 1 = exposição pelo rosto: com rosto, o "ae" é dela */
    int face_luma;          /* brilho que ela busca no miolo do rosto (0-255, na saída) */
} tune_t;

enum {
    HAS_AE       = 1 << 0,
    HAS_GAMMA    = 1 << 1,
    HAS_CONTRAST = 1 << 2,
    HAS_SAT      = 1 << 3,
    HAS_CCM      = 1 << 4,
    HAS_BF       = 1 << 5,
    HAS_SHARPEN  = 1 << 6,
};

typedef struct {
    const char *key;
    bool is_float;
    float min, max, step;
    size_t off;
    unsigned need;
} param_t;

#define P_INT(k, lo, hi, f, h)          { k, false, lo, hi, 1, offsetof(tune_t, f), h }
#define P_FLT(k, lo, hi, st, f, h)      { k, true, lo, hi, st, offsetof(tune_t, f), h }

static const param_t PARAMS[] = {
    P_INT("ae",         2,    235,        ae,         HAS_AE),   /* faixa real vem do sensor */
    P_INT("face",       0,    1,          face,       HAS_AE),
    P_INT("face_luma",  40,   220,        face_luma,  HAS_AE),
    P_FLT("gamma",     0.30, 1.50, 0.01, gamma,      HAS_GAMMA),
    P_INT("contrast",   0,    255,        contrast,   HAS_CONTRAST),
    P_INT("saturation", 0,    255,        saturation, HAS_SAT),
    P_FLT("ccm",        0,    1.50, 0.05, ccm,        HAS_CCM),
    P_FLT("red",        0.50, 2.00, 0.01, red,        HAS_CCM),
    P_FLT("blue",       0.50, 2.00, 0.01, blue,       HAS_CCM),
    P_INT("denoise",    2,    20,         denoise,    HAS_BF),
    P_FLT("sharp_m",    0.05, 4.00, 0.05, sharp_m,    HAS_SHARPEN),
    P_FLT("sharp_h",    0.05, 4.00, 0.05, sharp_h,    HAS_SHARPEN),
    P_INT("sharp_l",    0,    64,         sharp_l,    HAS_SHARPEN),
};
#define PARAM_COUNT (sizeof(PARAMS) / sizeof(PARAMS[0]))

/* Uma configuração completa do IPA com as tabelas ajustáveis em RAM. */
typedef struct {
    esp_ipa_config_t top;
    esp_ipa_adn_config_t adn;
    esp_ipa_adn_bf_t bf[TABLE_MAX];
    esp_ipa_aen_config_t aen;
    esp_ipa_aen_gamma_config_t gamma;
    esp_ipa_aen_gamma_unit_t gamma_tbl[TABLE_MAX];
    esp_ipa_aen_sharpen_t sharpen[TABLE_MAX];
    esp_ipa_aen_con_t con[TABLE_MAX];
    esp_ipa_acc_config_t acc;
    esp_ipa_acc_sat_t sat[TABLE_MAX];
    esp_ipa_acc_ccm_config_t ccm;
    esp_ipa_acc_ccm_unit_t ccm_tbl[TABLE_MAX];
} bank_t;

typedef struct {
    char sensor[16];
    tune_t t;
} saved_t;

static int s_fd = -1;
static char s_sensor[16];
static const esp_ipa_config_t *s_orig;
static bank_t *s_bank;              /* 2 bancos, em PSRAM */
static int s_active = -1;           /* banco em uso; -1 = s_orig */
static unsigned s_has;
static int s_ae_min, s_ae_max;
static tune_t s_def, s_cur;
static bool s_saved;                /* s_cur é o que está na NVS */

static SemaphoreHandle_t s_lock;    /* s_cur e o alvo do AE: httpd x face_ae_task */
static int s_sensor_ae = -1;        /* último alvo escrito no sensor */
static int s_face_ae = -1;          /* alvo imposto pelo rosto; -1 = vale o do slider */
static int s_face_luma = -1;        /* último brilho de rosto medido */
static int64_t s_face_us;           /* quando ele foi medido */

/* ---- tabelas ---------------------------------------------------------------- */

static bool table_ok(const void *table, uint32_t size)
{
    return table && size > 0 && size <= TABLE_MAX;
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void gamma_curve(esp_ipa_gamma_channel_t *ch, float g)
{
    for (int i = 0; i < ISP_GAMMA_CURVE_POINTS_NUM; i++) {
        int x = (i + 1) * 16;
        if (x > 255) {
            x = 255;
        }
        double y = 255.0 * pow(x / GAMMA_X_REF, g);
        ch->x[i] = (uint8_t)x;
        ch->y[i] = (uint8_t)(y > 255.0 ? 255 : (int)(y + 0.5));
    }
}

/* O inverso de gamma_curve num ponto do meio da curva verde: devolve o gamma
 * do JSON quando ele foi gerado por "gamma_param" (é o caso da OV5647). */
static float gamma_estimate(const esp_ipa_gamma_t *gm)
{
    const int i = ISP_GAMMA_CURVE_POINTS_NUM / 2 - 1;     /* x = 128 */
    int x = gm->green.x[i], y = gm->green.y[i];
    if (x <= 0 || x >= 240 || y <= 0 || y >= 255) {
        return 1.0f;
    }
    float g = (float)(log(y / 255.0) / log(x / GAMMA_X_REF));
    return roundf(clampf(g, 0.30f, 1.50f) * 100) / 100;
}

/* M' = (I + s*(M - I)) * diag(r, 1, b): s afasta ou aproxima da identidade
 * mantendo a soma das linhas (o branco continua branco); r e b multiplicam o
 * canal de entrada, como o balanço de branco do ISP faz no P4 rev < 3. */
static void ccm_transform(const esp_ipa_ccm_t *in, esp_ipa_ccm_t *out, const tune_t *t)
{
    for (int i = 0; i < ISP_CCM_DIMENSION; i++) {
        for (int j = 0; j < ISP_CCM_DIMENSION; j++) {
            float id = i == j ? 1.0f : 0.0f;
            float v = id + t->ccm * (in->matrix[i][j] - id);
            if (j == 0) {
                v *= t->red;
            } else if (j == 2) {
                v *= t->blue;
            }
            out->matrix[i][j] = clampf(v, -CCM_LIMIT, CCM_LIMIT);
        }
    }
}

/* Monta no banco livre a configuração de t e passa o pipeline para ela. */
static esp_err_t apply_isp(const tune_t *t)
{
    int next = s_active == 0 ? 1 : 0;
    bank_t *b = &s_bank[next];
    const esp_ipa_config_t *o = s_orig;
    b->top = *o;

    if (s_has & HAS_BF) {
        b->adn = *o->adn;
        for (uint32_t i = 0; i < o->adn->bf_table_size; i++) {
            b->bf[i] = o->adn->bf_table[i];
            b->bf[i].bf.level = (uint8_t)t->denoise;
        }
        b->adn.bf_table = b->bf;
        b->top.adn = &b->adn;
    }

    if (s_has & (HAS_GAMMA | HAS_SHARPEN | HAS_CONTRAST)) {
        b->aen = *o->aen;
        if (s_has & HAS_GAMMA) {
            const esp_ipa_aen_gamma_config_t *og = o->aen->gamma;
            b->gamma = *og;
            for (uint32_t i = 0; i < og->gamma_table_size; i++) {
                b->gamma_tbl[i] = og->gamma_table[i];
                if (t->gamma_set) {
                    gamma_curve(&b->gamma_tbl[i].gamma.red, t->gamma);
                    gamma_curve(&b->gamma_tbl[i].gamma.green, t->gamma);
                    gamma_curve(&b->gamma_tbl[i].gamma.blue, t->gamma);
                }
            }
            b->gamma.gamma_table = b->gamma_tbl;
            b->aen.gamma = &b->gamma;
        }
        if (s_has & HAS_SHARPEN) {
            for (uint16_t i = 0; i < o->aen->sharpen_table_size; i++) {
                b->sharpen[i] = o->aen->sharpen_table[i];
                esp_ipa_sharpen_t *sh = &b->sharpen[i].sharpen;
                sh->m_coeff = t->sharp_m;
                sh->h_coeff = t->sharp_h;
                sh->l_thresh = (uint8_t)(t->sharp_l < sh->h_thresh ? t->sharp_l : sh->h_thresh);
            }
            b->aen.sharpen_table = b->sharpen;
        }
        if (s_has & HAS_CONTRAST) {
            for (uint16_t i = 0; i < o->aen->con_table_size; i++) {
                b->con[i] = o->aen->con_table[i];
                b->con[i].contrast = (uint32_t)t->contrast;
            }
            b->aen.con_table = b->con;
        }
        b->top.aen = &b->aen;
    }

    if (s_has & (HAS_SAT | HAS_CCM)) {
        b->acc = *o->acc;
        if (s_has & HAS_SAT) {
            for (uint32_t i = 0; i < o->acc->sat_table_size; i++) {
                b->sat[i] = o->acc->sat_table[i];
                b->sat[i].saturation = (uint32_t)t->saturation;
            }
            b->acc.sat_table = b->sat;
        }
        if (s_has & HAS_CCM) {
            const esp_ipa_acc_ccm_config_t *oc = o->acc->ccm;
            b->ccm = *oc;
            for (uint32_t i = 0; i < oc->ccm_table_size; i++) {
                b->ccm_tbl[i].color_temp = oc->ccm_table[i].color_temp;
                ccm_transform(&oc->ccm_table[i].ccm, &b->ccm_tbl[i].ccm, t);
            }
            ccm_transform(&oc->luma_low_ccm, &b->ccm.luma_low_ccm, t);
            b->ccm.ccm_table = b->ccm_tbl;
            b->acc.ccm = &b->ccm;
        }
        b->top.acc = &b->acc;
    }

    esp_err_t err = esp_video_isp_pipeline_set_ipa_config(&b->top);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "o pipeline recusou a configuração nova: %s", esp_err_to_name(err));
        return err;
    }
    s_active = next;
    return ESP_OK;
}

/* Alvo do AE no sensor, só se mudou. Com s_lock (ou antes da task do rosto). */
static esp_err_t sensor_ae(int value)
{
    if (value == s_sensor_ae) {
        return ESP_OK;
    }
    struct v4l2_ext_control c = { .id = V4L2_CID_EXPOSURE, .value = value };
    struct v4l2_ext_controls cs = { .ctrl_class = V4L2_CID_USER_CLASS, .count = 1, .controls = &c };
    if (ioctl(s_fd, VIDIOC_S_EXT_CTRLS, &cs) != 0) {
        ESP_LOGE(TAG, "o sensor recusou o alvo de exposição %d", value);
        return ESP_FAIL;
    }
    s_sensor_ae = value;
    return ESP_OK;
}

/* Campo a campo (memcmp pegaria o padding): tudo menos o ae, que é do sensor. */
static bool same_isp(const tune_t *a, const tune_t *b)
{
    return a->gamma == b->gamma && a->gamma_set == b->gamma_set && a->contrast == b->contrast &&
           a->saturation == b->saturation && a->ccm == b->ccm && a->red == b->red && a->blue == b->blue &&
           a->denoise == b->denoise && a->sharp_m == b->sharp_m && a->sharp_h == b->sharp_h &&
           a->sharp_l == b->sharp_l;
}

static bool same_all(const tune_t *a, const tune_t *b)
{
    return same_isp(a, b) && a->ae == b->ae && a->face == b->face && a->face_luma == b->face_luma;
}

/* Só o que mudou: trocar a configuração do IPA reinicia os módulos dele. Com
 * o rosto no comando, o "ae" novo fica guardado para quando ele sair. */
static esp_err_t apply(const tune_t *t, const tune_t *prev)
{
    if (!same_isp(t, prev)) {
        esp_err_t err = apply_isp(t);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (s_has & HAS_AE) {
        if (!t->face) {
            s_face_ae = -1;     /* desligou: o slider volta a valer na hora */
        }
        return sensor_ae(s_face_ae >= 0 ? s_face_ae : t->ae);
    }
    return ESP_OK;
}

/* ---- NVS ------------------------------------------------------------------- */

static bool load_saved(tune_t *t)
{
    /* O Wi-Fi também inicializa a NVS, mais tarde; aqui só se já estiver boa.
     * Se ela precisar ser apagada, quem apaga é o wifi_connect. */
    if (nvs_flash_init() != ESP_OK) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    saved_t sv;
    size_t len = sizeof(sv);
    bool ok = nvs_get_blob(h, NVS_KEY, &sv, &len) == ESP_OK && len == sizeof(sv) &&
              strncmp(sv.sensor, s_sensor, sizeof(sv.sensor)) == 0;
    nvs_close(h);
    if (ok) {
        *t = sv.t;
    }
    return ok;
}

esp_err_t isp_tune_save(void)
{
    if (!s_orig) {
        return ESP_ERR_INVALID_STATE;
    }
    saved_t sv = { .t = s_cur };
    strlcpy(sv.sensor, s_sensor, sizeof(sv.sensor));
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, NVS_KEY, &sv, sizeof(sv));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "falha ao gravar na NVS: %s", esp_err_to_name(err));
        return err;
    }
    s_saved = true;
    ESP_LOGI(TAG, "ajuste salvo (volta no próximo boot)");
    return ESP_OK;
}

esp_err_t isp_tune_reset(void)
{
    if (!s_orig) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = esp_video_isp_pipeline_set_ipa_config(s_orig);
    if (err != ESP_OK) {
        return err;
    }
    s_active = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_face_ae = -1;
    s_cur = s_def;
    if (s_has & HAS_AE) {
        sensor_ae(s_def.ae);
    }
    xSemaphoreGive(s_lock);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, NVS_KEY);
        nvs_commit(h);
        nvs_close(h);
    }
    s_saved = false;
    ESP_LOGI(TAG, "ajuste de volta ao padrão do firmware");
    return ESP_OK;
}

/* ---- API -------------------------------------------------------------------- */

static void param_get(const tune_t *t, const param_t *p, float *v)
{
    const char *base = (const char *)t + p->off;
    *v = p->is_float ? *(const float *)base : (float)*(const int *)base;
}

static void param_put(tune_t *t, const param_t *p, float v)
{
    char *base = (char *)t + p->off;
    if (p->is_float) {
        *(float *)base = v;
    } else {
        *(int *)base = (int)lroundf(v);
    }
}

static void param_range(const param_t *p, float *lo, float *hi)
{
    *lo = p->min;
    *hi = p->max;
    if (p->off == offsetof(tune_t, ae)) {
        *lo = (float)s_ae_min;
        *hi = (float)s_ae_max;
    }
}

/* ---- Exposição pelo rosto ---------------------------------------------------
 *
 * O AE da OV5647 mede a imagem inteira: com uma janela clara atrás, ele
 * escurece tudo e o rosto fica escuro demais para o reconhecimento. Com "face"
 * ligado, esta task lê o brilho do maior rosto que a IA publica
 * (face_result_t.luma) e move o alvo do AE do sensor até o rosto chegar em
 * face_luma. O slider "ae" passa a valer só sem rosto na imagem.
 *
 * Um ajuste por vez: depois de mexer no alvo, só usa quadros capturados
 * FACE_AE_SETTLE_US depois. Sem rosto, segura o ajuste por FACE_AE_HOLD_US (a
 * pessoa pode só ter virado o rosto, ou a detecção falhado uns quadros) e
 * depois volta ao slider aos poucos, sem salto de brilho.
 */

/* Próximo alvo para levar o rosto de luma a target. O sensor mede antes do
 * gamma (~0,72), então o rosto na saída anda menos que o alvo: corrigir pela
 * razão (em vez de razão^(1/0,72)) chega em 2-3 ajustes sem passar do ponto. */
static int face_ae_step(int cur, int luma, int target)
{
    float ratio = (float)target / (float)(luma > 4 ? luma : 4);
    if (ratio < FACE_AE_TOL && ratio > 1.0f / FACE_AE_TOL) {
        return cur;
    }
    int next = (int)lroundf(cur * clampf(ratio, 1.0f / FACE_AE_MAX_STEP, FACE_AE_MAX_STEP));
    if (next == cur) {
        next += ratio > 1.0f ? 1 : -1;
    }
    return next < s_ae_min ? s_ae_min : next > s_ae_max ? s_ae_max : next;
}

static void face_ae_task(void *arg)
{
    uint32_t seq = 0;
    int64_t changed_us = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(FACE_AE_POLL_MS));
        face_result_t r;
        face_get_result(&r, 4 * FACE_AE_POLL_MS);
        bool fresh = r.seq != seq && r.luma >= 0;
        seq = r.seq;
        int64_t now = esp_timer_get_time();

        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (fresh) {
            s_face_luma = r.luma;
            s_face_us = now;
        }
        if (!s_cur.face) {
            /* desligada: quem manda é o slider (apply já o devolveu ao sensor) */
        } else if (fresh) {
            if (r.frame_us >= changed_us + FACE_AE_SETTLE_US) {
                s_face_ae = face_ae_step(s_face_ae >= 0 ? s_face_ae : s_cur.ae, r.luma, s_cur.face_luma);
                if (s_face_ae != s_sensor_ae && sensor_ae(s_face_ae) == ESP_OK) {
                    changed_us = now;
                }
            }
        } else if (s_face_ae >= 0 && now - s_face_us > FACE_AE_HOLD_US && now - changed_us >= FACE_AE_BACK_US) {
            int base = s_cur.ae;
            int next = s_face_ae + (int)lroundf((base - s_face_ae) * FACE_AE_BACK_FRAC);
            if (abs(base - next) <= 1) {
                next = base;
            }
            s_face_ae = next == base ? -1 : next;
            sensor_ae(next);
            changed_us = now;
        }
        xSemaphoreGive(s_lock);
    }
}

void isp_tune_ae_status(isp_ae_status_t *out)
{
    *out = (isp_ae_status_t) { .mode = "none", .sensor_ae = -1, .face_luma = -1, .target = -1 };
    if (!s_lock || !(s_has & HAS_AE)) {
        return;
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool seen = s_face_luma >= 0 && now - s_face_us <= FACE_AE_SEEN_US;
    out->sensor_ae = s_sensor_ae;
    out->face_luma = seen ? s_face_luma : -1;
    out->target = s_cur.face_luma;
    out->mode = !s_cur.face ? "off" : s_face_ae < 0 ? "idle" : seen ? "face" :
                now - s_face_us <= FACE_AE_HOLD_US ? "hold" : "back";
    xSemaphoreGive(s_lock);
}

esp_err_t isp_tune_init(int cam_fd, const char *sensor)
{
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) {
        return ESP_ERR_NO_MEM;
    }
    s_fd = cam_fd;
    strlcpy(s_sensor, sensor, sizeof(s_sensor));
    s_orig = esp_video_isp_pipeline_enum_ipa_configs(sensor, 0);
    if (!s_orig) {
        ESP_LOGW(TAG, "sem configuração de IPA para %s: ajuste do ISP desligado", sensor);
        return ESP_ERR_NOT_FOUND;
    }
    s_bank = heap_caps_calloc(2, sizeof(bank_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_bank) {
        s_orig = NULL;
        return ESP_ERR_NO_MEM;
    }

    const esp_ipa_config_t *o = s_orig;
    tune_t *d = &s_def;
    *d = (tune_t) { .gamma = 1.0f, .ccm = 1.0f, .red = 1.0f, .blue = 1.0f };

    /* Com AGC no IPA (IMX708) a exposição é dele; na OV5647 o AE roda no
     * sensor e o que dá para mexer é o alvo de brilho dele. */
    struct v4l2_query_ext_ctrl q = { .id = V4L2_CID_EXPOSURE };
    if (!o->agc && ioctl(cam_fd, VIDIOC_QUERY_EXT_CTRL, &q) == 0) {
        s_has |= HAS_AE;
        s_ae_min = (int)q.minimum;
        s_ae_max = (int)q.maximum;
        d->ae = (int)q.default_value;
        d->face = 1;
        d->face_luma = FACE_AE_TARGET_DEF;
        s_sensor_ae = d->ae;    /* o sensor começa no padrão dele */
    }
    if (o->adn && table_ok(o->adn->bf_table, o->adn->bf_table_size)) {
        s_has |= HAS_BF;
        d->denoise = o->adn->bf_table[0].bf.level;
    }
    if (o->aen) {
        const esp_ipa_aen_gamma_config_t *g = o->aen->gamma;
        if (g && table_ok(g->gamma_table, g->gamma_table_size)) {
            s_has |= HAS_GAMMA;
            d->gamma = gamma_estimate(&g->gamma_table[0].gamma);
        }
        if (table_ok(o->aen->sharpen_table, o->aen->sharpen_table_size)) {
            const esp_ipa_sharpen_t *sh = &o->aen->sharpen_table[0].sharpen;
            s_has |= HAS_SHARPEN;
            d->sharp_m = sh->m_coeff;
            d->sharp_h = sh->h_coeff;
            d->sharp_l = sh->l_thresh;
        }
        if (table_ok(o->aen->con_table, o->aen->con_table_size)) {
            s_has |= HAS_CONTRAST;
            d->contrast = (int)o->aen->con_table[0].contrast;
        }
    }
    if (o->acc) {
        if (table_ok(o->acc->sat_table, o->acc->sat_table_size)) {
            s_has |= HAS_SAT;
            d->saturation = (int)o->acc->sat_table[0].saturation;
        }
        if (o->acc->ccm && table_ok(o->acc->ccm->ccm_table, o->acc->ccm->ccm_table_size)) {
            s_has |= HAS_CCM;
        }
    }
    s_cur = s_def;

    /* O salvo pode ser de outro firmware: o que este sensor não tem fica no
     * padrão, e o resto é trazido para dentro da faixa. */
    tune_t saved;
    if (load_saved(&saved)) {
        for (size_t i = 0; i < PARAM_COUNT; i++) {
            const param_t *p = &PARAMS[i];
            float v, lo, hi;
            param_get(s_has & p->need ? &saved : &s_def, p, &v);
            param_range(p, &lo, &hi);
            param_put(&saved, p, clampf(v, lo, hi));
        }
        saved.gamma_set = saved.gamma_set && (s_has & HAS_GAMMA);
        if (apply(&saved, &s_cur) == ESP_OK) {
            s_cur = saved;
            s_saved = true;
            ESP_LOGI(TAG, "%s: ajuste salvo aplicado", sensor);
        }
    } else {
        ESP_LOGI(TAG, "%s: usando o tuning do firmware", sensor);
    }
    if ((s_has & HAS_AE) &&
        xTaskCreate(face_ae_task, "face_ae", FACE_AE_TASK_STACK, NULL, FACE_AE_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGW(TAG, "sem memória para a task da exposição pelo rosto");
    }
    return ESP_OK;
}

esp_err_t isp_tune_set(const char *query)
{
    if (!s_orig) {
        return ESP_ERR_INVALID_STATE;
    }
    char tmp[256];
    if (strlcpy(tmp, query, sizeof(tmp)) >= sizeof(tmp)) {
        return ESP_ERR_INVALID_ARG;
    }

    tune_t next = s_cur;
    char *save = NULL;
    for (char *tok = strtok_r(tmp, "&", &save); tok; tok = strtok_r(NULL, "&", &save)) {
        char *eq = strchr(tok, '=');
        if (!eq) {
            return ESP_ERR_INVALID_ARG;
        }
        *eq = '\0';
        const param_t *p = NULL;
        for (size_t i = 0; i < PARAM_COUNT; i++) {
            if (strcmp(PARAMS[i].key, tok) == 0) {
                p = &PARAMS[i];
                break;
            }
        }
        char *end = NULL;
        float v = strtof(eq + 1, &end);
        float lo, hi;
        if (!p || !(s_has & p->need) || end == eq + 1 || *end != '\0') {
            return ESP_ERR_INVALID_ARG;
        }
        param_range(p, &lo, &hi);
        if (!(v >= lo && v <= hi)) {
            return ESP_ERR_INVALID_ARG;
        }
        param_put(&next, p, v);
        if (p->need == HAS_GAMMA) {
            next.gamma_set = true;
        }
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t err = apply(&next, &s_cur);
    if (err == ESP_OK) {
        s_saved = s_saved && same_all(&next, &s_cur);
        s_cur = next;
    }
    xSemaphoreGive(s_lock);
    return err;
}

/* ---- JSON ------------------------------------------------------------------ */

typedef struct {
    char *buf;
    size_t cap, used;
} out_t;

static void put(out_t *o, const char *fmt, ...)
{
    if (o->used >= o->cap) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->buf + o->used, o->cap - o->used, fmt, ap);
    va_end(ap);
    o->used += n > 0 ? (size_t)n : 0;
}

static void put_matrix(out_t *o, const esp_ipa_ccm_t *m)
{
    put(o, "[");
    for (int i = 0; i < ISP_CCM_DIMENSION; i++) {
        for (int j = 0; j < ISP_CCM_DIMENSION; j++) {
            put(o, "%s%.4f", i || j ? "," : "", m->matrix[i][j]);
        }
    }
    put(o, "]");
}

static void put_u8_matrix(out_t *o, const uint8_t *m, int n)
{
    put(o, "[");
    for (int i = 0; i < n; i++) {
        put(o, "%s%u", i ? "," : "", m[i]);
    }
    put(o, "]");
}

/*
 * {"sensor","saved","p":[{"k","v","def","min","max","step"}...],"x":{...}}
 * "p" só traz o que este sensor permite ajustar. "x" é o resto do que vai no
 * JSON do cfg/ (matrizes fixas, tabelas e CCM final já com força e tom).
 */
size_t isp_tune_json(char *buf, size_t cap)
{
    out_t o = { .buf = buf, .cap = cap };
    if (!s_orig) {
        put(&o, "{\"sensor\":\"%s\",\"p\":[]}", s_sensor);
        return o.used;
    }
    put(&o, "{\"sensor\":\"%s\",\"saved\":%s,\"p\":[", s_sensor, s_saved ? "true" : "false");
    bool first = true;
    for (size_t i = 0; i < PARAM_COUNT; i++) {
        const param_t *p = &PARAMS[i];
        if (!(s_has & p->need)) {
            continue;
        }
        float v, d, lo, hi;
        param_get(&s_cur, p, &v);
        param_get(&s_def, p, &d);
        param_range(p, &lo, &hi);
        put(&o, "%s{\"k\":\"%s\",\"v\":%g,\"def\":%g,\"min\":%g,\"max\":%g,\"step\":%g}",
            first ? "" : ",", p->key, v, d, lo, hi, p->step);
        first = false;
    }
    put(&o, "],\"x\":{");

    const esp_ipa_config_t *c = s_orig;
    first = true;
    if (s_has & HAS_BF) {
        put(&o, "\"bf\":{\"m\":");
        put_u8_matrix(&o, &c->adn->bf_table[0].bf.matrix[0][0], ISP_BF_TEMPLATE_X_NUMS * ISP_BF_TEMPLATE_Y_NUMS);
        put(&o, "}");
        first = false;
    }
    if (s_has & HAS_GAMMA) {
        const esp_ipa_aen_gamma_config_t *g = c->aen->gamma;
        put(&o, "%s\"gamma\":{\"env\":\"%s\",\"step\":%g,\"luma\":%g}", first ? "" : ",",
            g->luma_env ? g->luma_env : "ae.luma.avg", g->luma_min_step, g->gamma_table[0].luma);
        first = false;
    }
    if (s_has & HAS_SHARPEN) {
        const esp_ipa_sharpen_t *sh = &c->aen->sharpen_table[0].sharpen;
        put(&o, "%s\"sharpen\":{\"ht\":%u,\"m\":", first ? "" : ",", sh->h_thresh);
        put_u8_matrix(&o, &sh->matrix[0][0], ISP_SHARPEN_TEMPLATE_X_NUMS * ISP_SHARPEN_TEMPLATE_Y_NUMS);
        put(&o, "}");
        first = false;
    }
    if (s_has & HAS_CCM) {
        const esp_ipa_acc_ccm_config_t *oc = c->acc->ccm;
        esp_ipa_ccm_t m;
        put(&o, "%s\"ccm\":{\"env\":\"%s\",\"thr\":%g,\"low\":", first ? "" : ",",
            oc->luma_env ? oc->luma_env : "ae.luma.avg", oc->luma_low_threshold);
        ccm_transform(&oc->luma_low_ccm, &m, &s_cur);
        put_matrix(&o, &m);
        put(&o, ",\"t\":[");
        for (uint32_t i = 0; i < oc->ccm_table_size; i++) {
            ccm_transform(&oc->ccm_table[i].ccm, &m, &s_cur);
            put(&o, "%s{\"ct\":%" PRIu32 ",\"m\":", i ? "," : "", oc->ccm_table[i].color_temp);
            put_matrix(&o, &m);
            put(&o, "}");
        }
        put(&o, "]}");
        first = false;
    }
    if (c->ian && c->ian->luma && c->ian->luma->ae) {
        put(&o, "%s\"ae_w\":", first ? "" : ",");
        put_u8_matrix(&o, c->ian->luma->ae->weight, ISP_AE_REGIONS);
    }
    put(&o, "}}");
    return o.used;
}
