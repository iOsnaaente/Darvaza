/*
 * StorageAO: dono da flash. Toda escrita (animações e NVS) passa por aqui, numa
 * task de prioridade baixa: gravar/apagar flash congela o cache dos dois cores,
 * então é melhor que aconteça em um lugar só e em pedaços pequenos.
 *
 * Partição "anims" dividida em slots de 4 KB (um setor):
 *   [SlotHdr 24 B | reservado até 32 B | dados da animação (até 4064 B)]
 * Upload: BEGIN apaga um slot livre -> CHUNKs gravam os dados em ordem ->
 * END confere o CRC (do que chegou e relido da flash) e só então grava o
 * cabeçalho. O cabeçalho é o commit: queda de energia antes dele = slot
 * ignorado, a versão antiga continua valendo. Não há índice na NVS: o boot
 * varre os cabeçalhos. Apagar = zerar o campo 'alive' (bits 1 -> 0 não
 * precisam de erase).
 */
#include "storage.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include "esp_log.h"
#include "esp_partition.h"

#include "app.hpp"

namespace storage {
namespace {

const char *const TAG = "storage";

constexpr uint32_t SLOT      = 4096;
constexpr uint32_t HDR       = 32;
constexpr uint32_t SLOT_DATA = SLOT - HDR;   // = anim::MAX_BYTES
constexpr uint32_t MAGIC     = 0x31494E41;   // "ANI1"
constexpr uint32_t ALIVE     = 0xFFFFFFFF;
constexpr int      MAX_SLOTS = 256;

#pragma pack(push, 1)
struct SlotHdr {
    uint32_t magic;
    uint16_t id;
    uint16_t rsv;
    uint32_t size;
    uint32_t crc;     // crc32 dos dados
    uint32_t gen;     // geração: se houver duas com o mesmo id, a maior vence
    uint32_t alive;   // ALIVE = valendo; 0 = apagada
};
#pragma pack(pop)
constexpr uint32_t ALIVE_AT = offsetof(SlotHdr, alive);   // o commit grava só até aqui
static_assert(sizeof(SlotHdr) <= HDR, "cabeçalho maior que o espaço reservado");

struct Entry {
    uint16_t id, slot;
    uint32_t size, crc, gen;
};

class StorageAO : public ao::ActiveObject {
public:
    explicit StorageAO(const esp_partition_t *part) : ActiveObject("storage", 8), part_(part)
    {
        if (!part_) {
            ESP_LOGE(TAG, "partição 'anims' não encontrada: grave a tabela de partições nova (idf.py flash)");
            return;
        }
        n_slots_ = static_cast<int>(std::min<uint32_t>(part_->size / SLOT, MAX_SLOTS));
        scan();
    }

protected:
    void on_event(ao::Event &e) override
    {
        if (e.sig == app::SIG_CFG_SAVE) return save_cfg(e);
        if (!part_) {   // sem partição: só a NVS funciona
            if (e.sig == app::SIG_ANIM_LOAD) return loaded(e, proto::ST_STORAGE, nullptr);
            return app::reply(e, proto::ST_STORAGE);
        }
        switch (e.sig) {
        case app::SIG_UPLOAD_BEGIN: begin(e); break;
        case app::SIG_UPLOAD_CHUNK: chunk(e); break;
        case app::SIG_UPLOAD_END:   end(e); break;
        case app::SIG_ANIM_LIST:    list(e); break;
        case app::SIG_ANIM_DELETE:  remove(e); break;
        case app::SIG_ANIM_LOAD:    load(e); break;
        default:
            ESP_LOGW(TAG, "sinal inesperado %u", e.sig);
            break;
        }
    }

private:
    // ---- índice ---------------------------------------------------------
    void scan()
    {
        for (int s = 0; s < n_slots_; ++s) {
            SlotHdr h;
            if (esp_partition_read(part_, s * SLOT, &h, sizeof(h)) != ESP_OK) continue;
            if (h.magic != MAGIC || h.alive != ALIVE || h.size == 0 || h.size > SLOT_DATA) continue;
            gen_ = std::max(gen_, h.gen);
            const Entry fresh{h.id, static_cast<uint16_t>(s), h.size, h.crc, h.gen};
            if (Entry *old = find(h.id)) {
                // Energia caiu entre gravar a nova e apagar a velha: fica a mais nova
                if (h.gen > old->gen) {
                    kill(old->slot);
                    *old = fresh;
                } else {
                    kill(s);
                }
                continue;
            }
            idx_[n_idx_++] = fresh;
        }
        ESP_LOGI(TAG, "%d animação(ões) gravada(s), %d slots de %lu B", n_idx_, n_slots_,
                 static_cast<unsigned long>(SLOT));
    }

    Entry *find(uint16_t id)
    {
        for (int i = 0; i < n_idx_; ++i)
            if (idx_[i].id == id) return &idx_[i];
        return nullptr;
    }

    bool used(int slot) const
    {
        for (int i = 0; i < n_idx_; ++i)
            if (idx_[i].slot == slot) return true;
        return false;
    }

    // Próximo slot livre a partir do último usado: espalha o desgaste
    int free_slot()
    {
        for (int i = 0; i < n_slots_; ++i) {
            const int s = (next_ + i) % n_slots_;
            if (!used(s)) {
                next_ = (s + 1) % n_slots_;
                return s;
            }
        }
        return -1;
    }

    bool kill(int slot)
    {
        const uint32_t dead = 0;
        return esp_partition_write(part_, slot * SLOT + ALIVE_AT, &dead, sizeof(dead)) == ESP_OK;
    }

    // CRC relido da flash (pega erro de gravação)
    uint32_t flash_crc(int slot, uint32_t size)
    {
        uint8_t  buf[256];
        uint32_t crc = 0;
        for (uint32_t off = 0; off < size; off += sizeof(buf)) {
            const uint32_t n = std::min<uint32_t>(sizeof(buf), size - off);
            if (esp_partition_read(part_, slot * SLOT + HDR + off, buf, n) != ESP_OK) return ~crc;
            crc = proto::crc32(buf, n, crc);
        }
        return crc;
    }

    // ---- upload ---------------------------------------------------------
    void begin(const ao::Event &e)
    {
        const auto b = e.get<proto::UploadBegin>();
        up_.active   = false;   // um BEGIN novo cancela o upload anterior
        if (b.id == 0 || b.id >= 0x8000 || b.size == 0 || b.size > SLOT_DATA) return app::reply(e, proto::ST_BAD_ARG);
        const int s = free_slot();
        if (s < 0) return app::reply(e, proto::ST_NO_SPACE);
        // Apaga o slot inteiro agora (~45 ms): os chunks depois só gravam (~1 ms/KB)
        if (esp_partition_erase_range(part_, s * SLOT, SLOT) != ESP_OK) return app::reply(e, proto::ST_STORAGE);
        up_ = {true, b.id, static_cast<uint16_t>(s), b.size, b.crc32, 0, 0};
        app::reply(e, proto::ST_OK);
    }

    void chunk(const ao::Event &e)
    {
        if (!up_.active) return app::reply(e, proto::ST_BAD_STATE);
        const uint8_t *p = e.blk ? e.blk->data() : e.data;
        uint32_t       n = e.blk ? e.blk->len : e.len;
        uint32_t       off;
        memcpy(&off, p, sizeof(off));
        p += sizeof(off);
        n -= sizeof(off);
        if (off + n <= up_.got) return app::reply(e, proto::ST_OK, &up_.got, sizeof(up_.got));   // repetido (resposta perdida)
        if (off != up_.got || off + n > up_.size) return app::reply(e, proto::ST_BAD_ARG, &up_.got, sizeof(up_.got));
        if (esp_partition_write(part_, up_.slot * SLOT + HDR + off, p, n) != ESP_OK) {
            up_.active = false;
            return app::reply(e, proto::ST_STORAGE);
        }
        up_.crc_now = proto::crc32(p, n, up_.crc_now);
        up_.got += n;
        app::reply(e, proto::ST_OK, &up_.got, sizeof(up_.got));
    }

    void end(const ao::Event &e)
    {
        if (!up_.active || up_.got != up_.size) return app::reply(e, proto::ST_BAD_STATE);
        up_.active = false;
        if (up_.crc_now != up_.crc || flash_crc(up_.slot, up_.size) != up_.crc) return app::reply(e, proto::ST_BAD_CRC);

        const SlotHdr h{MAGIC, up_.id, 0xFFFF, up_.size, up_.crc, gen_ + 1, ALIVE};
        if (esp_partition_write(part_, up_.slot * SLOT, &h, ALIVE_AT) != ESP_OK) return app::reply(e, proto::ST_STORAGE);
        ++gen_;
        const Entry fresh{up_.id, up_.slot, up_.size, up_.crc, h.gen};
        if (Entry *old = find(up_.id)) {   // substituiu: a velha sai só depois do commit da nova
            kill(old->slot);
            *old = fresh;
        } else {
            idx_[n_idx_++] = fresh;
        }
        ESP_LOGI(TAG, "animação %u gravada: %lu B no slot %u", up_.id, static_cast<unsigned long>(up_.size), up_.slot);
        app::reply(e, proto::ST_OK);
    }

    // ---- consultas ------------------------------------------------------
    void list(const ao::Event &e)
    {
        ao::Blob *b = ao::blob_alloc(2 + 2 * n_idx_);
        if (!b) return app::reply(e, proto::ST_BUSY);
        const uint16_t n = static_cast<uint16_t>(n_idx_);
        memcpy(b->data(), &n, 2);
        for (int i = 0; i < n_idx_; ++i) memcpy(b->data() + 2 + 2 * i, &idx_[i].id, 2);
        app::reply_blob(e, proto::ST_OK, b);
    }

    void remove(const ao::Event &e)
    {
        const uint16_t id = e.get<proto::AnimId>().id;
        Entry *en = find(id);
        if (!en) return app::reply(e, proto::ST_NOT_FOUND);
        if (!kill(en->slot)) return app::reply(e, proto::ST_STORAGE);
        *en = idx_[--n_idx_];
        ESP_LOGI(TAG, "animação %u apagada", id);
        app::reply(e, proto::ST_OK);
    }

    // Pedido do display: lê a animação num blob (que vai junto na resposta)
    void load(const ao::Event &e)
    {
        const Entry *en = find(e.get<proto::AnimId>().id);
        if (!en) return loaded(e, proto::ST_NOT_FOUND, nullptr);
        ao::Blob *b = ao::blob_alloc(en->size);
        if (!b) return loaded(e, proto::ST_BUSY, nullptr);
        if (esp_partition_read(part_, en->slot * SLOT + HDR, b->data(), en->size) != ESP_OK ||
            proto::crc32(b->data(), en->size) != en->crc) {
            ESP_LOGE(TAG, "animação %u corrompida na flash (slot %u)", en->id, en->slot);
            ao::blob_free(b);
            return loaded(e, proto::ST_STORAGE, nullptr);
        }
        loaded(e, proto::ST_OK, b);
    }

    void loaded(const ao::Event &req, proto::Status st, ao::Blob *b)
    {
        ao::Event r{};
        r.sig     = app::SIG_ANIM_LOADED;
        r.seq     = req.seq;
        r.op      = req.op;
        r.data[0] = st;
        memcpy(r.data + 1, req.data, sizeof(uint16_t));   // id
        r.len     = 3;
        r.blk     = b;
        if (!app::display->post(r)) ESP_LOGW(TAG, "fila do display cheia: animação descartada");
    }

    void save_cfg(const ao::Event &e)
    {
        const auto c = e.get<proto::Cfg>();
        if (!app::cfg_put(static_cast<proto::CfgKey>(c.key), c.value))
            ESP_LOGE(TAG, "NVS: não gravou o parâmetro %u", c.key);
    }

    struct Upload {
        bool     active;
        uint16_t id, slot;
        uint32_t size, crc, got, crc_now;
    };

    const esp_partition_t *part_;
    int      n_slots_ = 0;
    Entry    idx_[MAX_SLOTS];
    int      n_idx_ = 0;
    uint32_t gen_   = 0;
    int      next_  = 0;
    Upload   up_{};
};

}  // namespace

ao::ActiveObject *create()
{
    static StorageAO self(esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "anims"));
    return &self;
}

}  // namespace storage
