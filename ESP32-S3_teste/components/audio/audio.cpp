/*
 * AudioAO: 2x INMP441 em I2S estéreo a 16 kHz / 32 bit.
 *
 * O "relógio" deste AO é o I2S (um bloco de 16 ms por leitura), não a fila:
 * os comandos são atendidos entre um bloco e outro (drain), sem nunca bloquear.
 * Saídas: estado do SSL no mailbox (a cada bloco), EVT_SOUND para o host e,
 * com o stream ligado, o PCM pelo buffer de áudio do comm.
 */
#include "audio.hpp"

#include <cmath>
#include <cstdint>

#include "driver/i2s_std.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

#include "app.hpp"
#include "board.hpp"
#include "comm.hpp"
#include "ssl.hpp"

namespace audio {
namespace {

const char *const TAG = "audio";

constexpr bool    STREAM_AT_BOOT = true;   // tools/ouvir_mics.py espera o áudio desde o boot
constexpr int64_t EVT_PERIOD_MS  = 100;    // EVT_SOUND: no começo do som e a cada 100 ms

// Padrões dos parâmetros (valem até o host mandar outro via OP_AUDIO_CFG)
// Um dos INMP441 sai com polaridade invertida em relação ao outro (medido: correlação
// -0.6 a -0.9 abaixo de 300 Hz, onde os dois mics a 15 cm recebem a MESMA pressão). Com isso o
// pico do GCC vira vale e o atraso sai aleatório. Confira 'pol' no log: tem que ser > 0.
constexpr float DEF_MIC_POLARITY = -1.0f;   // +1 se trocar o módulo e 'pol' ficar negativo
constexpr float DEF_STREAM_SHIFT = 7.0f;    // 24 bit -> 16 bit com +6 dB (8 = sem ganho; 5 = +18 dB saturava palmas)

i2s_chan_handle_t mic_init()
{
    i2s_chan_handle_t rx = nullptr;
    i2s_chan_config_t chan = {};
    chan.id            = I2S_NUM_0;
    chan.role          = I2S_ROLE_MASTER;
    chan.dma_desc_num  = 6;
    chan.dma_frame_num = 240;   // 6 x 240 = 90 ms de folga a 16 kHz
    ESP_ERROR_CHECK(i2s_new_channel(&chan, nullptr, &rx));

    i2s_std_config_t cfg = {};
    cfg.clk_cfg.sample_rate_hz = ssl::SAMPLE_RATE;
    cfg.clk_cfg.clk_src        = I2S_CLK_SRC_DEFAULT;
    cfg.clk_cfg.mclk_multiple  = I2S_MCLK_MULTIPLE_256;

    // Philips I2S, slot de 32 bits (INMP441: 24 bits alinhados à esquerda)
    cfg.slot_cfg.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT;
    cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO;
    cfg.slot_cfg.slot_mode      = I2S_SLOT_MODE_STEREO;
    cfg.slot_cfg.slot_mask      = I2S_STD_SLOT_BOTH;
    cfg.slot_cfg.ws_width       = 32;
    cfg.slot_cfg.ws_pol         = false;
    cfg.slot_cfg.bit_shift      = true;
    cfg.slot_cfg.left_align     = true;

    cfg.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.bclk = pin::I2S_BCLK;
    cfg.gpio_cfg.ws   = pin::I2S_WS;
    cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
    cfg.gpio_cfg.din  = pin::I2S_DIN;

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx, &cfg));
    gpio_pulldown_en(pin::I2S_DIN);   // SD não flutua entre os slots (complementa o 100k externo)
    ESP_ERROR_CHECK(i2s_channel_enable(rx));
    ESP_LOGI(TAG, "I2S estéreo a %lu Hz", static_cast<unsigned long>(ssl::SAMPLE_RATE));
    return rx;
}

class AudioAO : public ao::ActiveObject {
public:
    explicit AudioAO(i2s_chan_handle_t rx) : ActiveObject("audio", 8), rx_(rx)
    {
        // Parâmetros gravados; valor inválido na NVS fica no padrão
        apply(proto::CFG_TRIGGER_DB,   app::cfg_get(proto::CFG_TRIGGER_DB, tracker_.cfg.trigger_db));
        apply(proto::CFG_MIC_POLARITY, app::cfg_get(proto::CFG_MIC_POLARITY, DEF_MIC_POLARITY));
        apply(proto::CFG_TDOA_TRIM,    app::cfg_get(proto::CFG_TDOA_TRIM, tracker_.cfg.tdoa_trim));
        apply(proto::CFG_STREAM_SHIFT, app::cfg_get(proto::CFG_STREAM_SHIFT, DEF_STREAM_SHIFT));
    }

protected:
    void run() override
    {
        ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
        while (true) {
            size_t n = 0;
            const esp_err_t err = i2s_channel_read(rx_, buf_, sizeof(buf_), &n, pdMS_TO_TICKS(200));
            esp_task_wdt_reset();
            drain();   // comandos entre um bloco e outro
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "I2S: leitura falhou: %s", esp_err_to_name(err));
                continue;
            }
            const int frames = static_cast<int>(n / (2 * sizeof(int32_t)));
            if (frames == 0) continue;
            if (polarity_ < 0)
                for (int i = 0; i < frames; ++i)   // amostra 24 bit alinhada à esquerda: -x não estoura
                    buf_[2 * i + 1] = buf_[2 * i + 1] == INT32_MIN ? INT32_MAX : -buf_[2 * i + 1];
            if (streaming_) stream(frames);
            publish(tracker_.process(buf_, frames));
        }
    }

    void on_event(ao::Event &e) override
    {
        switch (e.sig) {
        case app::SIG_STREAM:
            streaming_ = e.get<proto::Stream>().on != 0;
            app::reply(e, proto::ST_OK);
            break;
        case app::SIG_AUDIO_CFG: {
            const auto c = e.get<proto::Cfg>();
            const auto key = static_cast<proto::CfgKey>(c.key);
            if (!apply(key, c.value)) {
                app::reply(e, proto::ST_BAD_ARG);
                break;
            }
            app::save_cfg(key, c.value);
            app::reply(e, proto::ST_OK);
            break;
        }
        default:
            ESP_LOGW(TAG, "sinal inesperado %u", e.sig);
            break;
        }
    }

private:
    // Valida e aplica um parâmetro; false = chave desconhecida ou fora da faixa
    bool apply(proto::CfgKey key, float v)
    {
        switch (key) {
        case proto::CFG_TRIGGER_DB:
            if (!(v >= 0.0f && v <= 40.0f)) return false;
            tracker_.cfg.trigger_db = v;
            return true;
        case proto::CFG_MIC_POLARITY:
            if (v != 1.0f && v != -1.0f) return false;
            polarity_ = static_cast<int>(v);
            return true;
        case proto::CFG_TDOA_TRIM:
            if (!(v >= -8.0f && v <= 8.0f)) return false;
            tracker_.cfg.tdoa_trim = v;
            return true;
        case proto::CFG_STREAM_SHIFT:
            if (!(v >= 0.0f && v <= 8.0f) || v != floorf(v)) return false;
            shift_ = static_cast<int>(v);
            return true;
        default:
            return false;
        }
    }

    void stream(int frames)
    {
        for (int i = 0; i < frames * 2; ++i) {
            int32_t v = (buf_[i] >> 8) >> shift_;
            v = v > INT16_MAX ? INT16_MAX : (v < INT16_MIN ? INT16_MIN : v);
            pcm_[i] = static_cast<int16_t>(v);
        }
        comm::audio_push(seq_++, pcm_, frames);   // cheio: bloco perdido, o seq pula
    }

    void publish(const ssl::Output &o)
    {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        if (o.active) sound_ms_ = now_ms;

        app::SslState st;
        st.level_l   = o.level_l;
        st.level_r   = o.level_r;
        st.floor     = o.floor;
        st.s         = o.s;
        st.sigma     = o.sigma;
        st.tdoa      = o.tdoa;
        st.coherence = o.coherence;
        st.polarity  = o.polarity;
        st.sound_ms  = sound_ms_;
        app::ssl_publish(st);

        if (o.active && (!was_active_ || now_ms - last_evt_ms_ >= EVT_PERIOD_MS)) {
            const proto::EvtSound ev{o.s, o.sigma, o.coherence, 20.0f * log10f(fmaxf(o.level_l, o.level_r) + 1e-9f)};
            app::notify(proto::EVT_SOUND, &ev, sizeof(ev));
            last_evt_ms_ = now_ms;
        }
        was_active_ = o.active;
    }

    i2s_chan_handle_t rx_;
    ssl::Tracker      tracker_;
    int32_t           buf_[ssl::BLOCK * 2];   // intercalado L, R
    int16_t           pcm_[ssl::BLOCK * 2];
    bool              streaming_   = STREAM_AT_BOOT;
    int               polarity_    = static_cast<int>(DEF_MIC_POLARITY);
    int               shift_       = static_cast<int>(DEF_STREAM_SHIFT);
    uint16_t          seq_         = 0;
    int64_t           sound_ms_    = -1;
    int64_t           last_evt_ms_ = 0;
    bool              was_active_  = false;
};

}  // namespace

ao::ActiveObject *create()
{
    static AudioAO self(mic_init());
    return &self;
}

}  // namespace audio
