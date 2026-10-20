/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microfone (ver audio.h). Pinos do exemplo 12_I2SCodec da Waveshare para o
 * P4 (os mesmos da ESP32-P4-Function-EV-Board): MCLK 13, BCLK 12, WS 10,
 * DOUT 9 (para o codec), DIN 11 (do codec). O amplificador (NS4150B, GPIO 53)
 * só importa para tocar som.
 */
#include <math.h>
#include <stdatomic.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "audio.h"

static const char *TAG = "audio";

#define AUDIO_I2S_PORT      I2S_NUM_0
#define AUDIO_MCLK_PIN      13
#define AUDIO_BCLK_PIN      12
#define AUDIO_WS_PIN        10
#define AUDIO_DIN_PIN       11      /* ASDOUT do ES8311 -> P4 */
#define AUDIO_RATE_HZ       16000
#define AUDIO_FRAME         512     /* amostras por canal = 32 ms */
#define AUDIO_CHANNELS      2
#define MIC_SLOT            0       /* esquerda; a direita é a referência do DAC */

#define MIC_GAIN_DEFAULT_DB 30
#define LEVEL_FLOOR_DB      (-90.0f)
#define PEAK_HOLD_US        (1000 * 1000)
#define PEAK_FALL_DB_PER_S  20.0f

#define AUDIO_TASK_STACK    4096
#define AUDIO_TASK_PRIO     7       /* acima do httpd: o I2S não pode transbordar */

static esp_codec_dev_handle_t s_dev;
static atomic_bool s_ok;
static atomic_int s_gain = -1;
/* Níveis em centésimos de dB: inteiros, para a página ler sem lock. */
static atomic_int s_rms_cdb, s_peak_cdb, s_ref_cdb;

/* Gravação: quem chama preenche destino e tamanho e liga s_rec_on; daí em
 * diante só a task de áudio mexe neles, até desligar s_rec_on e dar
 * s_rec_done. Cancelar (timeout) também é ela quem conclui. */
static int16_t *s_rec_dst;
static size_t s_rec_n;
static atomic_size_t s_rec_got;
static atomic_bool s_rec_on, s_rec_cancel;
static SemaphoreHandle_t s_rec_done;

static float to_db(float v)
{
    float db = v > 0 ? 20.0f * log10f(v / 32768.0f) : LEVEL_FLOOR_DB;
    return db < LEVEL_FLOOR_DB ? LEVEL_FLOOR_DB : db;
}

/* RMS sem a componente DC (o ADC pode ter offset) e pico, de um canal. */
static void measure(const int16_t *pcm, int slot, float *rms, float *peak)
{
    int64_t sum = 0, sum2 = 0;
    int max = 0;
    for (int i = 0; i < AUDIO_FRAME; i++) {
        int v = pcm[i * AUDIO_CHANNELS + slot];
        sum += v;
        sum2 += (int64_t)v * v;
        int a = v < 0 ? -v : v;
        if (a > max) {
            max = a;
        }
    }
    double mean = (double)sum / AUDIO_FRAME;
    double var = (double)sum2 / AUDIO_FRAME - mean * mean;
    *rms = var > 0 ? (float)sqrt(var) : 0;
    *peak = (float)max;
}

static void audio_task(void *arg)
{
    const int bytes = AUDIO_FRAME * AUDIO_CHANNELS * sizeof(int16_t);
    int16_t *pcm = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!pcm) {
        ESP_LOGE(TAG, "sem memória para o buffer do microfone");
        atomic_store(&s_ok, false);
        vTaskDelete(NULL);
    }
    float held = LEVEL_FLOOR_DB;
    int64_t held_us = 0, last_us = esp_timer_get_time();

    while (1) {
        if (esp_codec_dev_read(s_dev, pcm, bytes) != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "leitura do I2S falhou");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        float rms, peak, ref_rms, ref_peak;
        measure(pcm, MIC_SLOT, &rms, &peak);
        measure(pcm, 1 - MIC_SLOT, &ref_rms, &ref_peak);

        /* Pico segurado: o maior do último segundo, depois cai devagar. */
        int64_t now = esp_timer_get_time();
        float peak_db = to_db(peak);
        if (peak_db >= held) {
            held = peak_db;
            held_us = now;
        } else if (now - held_us > PEAK_HOLD_US) {
            held -= PEAK_FALL_DB_PER_S * (float)(now - last_us) / 1e6f;
            if (held < peak_db) {
                held = peak_db;
            }
        }
        last_us = now;

        atomic_store(&s_rms_cdb, (int)lroundf(to_db(rms) * 100));
        atomic_store(&s_peak_cdb, (int)lroundf(held * 100));
        atomic_store(&s_ref_cdb, (int)lroundf(to_db(ref_rms) * 100));

        if (atomic_load(&s_rec_on)) {
            size_t got = atomic_load(&s_rec_got);
            if (!atomic_load(&s_rec_cancel)) {
                for (int i = 0; i < AUDIO_FRAME && got < s_rec_n; i++) {
                    s_rec_dst[got++] = pcm[i * AUDIO_CHANNELS + MIC_SLOT];
                }
                atomic_store(&s_rec_got, got);
            }
            if (got >= s_rec_n || atomic_load(&s_rec_cancel)) {
                atomic_store(&s_rec_on, false);
                xSemaphoreGive(s_rec_done);
            }
        }
    }
}

esp_err_t audio_record(int16_t *dst, size_t n, uint32_t timeout_ms)
{
    if (!atomic_load(&s_ok) || !dst || n == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load(&s_rec_on)) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_rec_done, 0);      /* sobra de uma gravação cancelada */
    s_rec_dst = dst;
    s_rec_n = n;
    atomic_store(&s_rec_got, 0);
    atomic_store(&s_rec_cancel, false);
    atomic_store(&s_rec_on, true);
    if (xSemaphoreTake(s_rec_done, pdMS_TO_TICKS(timeout_ms)) == pdTRUE) {
        return ESP_OK;
    }
    /* Para a task de áudio de escrever em dst antes de devolver: ela conclui
     * no próximo quadro (32 ms). */
    atomic_store(&s_rec_cancel, true);
    xSemaphoreTake(s_rec_done, pdMS_TO_TICKS(500));
    return ESP_ERR_TIMEOUT;
}

size_t audio_record_progress(void)
{
    return atomic_load(&s_rec_on) ? atomic_load(&s_rec_got) : 0;
}

esp_err_t audio_start(i2c_master_bus_handle_t bus)
{
    if (!bus) {
        return ESP_ERR_INVALID_ARG;
    }

    /* O ES8311 é escravo: o I2S gera MCLK/BCLK/WS, e o MCLK precisa estar
     * rodando antes de configurar o codec. Só RX: o alto-falante fica livre. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_frame_num = AUDIO_FRAME;   /* um buffer de DMA = um quadro */
    chan_cfg.dma_desc_num = 4;              /* ~128 ms de folga */
    i2s_chan_handle_t rx = NULL;
    esp_err_t err = i2s_new_channel(&chan_cfg, NULL, &rx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err));
        return err;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = AUDIO_MCLK_PIN,
            .bclk = AUDIO_BCLK_PIN,
            .ws = AUDIO_WS_PIN,
            .dout = I2S_GPIO_UNUSED,
            .din = AUDIO_DIN_PIN,
        },
    };
    if ((err = i2s_channel_init_std_mode(rx, &std_cfg)) != ESP_OK || (err = i2s_channel_enable(rx)) != ESP_OK) {
        ESP_LOGE(TAG, "I2S RX: %s", esp_err_to_name(err));
        i2s_del_channel(rx);
        return err;
    }

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = 0,
        .addr = ES8311_CODEC_DEFAULT_ADDR,  /* 0x30 = 0x18 << 1: o esp_codec_dev usa o endereço de 8 bits */
        .bus_handle = bus,
    };
    audio_codec_i2s_cfg_t i2s_cfg = { .port = AUDIO_I2S_PORT, .rx_handle = rx };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (!ctrl_if || !gpio_if || !data_if) {
        ESP_LOGE(TAG, "esp_codec_dev: falha criando as interfaces");
        return ESP_FAIL;
    }
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = -1,
        .use_mclk = true,
        .no_dac_ref = false,    /* direita = referência do DAC (para o AEC no futuro) */
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    if (!codec_if) {
        ESP_LOGE(TAG, "ES8311 não respondeu no I2C (0x18)");
        return ESP_ERR_NOT_FOUND;
    }
    esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN,
        .codec_if = codec_if,
        .data_if = data_if,
    };
    s_dev = esp_codec_dev_new(&dev_cfg);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = AUDIO_CHANNELS,
        .sample_rate = AUDIO_RATE_HZ,
    };
    if (!s_dev || esp_codec_dev_open(s_dev, &fs) != ESP_CODEC_DEV_OK) {
        ESP_LOGE(TAG, "não consegui abrir o ES8311");
        return ESP_FAIL;
    }

    s_rec_done = xSemaphoreCreateBinary();
    if (!s_rec_done) {
        return ESP_ERR_NO_MEM;
    }
    atomic_store(&s_rms_cdb, (int)(LEVEL_FLOOR_DB * 100));
    atomic_store(&s_peak_cdb, (int)(LEVEL_FLOOR_DB * 100));
    atomic_store(&s_ref_cdb, (int)(LEVEL_FLOOR_DB * 100));
    atomic_store(&s_ok, true);
    audio_set_gain(MIC_GAIN_DEFAULT_DB);

    /* Core 0: o core 1 é da detecção de rostos. */
    if (xTaskCreatePinnedToCore(audio_task, "audio", AUDIO_TASK_STACK, NULL, AUDIO_TASK_PRIO, NULL, 0) != pdPASS) {
        atomic_store(&s_ok, false);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "microfone: ES8311, %d Hz, ganho %d dB", AUDIO_RATE_HZ, atomic_load(&s_gain));
    return ESP_OK;
}

bool audio_ok(void)
{
    return atomic_load(&s_ok);
}

void audio_get_level(audio_level_t *out)
{
    out->rms_db = atomic_load(&s_rms_cdb) / 100.0f;
    out->peak_db = atomic_load(&s_peak_cdb) / 100.0f;
    out->ref_db = atomic_load(&s_ref_cdb) / 100.0f;
}

esp_err_t audio_set_gain(int db)
{
    if (!atomic_load(&s_ok)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (db < 0 || db > 42) {
        return ESP_ERR_INVALID_ARG;
    }
    db = db / 6 * 6;    /* o ES8311 só tem degraus de 6 dB */
    if (esp_codec_dev_set_in_gain(s_dev, (float)db) != ESP_CODEC_DEV_OK) {
        return ESP_FAIL;
    }
    atomic_store(&s_gain, db);
    return ESP_OK;
}

int audio_gain(void)
{
    return atomic_load(&s_ok) ? atomic_load(&s_gain) : -1;
}
