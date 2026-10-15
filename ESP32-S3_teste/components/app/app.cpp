#include "app.hpp"

#include <cstdio>
#include <cstring>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "esp_err.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace app {

ao::ActiveObject *comm    = nullptr;
ao::ActiveObject *audio   = nullptr;
ao::ActiveObject *display = nullptr;
ao::ActiveObject *storage = nullptr;

static QueueHandle_t s_ssl = nullptr;
static const char *const NVS_NS = "cfg";

void init()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Partição cheia ou de outra versão do IDF: recomeça do zero (só perde os parâmetros)
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_ssl = xQueueCreate(1, sizeof(SslState));
    configASSERT(s_ssl);
}

static void nvs_key(proto::CfgKey key, char (&name)[8]) { snprintf(name, sizeof(name), "k%u", static_cast<unsigned>(key)); }

float cfg_get(proto::CfgKey key, float def)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return def;   // nada gravado ainda
    char     name[8];
    uint32_t raw = 0;
    nvs_key(key, name);
    const esp_err_t err = nvs_get_u32(h, name, &raw);
    nvs_close(h);
    if (err != ESP_OK) return def;
    float v;
    memcpy(&v, &raw, sizeof(v));
    return v;
}

bool cfg_put(proto::CfgKey key, float value)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return false;
    char     name[8];
    uint32_t raw;
    nvs_key(key, name);
    memcpy(&raw, &value, sizeof(raw));
    const bool ok = nvs_set_u32(h, name, raw) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

void reply(const ao::Event &req, proto::Status st, const void *data, size_t len)
{
    ao::Event e{};
    e.sig     = SIG_RSP;
    e.seq     = req.seq;
    e.op      = req.op;
    e.data[0] = st;
    if (len > sizeof(e.data) - 1) {
        // Não cabe: vai como blob (raro; se faltar memória, sai só o status)
        e.blk = ao::blob_alloc(len);
        if (e.blk) memcpy(e.blk->data(), data, len);
        e.len = 1;
    } else {
        if (len) memcpy(e.data + 1, data, len);
        e.len = static_cast<uint8_t>(1 + len);
    }
    comm->post(e);
}

void reply_blob(const ao::Event &req, proto::Status st, ao::Blob *payload)
{
    ao::Event e{};
    e.sig     = SIG_RSP;
    e.seq     = req.seq;
    e.op      = req.op;
    e.data[0] = st;
    e.len     = 1;
    e.blk     = payload;
    comm->post(e);
}

void notify(proto::Evt evt, const void *data, size_t len)
{
    ao::Event e{};
    e.sig = SIG_EVT;
    e.op  = evt;
    if (len > sizeof(e.data)) len = sizeof(e.data);
    if (len) memcpy(e.data, data, len);
    e.len = static_cast<uint8_t>(len);
    comm->post(e);
}

void save_cfg(proto::CfgKey key, float value)
{
    ao::Event e{};
    e.sig = SIG_CFG_SAVE;
    e.put(proto::Cfg{key, value});
    storage->post(e);
}

void ssl_publish(const SslState &s) { xQueueOverwrite(s_ssl, &s); }

bool ssl_read(SslState &s) { return xQueuePeek(s_ssl, &s, 0) == pdTRUE; }

}  // namespace app
