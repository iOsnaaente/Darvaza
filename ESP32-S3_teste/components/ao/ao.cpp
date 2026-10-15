#include "ao.hpp"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"

namespace ao {

// ---------------------------------------------------------------------------
// Blobs
// ---------------------------------------------------------------------------
static std::atomic<size_t> s_blob_bytes{0};

Blob *blob_alloc(size_t len)
{
    const size_t total = sizeof(Blob) + len;
    size_t cur = s_blob_bytes.load(std::memory_order_relaxed);
    do {
        if (cur + total > BLOB_BUDGET) return nullptr;
    } while (!s_blob_bytes.compare_exchange_weak(cur, cur + total, std::memory_order_relaxed));

    auto *b = static_cast<Blob *>(heap_caps_malloc(total, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (!b) {
        s_blob_bytes.fetch_sub(total, std::memory_order_relaxed);
        return nullptr;
    }
    b->len  = static_cast<uint32_t>(len);
    b->size = static_cast<uint32_t>(total);
    return b;
}

void blob_free(Blob *b)
{
    if (!b) return;
    s_blob_bytes.fetch_sub(b->size, std::memory_order_relaxed);
    heap_caps_free(b);
}

size_t blob_bytes_in_use() { return s_blob_bytes.load(std::memory_order_relaxed); }

// ---------------------------------------------------------------------------
// ActiveObject
// ---------------------------------------------------------------------------
// Mesmo sem eventos o laço acorda neste intervalo para alimentar o watchdog
static constexpr TickType_t WDT_KICK = pdMS_TO_TICKS(1000);

ActiveObject::ActiveObject(const char *name, uint8_t queue_len) : name_(name)
{
    q_ = xQueueCreate(queue_len, sizeof(Event));
    configASSERT(q_);
}

void ActiveObject::start(UBaseType_t prio, uint32_t stack_bytes, BaseType_t core)
{
    const BaseType_t ok = xTaskCreatePinnedToCore(trampoline, name_, stack_bytes, this, prio, &task_, core);
    configASSERT(ok == pdPASS);
}

void ActiveObject::trampoline(void *self)
{
    static_cast<ActiveObject *>(self)->run();
    vTaskDelete(nullptr);
}

void ActiveObject::note_depth()
{
    const auto n = static_cast<uint8_t>(uxQueueMessagesWaiting(q_));
    uint8_t cur  = peak_.load(std::memory_order_relaxed);
    while (n > cur && !peak_.compare_exchange_weak(cur, n, std::memory_order_relaxed)) {}
}

bool ActiveObject::post(const Event &e)
{
    if (xQueueSend(q_, &e, 0) != pdTRUE) {
        blob_free(e.blk);
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    note_depth();
    return true;
}

bool ActiveObject::post_from_isr(const Event &e, BaseType_t *woken)
{
    // Sem blob aqui: ISR não aloca
    if (xQueueSendFromISR(q_, &e, woken) != pdTRUE) {
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

void ActiveObject::dispatch(Event &e)
{
    on_event(e);
    blob_free(e.blk);   // o handler não ficou com ele
    e.blk = nullptr;
}

void ActiveObject::drain()
{
    Event e;
    while (xQueueReceive(q_, &e, 0) == pdTRUE) dispatch(e);
}

void ActiveObject::run()
{
    ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
    on_start();
    Event e;
    while (true) {
        // No máximo um evento por volta e o tick logo que vencer: uma rajada de
        // eventos não congela o display, e um frame atrasado não trava a fila.
        const TickType_t t = next_tick();
        if (xQueueReceive(q_, &e, t < WDT_KICK ? t : WDT_KICK) == pdTRUE) dispatch(e);
        if (next_tick() == 0) on_tick();
        esp_task_wdt_reset();
    }
}

}  // namespace ao
