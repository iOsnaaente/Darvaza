#pragma once
/*
 * Active Object leve: 1 task + 1 fila de eventos + estado privado.
 *
 * Regras da casa:
 *  - Ninguém chama métodos de um AO nem mexe no estado dele: só posta eventos na fila.
 *  - on_event roda até o fim (run-to-completion) e só bloqueia em recurso próprio
 *    (ex.: o DMA dos LCDs no display), nunca esperando outro AO.
 *  - ISR não processa nada: só posta (post_from_isr) ou libera um semáforo.
 *  - Cada AO se registra no task WDT e o alimenta no próprio laço.
 */
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace ao {

// ---------------------------------------------------------------------------
// Payload grande (> 20 B) que viaja junto com o evento. Quem recebe o evento
// vira dono do blob: se o handler não ficar com ele (ev.blk = nullptr), o laço
// do AO libera sozinho depois do on_event. O total alocado tem teto
// (BLOB_BUDGET): passou dele, blob_alloc falha e quem pediu responde BUSY.
// ---------------------------------------------------------------------------
constexpr size_t BLOB_BUDGET = 24 * 1024;

struct Blob {
    uint32_t len;    // bytes válidos em data()
    uint32_t size;   // bytes alocados (para o orçamento)
    uint8_t       *data()       { return reinterpret_cast<uint8_t *>(this + 1); }
    const uint8_t *data() const { return reinterpret_cast<const uint8_t *>(this + 1); }
};

Blob  *blob_alloc(size_t len);   // nullptr se estourar o orçamento ou faltar heap
void   blob_free(Blob *b);       // aceita nullptr
size_t blob_bytes_in_use();

// ---------------------------------------------------------------------------
// Evento: 32 bytes, copiado para dentro da fila
// ---------------------------------------------------------------------------
struct Event {
    uint16_t sig;        // o que aconteceu (app::Sig)
    uint16_t seq;        // seq do comando do host que originou (para a resposta)
    uint8_t  op;         // opcode desse comando (idem)
    uint8_t  len;        // bytes válidos em data
    uint8_t  rsv[2];
    uint8_t  data[20];   // payload pequeno (structs packed do proto, little-endian)
    Blob    *blk;        // payload grande, ou nullptr

    template <typename T> void put(const T &v)
    {
        static_assert(sizeof(T) <= sizeof(data), "payload não cabe no evento");
        memcpy(data, &v, sizeof(T));
        len = sizeof(T);
    }
    template <typename T> T get() const
    {
        static_assert(sizeof(T) <= sizeof(data), "payload não cabe no evento");
        T v;
        memcpy(&v, data, sizeof(T));
        return v;
    }
};
static_assert(sizeof(Event) == 32, "evento deve ter 32 B");

// ---------------------------------------------------------------------------
class ActiveObject {
public:
    ActiveObject(const char *name, uint8_t queue_len);
    virtual ~ActiveObject() = default;
    ActiveObject(const ActiveObject &)            = delete;
    ActiveObject &operator=(const ActiveObject &) = delete;

    // Cria a task. Os AOs podem receber eventos antes disso (ficam na fila).
    void start(UBaseType_t prio, uint32_t stack_bytes, BaseType_t core);

    // O evento é sempre consumido: se a fila estiver cheia, o blob é liberado,
    // conta em dropped() e retorna false (quem posta decide: responde BUSY, ignora...).
    bool post(const Event &e);
    bool post_from_isr(const Event &e, BaseType_t *woken);

    // Métricas (seguras de ler de qualquer task)
    const char  *name() const { return name_; }
    TaskHandle_t task() const { return task_; }
    UBaseType_t  queued() const { return uxQueueMessagesWaiting(q_); }
    uint8_t      queue_peak() const { return peak_.load(std::memory_order_relaxed); }
    uint32_t     dropped() const { return dropped_.load(std::memory_order_relaxed); }

protected:
    virtual void on_start() {}
    virtual void on_event(Event &e) = 0;
    // Ticks até o próximo on_tick (0 = já venceu; portMAX_DELAY = não tem tick)
    virtual TickType_t next_tick() { return portMAX_DELAY; }
    virtual void on_tick() {}
    // Laço padrão: espera evento ou tick. Um AO com outro "relógio" (o áudio
    // bloqueia no I2S) troca o laço e chama drain() entre um bloco e outro.
    virtual void run();

    void dispatch(Event &e);   // on_event + libera o blob que ninguém pegou
    void drain();              // trata tudo que está na fila, sem bloquear

private:
    static void trampoline(void *self);
    void note_depth();

    const char           *name_;
    QueueHandle_t         q_    = nullptr;
    TaskHandle_t          task_ = nullptr;
    std::atomic<uint8_t>  peak_{0};
    std::atomic<uint32_t> dropped_{0};
};

}  // namespace ao
