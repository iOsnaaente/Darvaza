#pragma once
/*
 * Animação v1: lista de keyframes da pose dos olhos (sem dependência do ESP-IDF).
 *
 * Arquivo (little-endian), no máximo MAX_BYTES:
 *   Header { magic u16 = 0x4E41 ("AN"), version u8 = 1, flags u8, n_keys u16, rsv u16 }
 *   Key x n_keys { dur_ms u16, gx i8, gy i8, pupil u8, lid_l u8, lid_r u8, rsv u8 }
 *
 * Cada key é a pose a ser alcançada em dur_ms, interpolando linearmente a
 * partir da anterior. A primeira parte da pose em que os olhos estavam quando a
 * animação começou (transição suave). gx/gy e pupil em px; lid 0 = aberto,
 * 255 = fechado. Flags: LOOP repete (a key 0 volta a partir da última); HOLD
 * congela a última pose ao terminar (senão volta ao modo automático).
 */
#include <cstddef>
#include <cstdint>

namespace anim {

constexpr uint16_t MAGIC     = 0x4E41;
constexpr uint8_t  VERSION   = 1;
constexpr size_t   MAX_BYTES = 4064;   // cabe num slot da flash (storage)

enum Flags : uint8_t { F_LOOP = 1, F_HOLD = 2 };

#pragma pack(push, 1)
struct Header {
    uint16_t magic;
    uint8_t  version;
    uint8_t  flags;
    uint16_t n_keys;
    uint16_t rsv;
};
struct Key {
    uint16_t dur_ms;
    int8_t   gx, gy;
    uint8_t  pupil;
    uint8_t  lid_l, lid_r;
    uint8_t  rsv;
};
#pragma pack(pop)
static_assert(sizeof(Header) == 8 && sizeof(Key) == 8, "layout da animação mudou: atualize tools/proto.py");

struct Pose {
    float gx = 0.0f, gy = 0.0f;   // olhar (px)
    float pupil = 26.0f;          // raio da pupila (px)
    float lid[2] = {0.0f, 0.0f};  // 0 = aberto .. 1 = fechado (olho esquerdo, direito)
};

// nullptr = ok; senão, o motivo
const char *validate(const uint8_t *p, size_t n);

class Player {
public:
    // p já validado; tem que continuar vivo enquanto a animação tocar
    void start(const uint8_t *p, const Pose &from, uint32_t now_ms);
    // Pose no instante now_ms; false = terminou (sem LOOP), out fica na última pose
    bool pose(uint32_t now_ms, Pose &out);
    bool hold() const { return flags_ & F_HOLD; }

private:
    const Key *keys_      = nullptr;
    uint16_t   n_         = 0;
    uint8_t    flags_     = 0;
    uint16_t   idx_       = 0;   // key que está sendo alcançada
    uint32_t   seg_start_ = 0;   // quando começou a ir para ela
    Pose       from_;            // pose no começo do segmento
};

}  // namespace anim
