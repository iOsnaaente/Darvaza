/*
 * Comunicação com o host pela UART.
 *
 *  - comm_rx (task): lê a UART, monta os frames e ROTEIA cada comando pela
 *    tabela ROUTES para o AO dono (ou responde erro de transporte).
 *  - CommAO: único que escreve na UART. Respostas, eventos e log saem na ordem
 *    em que chegam; o áudio só quando a fila está vazia (passa por último).
 *  - Hook do ESP_LOG: o texto vai para um buffer e sai em frames LOG.
 */
#include "comm.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "freertos/FreeRTOS.h"
#include "freertos/message_buffer.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"

#include "driver/uart_vfs.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "app.hpp"

namespace comm {
namespace {

const char *const TAG = "comm";

constexpr int    UART_RX_BUF     = 4096;
constexpr int    UART_TX_BUF     = 16 * 1024;   // ~80 ms a 2 Mbaud: limita a espera de uma resposta atrás do áudio
constexpr int    UART_EVT_QUEUE  = 16;
constexpr size_t AUDIO_BLOCK_MAX = 2 + 256 * 4;   // seq + 256 quadros (L,R) i16
constexpr size_t AUDIO_BUF       = 8 * 1024;      // ~7 blocos de 16 ms
constexpr size_t LOG_BUF         = 4096;
constexpr size_t LOG_LINE        = 256;
constexpr size_t LOG_CHUNK       = 240;           // texto por frame LOG

Config                s_cfg;
QueueHandle_t         s_uart_q  = nullptr;
MessageBufferHandle_t s_audio   = nullptr;
StreamBufferHandle_t  s_log     = nullptr;
SemaphoreHandle_t     s_log_mtx = nullptr;
ao::ActiveObject     *s_self    = nullptr;

// Campainhas: só um SIG_*_READY por vez na fila do comm
std::atomic<bool>     s_audio_bell{false};
std::atomic<bool>     s_log_bell{false};

std::atomic<uint32_t> s_rx_frames{0};
std::atomic<uint32_t> s_rx_bad{0};
std::atomic<uint32_t> s_audio_drop{0};
std::atomic<uint32_t> s_log_drop{0};

void ring(std::atomic<bool> &bell, app::Sig sig)
{
    if (bell.exchange(true)) return;   // já tem uma na fila
    ao::Event e{};
    e.sig = sig;
    if (!s_self->post(e)) bell = false;   // fila cheia: a próxima escrita tenta de novo
}

// ---------------------------------------------------------------------------
// Log -> buffer. Chamado de qualquer task (nunca de ISR) e sem lock do lado
// do ESP-IDF, por isso o mutex. Linha inteira ou nada.
// ---------------------------------------------------------------------------
int log_vprintf(const char *fmt, va_list ap)
{
    static char line[LOG_LINE];   // protegido por s_log_mtx
    if (xSemaphoreTake(s_log_mtx, pdMS_TO_TICKS(20)) != pdTRUE) {
        s_log_drop.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    n     = std::min(n, static_cast<int>(sizeof(line)) - 1);
    bool wrote = false;
    if (n > 0) {
        if (xStreamBufferSpacesAvailable(s_log) >= static_cast<size_t>(n)) {
            xStreamBufferSend(s_log, line, n, 0);
            wrote = true;
        } else {
            s_log_drop.fetch_add(1, std::memory_order_relaxed);
        }
    }
    xSemaphoreGive(s_log_mtx);
    if (wrote) ring(s_log_bell, app::SIG_LOG_READY);
    return n;
}

// ---------------------------------------------------------------------------
// Roteamento: opcode -> AO dono. Comando novo = uma linha aqui + um case no AO.
// O comm só valida transporte (op conhecido, tamanho); o AO valida o conteúdo.
// ---------------------------------------------------------------------------
enum class To : uint8_t { Comm, Audio, Display, Storage };

struct Route {
    uint8_t  op;
    To       to;
    app::Sig sig;
    uint16_t min_len, max_len;
};

constexpr Route ROUTES[] = {
    {proto::OP_PING,         To::Comm,    app::SIG_PING,         0, 0},
    {proto::OP_STATS,        To::Comm,    app::SIG_STATS,        0, 0},
    {proto::OP_RESET,        To::Comm,    app::SIG_RESET,        0, 0},
    {proto::OP_ANIM_PLAY,    To::Display, app::SIG_ANIM_PLAY,    sizeof(proto::AnimId), sizeof(proto::AnimId)},
    {proto::OP_BACKLIGHT,    To::Display, app::SIG_BACKLIGHT,    sizeof(proto::Backlight), sizeof(proto::Backlight)},
    {proto::OP_STREAM,       To::Audio,   app::SIG_STREAM,       sizeof(proto::Stream), sizeof(proto::Stream)},
    {proto::OP_AUDIO_CFG,    To::Audio,   app::SIG_AUDIO_CFG,    sizeof(proto::Cfg), sizeof(proto::Cfg)},
    {proto::OP_UPLOAD_BEGIN, To::Storage, app::SIG_UPLOAD_BEGIN, sizeof(proto::UploadBegin), sizeof(proto::UploadBegin)},
    {proto::OP_UPLOAD_CHUNK, To::Storage, app::SIG_UPLOAD_CHUNK, 4 + 1, 4 + proto::CHUNK_MAX},
    {proto::OP_UPLOAD_END,   To::Storage, app::SIG_UPLOAD_END,   0, 0},
    {proto::OP_ANIM_LIST,    To::Storage, app::SIG_ANIM_LIST,    0, 0},
    {proto::OP_ANIM_DELETE,  To::Storage, app::SIG_ANIM_DELETE,  sizeof(proto::AnimId), sizeof(proto::AnimId)},
};

ao::ActiveObject *dest(To to)
{
    switch (to) {
    case To::Comm:    return app::comm;
    case To::Audio:   return app::audio;
    case To::Display: return app::display;
    case To::Storage: return app::storage;
    }
    return nullptr;
}

void route(const proto::Frame &f)
{
    if (f.ch != proto::CH_CMD) return;   // o host só manda comandos
    s_rx_frames.fetch_add(1, std::memory_order_relaxed);

    ao::Event e{};
    e.seq = f.seq;
    e.op  = f.op;
    const Route *r = std::find_if(std::begin(ROUTES), std::end(ROUTES), [&](const Route &x) { return x.op == f.op; });
    if (r == std::end(ROUTES)) return app::reply(e, proto::ST_UNKNOWN_OP);
    if (f.len < r->min_len || f.len > r->max_len) return app::reply(e, proto::ST_BAD_LEN);

    e.sig = r->sig;
    if (f.len <= sizeof(e.data)) {
        memcpy(e.data, f.payload, f.len);
        e.len = static_cast<uint8_t>(f.len);
    } else {
        e.blk = ao::blob_alloc(f.len);
        if (!e.blk) return app::reply(e, proto::ST_BUSY);
        memcpy(e.blk->data(), f.payload, f.len);
    }
    if (!dest(r->to)->post(e)) app::reply(e, proto::ST_BUSY);
}

void rx_task(void *)
{
    static uint8_t       buf[256];
    static proto::Parser parser;
    proto::Frame         f;
    uint32_t             overflows = 0;
    uart_event_t         ev;
    while (true) {
        if (xQueueReceive(s_uart_q, &ev, portMAX_DELAY) != pdTRUE) continue;
        switch (ev.type) {
        case UART_DATA:
            for (size_t left = ev.size; left > 0;) {
                const int n = uart_read_bytes(s_cfg.port, buf, std::min(left, sizeof(buf)), 0);
                if (n <= 0) break;
                left -= n;
                for (int i = 0; i < n; ++i)
                    if (parser.feed(buf[i], f)) route(f);
            }
            break;
        case UART_FIFO_OVF:
        case UART_BUFFER_FULL:
            // Perdeu bytes: descarta tudo e volta a procurar o próximo frame
            uart_flush_input(s_cfg.port);
            xQueueReset(s_uart_q);
            parser.reset();
            ++overflows;
            ESP_LOGW(TAG, "RX transbordou (%lu vezes)", static_cast<unsigned long>(overflows));
            break;
        default:
            break;
        }
        s_rx_bad.store(parser.bad() + overflows, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
class CommAO : public ao::ActiveObject {
public:
    CommAO() : ActiveObject("comm", 24) {}

protected:
    void on_start() override
    {
        const BaseType_t ok = xTaskCreatePinnedToCore(rx_task, "comm_rx", 3072, nullptr, 9, nullptr, 0);
        configASSERT(ok == pdPASS);
        const proto::EvtBoot boot{proto::VERSION, static_cast<uint8_t>(esp_reset_reason())};
        send(proto::CH_EVT, proto::EVT_BOOT, evt_seq_++, &boot, sizeof(boot));
    }

    void on_event(ao::Event &e) override
    {
        switch (e.sig) {
        case app::SIG_RSP:         send_rsp(e); break;
        case app::SIG_EVT:         send(proto::CH_EVT, e.op, evt_seq_++, e.data, e.len); break;
        case app::SIG_LOG_READY:   flush_log(); break;
        case app::SIG_AUDIO_READY: break;   // sai abaixo, quando a fila esvaziar
        case app::SIG_PING:        respond(e, proto::ST_OK, &proto::VERSION, 1); break;
        case app::SIG_STATS:       send_stats(e); break;
        case app::SIG_RESET:
            respond(e, proto::ST_OK);
            flush_log();
            uart_wait_tx_done(s_cfg.port, pdMS_TO_TICKS(200));
            esp_restart();
            break;
        default:
            ESP_LOGW(TAG, "sinal inesperado %u", e.sig);
            break;
        }
        // Áudio por último: resposta, evento e log que estiverem na fila passam na frente
        if (queued() == 0) flush_audio();
    }

private:
    void send(proto::Channel ch, uint8_t op, uint16_t seq, const void *p, size_t n)
    {
        n = std::min(n, proto::MAX_PAYLOAD);
        uart_write_bytes(s_cfg.port, tx_, proto::encode(tx_, ch, op, seq, p, static_cast<uint16_t>(n)));
    }

    // Resposta montada aqui mesmo (comandos do próprio comm)
    void respond(const ao::Event &req, proto::Status st, const void *p = nullptr, size_t n = 0)
    {
        uint8_t *pl = proto::payload_of(tx_);
        n           = std::min(n, proto::MAX_PAYLOAD - 1);
        pl[0]       = st;
        if (n) memcpy(pl + 1, p, n);
        uart_write_bytes(s_cfg.port, tx_, proto::seal(tx_, proto::CH_RSP, req.op, req.seq, static_cast<uint16_t>(1 + n)));
    }

    // Resposta de outro AO: data[0..len) (status + pouco) seguido do blob, se houver
    void send_rsp(const ao::Event &e)
    {
        uint8_t *pl = proto::payload_of(tx_);
        size_t   n  = std::min<size_t>(e.len, sizeof(e.data));
        memcpy(pl, e.data, n);
        if (e.blk) {
            const size_t extra = std::min<size_t>(e.blk->len, proto::MAX_PAYLOAD - n);
            memcpy(pl + n, e.blk->data(), extra);
            n += extra;
        }
        uart_write_bytes(s_cfg.port, tx_, proto::seal(tx_, proto::CH_RSP, e.op, e.seq, static_cast<uint16_t>(n)));
    }

    void send_stats(const ao::Event &e)
    {
        proto::Stats st{};
        st.uptime_ms  = static_cast<uint32_t>(esp_timer_get_time() / 1000);
        st.heap_free  = esp_get_free_heap_size();
        st.heap_min   = esp_get_minimum_free_heap_size();
        st.blob_bytes = ao::blob_bytes_in_use();
        st.rx_frames  = sat16(s_rx_frames.load());
        st.rx_bad     = sat16(s_rx_bad.load());
        st.audio_drop = sat16(s_audio_drop.load());
        st.log_drop   = sat16(s_log_drop.load());
        ao::ActiveObject *const aos[4] = {app::comm, app::audio, app::display, app::storage};
        for (int i = 0; i < 4; ++i) {
            if (!aos[i]) continue;
            st.ao[i].stack_free = aos[i]->task() ? sat16(uxTaskGetStackHighWaterMark(aos[i]->task())) : 0;
            st.ao[i].queued     = static_cast<uint8_t>(aos[i]->queued());
            st.ao[i].queue_peak = aos[i]->queue_peak();
            st.ao[i].dropped    = sat16(aos[i]->dropped());
        }
        respond(e, proto::ST_OK, &st, sizeof(st));
    }

    void flush_log()
    {
        s_log_bell = false;   // antes de ler: o que chegar durante a leitura toca de novo
        while (size_t n = xStreamBufferReceive(s_log, proto::payload_of(tx_), LOG_CHUNK, 0))
            uart_write_bytes(s_cfg.port, tx_, proto::seal(tx_, proto::CH_LOG, 0, log_seq_++, static_cast<uint16_t>(n)));
    }

    void flush_audio()
    {
        s_audio_bell = false;
        while (queued() == 0) {   // chegou evento? ele passa na frente; o resto sai na próxima
            const size_t n = xMessageBufferReceive(s_audio, audio_, sizeof(audio_), 0);
            if (n < 2) break;
            const uint16_t seq = static_cast<uint16_t>(audio_[0] | (audio_[1] << 8));
            send(proto::CH_AUDIO, 0, seq, audio_ + 2, n - 2);
        }
    }

    static uint16_t sat16(uint32_t v) { return v > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(v); }

    uint8_t  tx_[proto::MAX_FRAME];
    uint8_t  audio_[AUDIO_BLOCK_MAX];
    uint16_t evt_seq_ = 0;
    uint16_t log_seq_ = 0;
};

}  // namespace

ao::ActiveObject *create(const Config &cfg)
{
    s_cfg = cfg;
    ESP_ERROR_CHECK(uart_driver_install(cfg.port, UART_RX_BUF, UART_TX_BUF, UART_EVT_QUEUE, &s_uart_q, 0));
    ESP_ERROR_CHECK(uart_set_baudrate(cfg.port, cfg.baud));
    ESP_ERROR_CHECK(uart_set_pin(cfg.port, cfg.tx_pin, cfg.rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // printf e afins também pelo driver: cada escrita é atômica e não cai no meio de um frame
    uart_vfs_dev_use_driver(cfg.port);

    s_audio   = xMessageBufferCreate(AUDIO_BUF);
    s_log     = xStreamBufferCreate(LOG_BUF, 1);
    s_log_mtx = xSemaphoreCreateMutex();
    configASSERT(s_audio && s_log && s_log_mtx);

    static CommAO self;
    s_self = &self;
    esp_log_set_vprintf(log_vprintf);
    return s_self;
}

bool audio_push(uint16_t seq, const int16_t *lr, int frames)
{
    static uint8_t blk[AUDIO_BLOCK_MAX];   // só a task de áudio chama
    const size_t n = 2 + static_cast<size_t>(frames) * 4;
    if (frames <= 0 || n > sizeof(blk)) return false;
    blk[0] = static_cast<uint8_t>(seq);
    blk[1] = static_cast<uint8_t>(seq >> 8);
    memcpy(blk + 2, lr, n - 2);
    if (xMessageBufferSend(s_audio, blk, n, 0) != n) {
        s_audio_drop.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    ring(s_audio_bell, app::SIG_AUDIO_READY);
    return true;
}

}  // namespace comm
