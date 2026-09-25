#include "anim.hpp"

#include <cstring>

namespace anim {
namespace {

Pose to_pose(const Key &k)
{
    Pose p;
    p.gx     = k.gx;
    p.gy     = k.gy;
    p.pupil  = k.pupil;
    p.lid[0] = k.lid_l / 255.0f;
    p.lid[1] = k.lid_r / 255.0f;
    return p;
}

Pose lerp(const Pose &a, const Pose &b, float t)
{
    Pose p;
    p.gx     = a.gx + (b.gx - a.gx) * t;
    p.gy     = a.gy + (b.gy - a.gy) * t;
    p.pupil  = a.pupil + (b.pupil - a.pupil) * t;
    p.lid[0] = a.lid[0] + (b.lid[0] - a.lid[0]) * t;
    p.lid[1] = a.lid[1] + (b.lid[1] - a.lid[1]) * t;
    return p;
}

}  // namespace

const char *validate(const uint8_t *p, size_t n)
{
    if (!p || n < sizeof(Header)) return "menor que o cabeçalho";
    if (n > MAX_BYTES) return "maior que MAX_BYTES";
    Header h;
    memcpy(&h, p, sizeof(h));
    if (h.magic != MAGIC) return "magic errado";
    if (h.version != VERSION) return "versão desconhecida";
    if (h.n_keys == 0) return "sem keyframes";
    if (n != sizeof(Header) + static_cast<size_t>(h.n_keys) * sizeof(Key)) return "tamanho não bate com n_keys";
    if (h.flags & F_LOOP) {
        const Key *k   = reinterpret_cast<const Key *>(p + sizeof(Header));
        uint32_t total = 0;
        for (uint16_t i = 0; i < h.n_keys; ++i) total += k[i].dur_ms;
        if (total == 0) return "loop com duração zero";
    }
    return nullptr;
}

void Player::start(const uint8_t *p, const Pose &from, uint32_t now_ms)
{
    Header h;
    memcpy(&h, p, sizeof(h));
    keys_      = reinterpret_cast<const Key *>(p + sizeof(Header));
    n_         = h.n_keys;
    flags_     = h.flags;
    idx_       = 0;
    seg_start_ = now_ms;
    from_      = from;
}

bool Player::pose(uint32_t now_ms, Pose &out)
{
    if (!keys_) {
        out = from_;
        return false;
    }
    while (true) {
        const Key     &k   = keys_[idx_];
        const uint32_t dur = k.dur_ms;
        const uint32_t el  = now_ms - seg_start_;
        if (el < dur) {
            out = lerp(from_, to_pose(k), static_cast<float>(el) / dur);
            return true;
        }
        // Segmento acabou: a key vira o ponto de partida do próximo
        from_ = to_pose(k);
        seg_start_ += dur;
        if (++idx_ < n_) continue;
        if (flags_ & F_LOOP) {
            idx_ = 0;
            continue;
        }
        idx_ = n_ - 1;
        keys_ = nullptr;
        out   = from_;
        return false;
    }
}

}  // namespace anim
