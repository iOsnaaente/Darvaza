/*
 * DisplayAO: dois olhos em GC9A01, render procedural a 30 fps.
 *
 * Modos:
 *   AUTO  olhos vivos: piscam, olham ao redor e seguem o som (mailbox do SSL)
 *   ANIM  tocando uma animação (embutida ou gravada no storage)
 *   HOLD  animação com F_HOLD terminou: congela na última pose
 * Comandos e animações só trocam de modo entre um frame e outro (o AO trata
 * eventos entre os ticks), então o render nunca vê estado pela metade.
 */
#include "display.hpp"

#include <cmath>
#include <cstring>

#include "esp_log.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "anim.hpp"
#include "app.hpp"
#include "lcd.hpp"
#include "render.hpp"

namespace display {
namespace {

const char *const TAG = "eyes";

constexpr int   TARGET_FPS   = 30;
constexpr float FOLLOW_SIGMA = 0.35f;   // só move os olhos com incerteza abaixo disso
constexpr int   GAZE_SIGN    = +1;      // olhos viram para o lado oposto? troque para -1
constexpr int   GAZE_MAX_PX  = 52;      // deslocamento máximo da íris
constexpr int   HOLD_MS      = 1800;    // continua olhando para o som depois que ele para
constexpr int   BLINK_MS     = 160;
constexpr int   DEF_BACKLIGHT = 80;
constexpr uint16_t BUILTIN_BASE = 0x8000;   // ids >= isso: animações embutidas

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline uint32_t rand_range(uint32_t lo, uint32_t hi) { return lo + esp_random() % (hi - lo + 1); }

// ---------------------------------------------------------------------------
// Animações embutidas (servem de exemplo do formato; ver anim.hpp)
// ---------------------------------------------------------------------------
template <size_t N> struct __attribute__((packed)) Clip {
    anim::Header h;
    anim::Key    k[N];
};

//                         dur   gx   gy  pupila lid_l lid_r
constexpr Clip<3> SLEEP = {{anim::MAGIC, anim::VERSION, anim::F_HOLD, 3, 0},
                           {{600,   0,  10, 20, 150, 150, 0},
                            {300,   0,  10, 20, 140, 140, 0},
                            {700,   0,  14, 18, 255, 255, 0}}};
constexpr Clip<3> STARTLE = {{anim::MAGIC, anim::VERSION, 0, 3, 0},
                             {{60,    0,  -6, 12,   0,   0, 0},
                              {1200,  0,  -6, 13,   0,   0, 0},
                              {400,   0,   0, 26,   0,   0, 0}}};
constexpr Clip<3> WINK = {{anim::MAGIC, anim::VERSION, 0, 3, 0},
                          {{100,   8,   0, 26,   0, 255, 0},
                           {200,   8,   0, 26,   0, 255, 0},
                           {120,   0,   0, 26,   0,   0, 0}}};
constexpr Clip<4> SEARCH = {{anim::MAGIC, anim::VERSION, anim::F_LOOP, 4, 0},
                            {{400, -50,   0, 24,   0,   0, 0},
                             {600, -50,   0, 24,   0,   0, 0},
                             {500,  50,   0, 24,   0,   0, 0},
                             {600,  50,   0, 24,   0,   0, 0}}};

struct Builtin {
    uint16_t       id;
    const uint8_t *data;
    size_t         size;
};
template <size_t N> Builtin builtin(uint16_t id, const Clip<N> &c)
{
    return {id, reinterpret_cast<const uint8_t *>(&c), sizeof(c)};
}
const Builtin BUILTINS[] = {
    builtin(0x8001, SLEEP),     // dormir (fica fechado)
    builtin(0x8002, STARTLE),   // susto
    builtin(0x8003, WINK),      // piscadela (olho direito)
    builtin(0x8004, SEARCH),    // procurar (loop)
};

// ---------------------------------------------------------------------------
// Diagnóstico: onde a task dos olhos está. Se ela travar, o watchdog chama o
// gancho abaixo e o log mostra a última etapa em que ela entrou.
// ---------------------------------------------------------------------------
enum Stage : uint8_t { ST_LOGIC, ST_WAIT_BUF, ST_RENDER, ST_DRAW, ST_LOG, ST_IDLE };
const char *const STAGE_NAMES[] = {"lógica", "espera buffer", "render", "draw_bitmap (driver SPI)", "log",
                                   "esperando evento/frame"};
volatile uint8_t  s_stage = ST_LOGIC, s_stage_eye = 0, s_stage_band = 0;
volatile uint32_t s_frames_since_log = 0;
volatile int64_t  s_stage_us = 0;

inline void stage(Stage st, int eye = 0, int band = 0)
{
    s_stage      = st;
    s_stage_eye  = static_cast<uint8_t>(eye);
    s_stage_band = static_cast<uint8_t>(band);
    s_stage_us   = esp_timer_get_time();
}

// ---------------------------------------------------------------------------
class DisplayAO : public ao::ActiveObject {
public:
    explicit DisplayAO(BaseType_t core) : ActiveObject("display", 8)
    {
        backlight_init();
        lcd_init_all(eyes_, core);
        const float bl = app::cfg_get(proto::CFG_BACKLIGHT, DEF_BACKLIGHT);
        backlight_ = (bl >= 0.0f && bl <= 100.0f) ? static_cast<int>(bl) : DEF_BACKLIGHT;
    }

protected:
    void on_start() override
    {
        const int64_t now_ms = esp_timer_get_time() / 1000;
        next_saccade_ = now_ms + 1000;
        next_blink_   = now_ms + 3000;
        t_log_        = now_ms;
        next_frame_   = xTaskGetTickCount();
    }

    TickType_t next_tick() override
    {
        const int32_t d = static_cast<int32_t>(next_frame_ - xTaskGetTickCount());
        return d > 0 ? static_cast<TickType_t>(d) : 0;
    }

    void on_tick() override
    {
        frame();
        // Atrasou mais de um frame (flash sendo gravada, por ex.)? Pula em vez de correr atrás
        const TickType_t period = pdMS_TO_TICKS(1000 / TARGET_FPS);
        const TickType_t now    = xTaskGetTickCount();
        next_frame_ += period;
        if (static_cast<int32_t>(now - next_frame_) > static_cast<int32_t>(period)) next_frame_ = now + period;
        stage(ST_IDLE);
    }

    void on_event(ao::Event &e) override
    {
        switch (e.sig) {
        case app::SIG_ANIM_PLAY:   play(e); break;
        case app::SIG_ANIM_LOADED: loaded(e); break;
        case app::SIG_BACKLIGHT: {
            const int pct = e.get<proto::Backlight>().percent;
            if (pct > 100) {
                app::reply(e, proto::ST_BAD_ARG);
                break;
            }
            backlight_ = pct;
            if (lit_) backlight_set(backlight_);
            app::save_cfg(proto::CFG_BACKLIGHT, static_cast<float>(pct));
            app::reply(e, proto::ST_OK);
            break;
        }
        default:
            ESP_LOGW(TAG, "sinal inesperado %u", e.sig);
            break;
        }
    }

private:
    // ---- comandos -------------------------------------------------------
    void play(const ao::Event &e)
    {
        const uint16_t id = e.get<proto::AnimId>().id;
        if (id == 0) {   // volta ao automático
            stop_anim();
            return app::reply(e, proto::ST_OK);
        }
        if (id >= BUILTIN_BASE) {
            for (const auto &b : BUILTINS)
                if (b.id == id) return app::reply(e, start_anim(id, b.data, b.size, nullptr));
            return app::reply(e, proto::ST_NOT_FOUND);
        }
        // Gravada: pede ao storage; a resposta ao host sai quando ela chegar
        ao::Event req{};
        req.sig = app::SIG_ANIM_LOAD;
        req.seq = e.seq;
        req.op  = e.op;
        req.put(proto::AnimId{id});
        if (!app::storage->post(req)) app::reply(e, proto::ST_BUSY);
    }

    // Resposta do storage: data[0] = Status, data[1..2] = id, blob = animação
    void loaded(ao::Event &e)
    {
        const auto st = static_cast<proto::Status>(e.data[0]);
        uint16_t   id;
        memcpy(&id, e.data + 1, sizeof(id));
        if (st != proto::ST_OK || !e.blk) return app::reply(e, st != proto::ST_OK ? st : proto::ST_STORAGE);
        ao::Blob *b = e.blk;
        e.blk       = nullptr;   // fica com o blob enquanto a animação tocar
        app::reply(e, start_anim(id, b->data(), b->len, b));
    }

    proto::Status start_anim(uint16_t id, const uint8_t *p, size_t n, ao::Blob *owner)
    {
        if (const char *why = anim::validate(p, n)) {
            ESP_LOGW(TAG, "animação %u inválida: %s", id, why);
            ao::blob_free(owner);
            return proto::ST_BAD_ARG;
        }
        player_.start(p, pose_, now32());
        ao::blob_free(anim_blob_);   // a anterior, depois de o player largar dela
        anim_blob_ = owner;
        anim_id_   = id;
        mode_      = Mode::Anim;
        ESP_LOGI(TAG, "animação %u", id);
        return proto::ST_OK;
    }

    void stop_anim()
    {
        mode_ = Mode::Auto;
        ao::blob_free(anim_blob_);
        anim_blob_ = nullptr;
    }

    // ---- frame ----------------------------------------------------------
    void frame()
    {
        stage(ST_LOGIC);
        s_frames_since_log = s_frames_since_log + 1;
        const int64_t now_ms = esp_timer_get_time() / 1000;
        app::SslState ssl;
        app::ssl_read(ssl);   // ainda sem dados: valores padrão (silêncio)
        const bool heard = ssl.sound_ms >= 0 && now_ms - ssl.sound_ms < HOLD_MS;

        switch (mode_) {
        case Mode::Auto:
            auto_pose(now_ms, ssl, heard);
            break;
        case Mode::Anim:
            if (!player_.pose(now32(), pose_)) {
                const proto::AnimId done{anim_id_};
                app::notify(proto::EVT_ANIM_DONE, &done, sizeof(done));
                if (player_.hold()) {
                    mode_ = Mode::Hold;
                } else {
                    stop_anim();
                    attending_ = false;
                }
            }
            break;
        case Mode::Hold:
            break;
        }

        EyeState st[2];
        for (int e = 0; e < 2; ++e) {
            const float lid = clampf(pose_.lid[e], 0.0f, 1.0f);
            st[e].gx        = static_cast<int>(pose_.gx);
            st[e].gy        = static_cast<int>(pose_.gy);
            st[e].pupil_r   = static_cast<int>(clampf(pose_.pupil, 4.0f, static_cast<float>(st[e].iris_r - 8)));
            st[e].lid_top   = 20 + static_cast<int>(100.0f * lid);
            st[e].lid_bot   = 220 - static_cast<int>(100.0f * lid);
        }
        draw(st);

        // Acende só depois do primeiro frame inteiro (esconde o lixo da GRAM)
        if (!lit_ && ++frames_total_ >= 2) {
            backlight_set(backlight_);
            lit_ = true;
        }

        ++frames_;
        if (now_ms - t_log_ >= 1000) {
            stage(ST_LOG);
            log(ssl, heard);
            s_frames_since_log = 0;
            frames_ = 0;
            t_log_  = now_ms;
        }
    }

    // Olhar: som acima do fundo -> ângulo estimado (e segura HOLD_MS depois que
    // ele para); silêncio -> sacadas aleatórias.
    void auto_pose(int64_t now_ms, const app::SslState &ssl, bool heard)
    {
        const float loud = fmaxf(ssl.level_l, ssl.level_r);
        if (heard) {
            if (!attending_) blink_start_ = now_ms;   // pisca ao se virar, como quem é chamado
            // Segue o ângulo estimado de forma contínua; incerto = fica onde está
            if (ssl.sigma < FOLLOW_SIGMA) tgx_ = GAZE_SIGN * ssl.s * GAZE_MAX_PX;
            tgy_ = 0;
            next_saccade_ = now_ms + rand_range(600, 1500);
        } else if (now_ms >= next_saccade_) {
            tgx_ = static_cast<float>(static_cast<int>(rand_range(0, 100)) - 50);
            tgy_ = static_cast<float>(static_cast<int>(rand_range(0, 60)) - 30);
            next_saccade_ = now_ms + rand_range(800, 2500);
        }
        attending_ = heard;
        // Virada para o som é mais rápida que o vagar em repouso
        const float k = heard ? 0.5f : 0.35f;
        pose_.gx += (tgx_ - pose_.gx) * k;
        pose_.gy += (tgy_ - pose_.gy) * k;

        // Pupila dilata com o volume acima do fundo
        const float target_pupil = 22.0f + clampf((loud - ssl.floor) / 0.03f, 0.0f, 1.0f) * 16.0f;
        pose_.pupil += (target_pupil - pose_.pupil) * 0.2f;

        // Piscar
        float lid = 0.0f;   // 0 = aberto, 1 = fechado
        if (blink_start_ < 0 && now_ms >= next_blink_) blink_start_ = now_ms;
        if (blink_start_ >= 0) {
            const float p = static_cast<float>(now_ms - blink_start_) / BLINK_MS;
            if (p >= 1.0f) {
                blink_start_ = -1;
                next_blink_  = now_ms + rand_range(2000, 6000);
            } else {
                lid = p < 0.5f ? p * 2.0f : (1.0f - p) * 2.0f;
            }
        }
        pose_.lid[0] = pose_.lid[1] = lid;
    }

    // Envio intercalado por faixa: SPI2 e SPI3 transmitem em paralelo
    // enquanto a CPU renderiza a próxima faixa.
    void draw(const EyeState (&st)[2])
    {
        for (int band = 0; band < N_BANDS; ++band) {
            const int y0 = band * BAND_LINES;
            for (int e = 0; e < 2; ++e) {
                Eye &eye = eyes_[e];
                // Nunca espera para sempre: se o callback de fim de DMA se perder, o
                // buffer não voltaria e a task travaria em silêncio. Uma faixa leva
                // ~4 ms a 40 MHz; após 100 ms o DMA com certeza já terminou.
                stage(ST_WAIT_BUF, e, band);
                if (xSemaphoreTake(eye.free_bufs, pdMS_TO_TICKS(100)) != pdTRUE) {
                    ++lcd_timeouts_[e];
                    ESP_LOGE(TAG, "LCD%d: fim de transferência não chegou (%lu vezes)", e + 1,
                             static_cast<unsigned long>(lcd_timeouts_[e]));
                }
                uint16_t *b = eye.buf[eye.next];
                eye.next ^= 1;
                stage(ST_RENDER, e, band);
                render_band(b, y0, st[e]);
                stage(ST_DRAW, e, band);
                const esp_err_t err = esp_lcd_panel_draw_bitmap(eye.panel, 0, y0, LCD_W, y0 + BAND_LINES, b);
                if (err != ESP_OK) {
                    // Nada foi enfileirado -> o callback não virá; devolve o buffer
                    xSemaphoreGive(eye.free_bufs);
                    ++lcd_errors_[e];
                    ESP_LOGE(TAG, "LCD%d: draw_bitmap falhou: %s (%lu vezes)", e + 1, esp_err_to_name(err),
                             static_cast<unsigned long>(lcd_errors_[e]));
                }
            }
        }
    }

    void log(const app::SslState &ssl, bool heard)
    {
        static const char *const MODES[] = {"auto", "anim", "hold"};
        const float L = ssl.level_l, R = ssl.level_r, fl = ssl.floor;
        ESP_LOGI(TAG, "fps=%d %s  L=%.1f  R=%.1f  fundo=%.1f dBFS  pol=%+.2f  atraso=%+.2f amostras  coer=%.2f  ângulo=%+.0f° ±%.2f%s",
                 frames_, MODES[static_cast<int>(mode_)], 20.0f * log10f(L + 1e-9f), 20.0f * log10f(R + 1e-9f),
                 20.0f * log10f(fl + 1e-9f), ssl.polarity, ssl.tdoa, ssl.coherence,
                 asinf(clampf(ssl.s, -1.0f, 1.0f)) * 57.2958f, ssl.sigma, heard ? "  <- som" : "");
        ESP_LOGI(TAG, "heap livre=%lu (mín %lu)  pilha livre: display=%u audio=%u  erros LCD1=%lu/%lu LCD2=%lu/%lu",
                 static_cast<unsigned long>(esp_get_free_heap_size()),
                 static_cast<unsigned long>(esp_get_minimum_free_heap_size()),
                 uxTaskGetStackHighWaterMark(nullptr),
                 app::audio->task() ? uxTaskGetStackHighWaterMark(app::audio->task()) : 0,
                 static_cast<unsigned long>(lcd_timeouts_[0]), static_cast<unsigned long>(lcd_errors_[0]),
                 static_cast<unsigned long>(lcd_timeouts_[1]), static_cast<unsigned long>(lcd_errors_[1]));
    }

    static uint32_t now32() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

    enum class Mode : uint8_t { Auto, Anim, Hold };

    Eye         eyes_[2];
    Mode        mode_ = Mode::Auto;
    anim::Pose  pose_;                   // pose atual dos dois olhos
    anim::Player player_;
    ao::Blob   *anim_blob_ = nullptr;    // animação gravada tocando (nullptr = embutida)
    uint16_t    anim_id_   = 0;

    // Modo automático
    float   tgx_ = 0, tgy_ = 0;
    int64_t next_saccade_ = 0, next_blink_ = 0, blink_start_ = -1;
    bool    attending_ = false;   // olhando para um som

    int        backlight_    = DEF_BACKLIGHT;
    bool       lit_          = false;
    uint32_t   frames_total_ = 0;
    int        frames_       = 0;
    int64_t    t_log_        = 0;
    TickType_t next_frame_   = 0;
    uint32_t   lcd_timeouts_[2] = {0, 0}, lcd_errors_[2] = {0, 0};
};

}  // namespace

ao::ActiveObject *create(BaseType_t core)
{
    static DisplayAO self(core);
    return &self;
}

}  // namespace display

// Watchdog: se a task dos olhos parar de rodar por 5 s, o log diz qual task e onde
extern "C" void esp_task_wdt_isr_user_handler(void)
{
    using namespace display;
    esp_rom_printf("\n*** display parada em: %s  LCD%d faixa %d  há %lld ms  quadros desde o log: %lu\n",
                   STAGE_NAMES[s_stage], s_stage_eye + 1, s_stage_band,
                   (esp_timer_get_time() - s_stage_us) / 1000, static_cast<unsigned long>(s_frames_since_log));
}
