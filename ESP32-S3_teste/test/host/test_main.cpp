/*
 * Testes no PC das partes sem ESP-IDF: protocolo, animação e SSL.
 *
 *   cmake -S test/host -B build-host && cmake --build build-host && build-host/host_tests
 */
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

#include "anim.hpp"
#include "proto.hpp"
#include "ssl.hpp"

static int s_fail = 0, s_pass = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (cond) ++s_pass;                                                    \
        else { ++s_fail; std::printf("FALHOU %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)
#define CHECK_NEAR(a, b, tol) CHECK(std::fabs((a) - (b)) <= (tol))

// ---------------------------------------------------------------------------
// Protocolo
// ---------------------------------------------------------------------------
// Frame de referência (CRC calculado com binascii.crc_hqx, do Python; o mesmo
// vetor está em tools/test_proto.py): CMD, ANIM_PLAY, seq 0x1234, id 0x8001
static const uint8_t REF_FRAME[] = {0xA5, 0x5A, 0x01, 0x10, 0x34, 0x12, 0x02, 0x00, 0x01, 0x80, 0xB3, 0x67};

static std::vector<proto::Frame> parse_all(proto::Parser &p, const std::vector<uint8_t> &bytes,
                                           std::vector<std::vector<uint8_t>> *payloads = nullptr)
{
    std::vector<proto::Frame> out;
    proto::Frame f;
    for (uint8_t b : bytes)
        if (p.feed(b, f)) {
            out.push_back(f);
            if (payloads) payloads->emplace_back(f.payload, f.payload + f.len);
        }
    return out;
}

static std::vector<uint8_t> frame(proto::Channel ch, uint8_t op, uint16_t seq, std::vector<uint8_t> payload)
{
    std::vector<uint8_t> buf(proto::MAX_FRAME);
    const size_t n = proto::encode(buf.data(), ch, op, seq, payload.data(), static_cast<uint16_t>(payload.size()));
    buf.resize(n);
    return buf;
}

static void test_crc()
{
    const auto *s = reinterpret_cast<const uint8_t *>("123456789");
    CHECK(proto::crc16(s, 9) == 0x29B1);        // valor de verificação do CRC-16/CCITT-FALSE
    CHECK(proto::crc32(s, 9) == 0xCBF43926);    // = zlib.crc32
    CHECK(proto::crc32(s + 4, 5, proto::crc32(s, 4)) == 0xCBF43926);   // encadeável (leitura em pedaços)
}

static void test_encode()
{
    const auto f = frame(proto::CH_CMD, proto::OP_ANIM_PLAY, 0x1234, {0x01, 0x80});
    CHECK(f.size() == sizeof(REF_FRAME));
    CHECK(memcmp(f.data(), REF_FRAME, sizeof(REF_FRAME)) == 0);
}

static void test_parser()
{
    {   // lixo, frame, lixo que parece SOF, frame vazio
        proto::Parser p;
        std::vector<uint8_t> in = {'o', 'l', 'a', 0xA5, 0x00};
        in.insert(in.end(), std::begin(REF_FRAME), std::end(REF_FRAME));
        in.insert(in.end(), {0xA5, 0xA5});   // SOF0 repetido antes do próximo
        const auto ping = frame(proto::CH_CMD, proto::OP_PING, 7, {});
        in.insert(in.end(), ping.begin() + 1, ping.end());   // o 0xA5 acima serve de SOF0
        std::vector<std::vector<uint8_t>> pl;
        const auto fs = parse_all(p, in, &pl);
        CHECK(fs.size() == 2);
        if (fs.size() == 2) {
            CHECK(fs[0].ch == proto::CH_CMD && fs[0].op == proto::OP_ANIM_PLAY && fs[0].seq == 0x1234);
            CHECK(pl[0] == std::vector<uint8_t>({0x01, 0x80}));
            CHECK(fs[1].op == proto::OP_PING && fs[1].seq == 7 && fs[1].len == 0);
        }
        CHECK(p.bad() == 0);
    }
    {   // CRC errado: descarta e conta
        proto::Parser p;
        std::vector<uint8_t> in(std::begin(REF_FRAME), std::end(REF_FRAME));
        in[9] ^= 0x01;
        CHECK(parse_all(p, in).empty());
        CHECK(p.bad() == 1);
    }
    {   // tamanho absurdo no cabeçalho: descarta sem esperar 64 KB
        proto::Parser p;
        std::vector<uint8_t> in = {0xA5, 0x5A, 0x01, 0x01, 0x00, 0x00, 0xFF, 0xFF};
        const auto ping = frame(proto::CH_CMD, proto::OP_PING, 1, {});
        in.insert(in.end(), ping.begin(), ping.end());
        CHECK(parse_all(p, in).size() == 1);
        CHECK(p.bad() == 1);
    }
    {   // frame truncado come o começo do seguinte (o host reenvia); o terceiro passa
        proto::Parser p;
        std::vector<uint8_t> in(std::begin(REF_FRAME), std::begin(REF_FRAME) + 9);   // falta 1 byte + CRC
        const auto a = frame(proto::CH_CMD, proto::OP_PING, 1, {});
        const auto b = frame(proto::CH_CMD, proto::OP_STATS, 2, {});
        in.insert(in.end(), a.begin(), a.end());
        in.insert(in.end(), b.begin(), b.end());
        const auto fs = parse_all(p, in);
        CHECK(fs.size() == 1 && fs[0].op == proto::OP_STATS);
        CHECK(p.bad() == 1);
    }
    {   // payload máximo, byte a byte
        proto::Parser p;
        std::vector<uint8_t> big(proto::MAX_PAYLOAD);
        for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<uint8_t>(i * 7);
        std::vector<std::vector<uint8_t>> pl;
        const auto fs = parse_all(p, frame(proto::CH_AUDIO, 0, 42, big), &pl);
        CHECK(fs.size() == 1 && pl.size() == 1 && pl[0] == big);
    }
}

// ---------------------------------------------------------------------------
// Animação
// ---------------------------------------------------------------------------
static std::vector<uint8_t> clip(uint8_t flags, std::vector<anim::Key> keys)
{
    anim::Header h{anim::MAGIC, anim::VERSION, flags, static_cast<uint16_t>(keys.size()), 0};
    std::vector<uint8_t> out(sizeof(h) + keys.size() * sizeof(anim::Key));
    memcpy(out.data(), &h, sizeof(h));
    memcpy(out.data() + sizeof(h), keys.data(), keys.size() * sizeof(anim::Key));
    return out;
}

static void test_anim_validate()
{
    // Mesmo vetor de tools/test_proto.py (proto.anim_build): 1 key, 100 ms, gx 10, pupila 26, lid_r 255
    static const uint8_t REF_ANIM[] = {0x41, 0x4E, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00,
                                       0x64, 0x00, 0x0A, 0x00, 0x1A, 0x00, 0xFF, 0x00};
    const auto ref = clip(0, {{100, 10, 0, 26, 0, 255, 0}});
    CHECK(ref.size() == sizeof(REF_ANIM) && memcmp(ref.data(), REF_ANIM, sizeof(REF_ANIM)) == 0);
    CHECK(anim::validate(REF_ANIM, sizeof(REF_ANIM)) == nullptr);

    const auto ok = clip(0, {{100, 10, 0, 26, 0, 0, 0}});
    CHECK(anim::validate(ok.data(), ok.size()) == nullptr);

    auto bad = ok;
    bad[0] ^= 0xFF;
    CHECK(anim::validate(bad.data(), bad.size()) != nullptr);   // magic
    CHECK(anim::validate(ok.data(), ok.size() - 1) != nullptr);   // tamanho x n_keys
    const auto empty = clip(0, {});
    CHECK(anim::validate(empty.data(), empty.size()) != nullptr);
    const auto zero_loop = clip(anim::F_LOOP, {{0, 0, 0, 26, 0, 0, 0}});
    CHECK(anim::validate(zero_loop.data(), zero_loop.size()) != nullptr);   // travaria o player
    std::vector<uint8_t> huge = clip(0, std::vector<anim::Key>(600, anim::Key{10, 0, 0, 26, 0, 0, 0}));
    CHECK(anim::validate(huge.data(), huge.size()) != nullptr);   // > MAX_BYTES
}

static void test_anim_player()
{
    anim::Pose from;   // gx 0, pupil 26, olhos abertos
    {
        const auto c = clip(0, {{100, 10, 0, 26, 0, 255, 0}, {100, 20, 0, 26, 0, 0, 0}});
        anim::Player pl;
        anim::Pose   p;
        pl.start(c.data(), from, 1000);
        CHECK(pl.pose(1050, p));
        CHECK_NEAR(p.gx, 5.0f, 1e-4f);
        CHECK_NEAR(p.lid[1], 0.5f, 1e-4f);   // piscadela só no direito
        CHECK_NEAR(p.lid[0], 0.0f, 1e-4f);
        CHECK(pl.pose(1150, p));
        CHECK_NEAR(p.gx, 15.0f, 1e-4f);
        CHECK(!pl.pose(1200, p));   // acabou
        CHECK_NEAR(p.gx, 20.0f, 1e-4f);
        CHECK(!pl.hold());
        CHECK(!pl.pose(5000, p));   // continua parado na última pose
        CHECK_NEAR(p.gx, 20.0f, 1e-4f);
    }
    {   // loop: depois da última key volta para a primeira, partindo da última
        const auto c = clip(anim::F_LOOP, {{100, 10, 0, 26, 0, 0, 0}, {100, 20, 0, 26, 0, 0, 0}});
        anim::Player pl;
        anim::Pose   p;
        pl.start(c.data(), from, 0);
        CHECK(pl.pose(250, p));
        CHECK_NEAR(p.gx, 15.0f, 1e-4f);   // 20 -> 10, na metade
        CHECK(pl.pose(100 * 2 * 1000 + 50, p));   // muitas voltas depois
        CHECK_NEAR(p.gx, 15.0f, 1e-4f);
    }
    {   // hold e key com duração zero (salto)
        const auto c = clip(anim::F_HOLD, {{0, -30, 5, 20, 255, 255, 0}});
        anim::Player pl;
        anim::Pose   p;
        pl.start(c.data(), from, 0);
        CHECK(!pl.pose(0, p));
        CHECK(pl.hold());
        CHECK_NEAR(p.gx, -30.0f, 1e-4f);
        CHECK_NEAR(p.lid[0], 1.0f, 1e-4f);
    }
}

// ---------------------------------------------------------------------------
// SSL: ruído com atraso conhecido entre os mics -> ângulo estimado
// ---------------------------------------------------------------------------
static void test_ssl_direction(int delay, float expect_s)
{
    ssl::Tracker t;
    std::mt19937 rng(1234 + delay);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    constexpr float FS = 8388608.0f * 256.0f;   // 24 bits alinhados à esquerda em 32
    std::vector<float> src(ssl::BLOCK + 16, 0.0f);
    std::vector<int32_t> buf(2 * ssl::BLOCK);

    const int blocks = 5 * 1000 / 16;   // 5 s
    for (int b = 0; b < blocks; ++b) {
        // 1 s de quase silêncio para o fundo assentar; depois 160 ms de ruído a cada 480 ms
        const bool burst = b >= 62 && (b % 30) < 10;
        const float amp  = burst ? 0.05f : 0.0001f;
        // Desloca o histórico e gera o bloco: r = fonte, l = fonte atrasada 'delay'
        // amostras (l ouve depois = som vindo da direita, tdoa > 0)
        std::copy(src.end() - 16, src.end(), src.begin());
        for (int i = 16; i < static_cast<int>(src.size()); ++i) src[i] = gauss(rng) * amp;
        for (int i = 0; i < ssl::BLOCK; ++i) {
            const float r = src[16 + i];
            const float l = src[16 + i - delay];
            buf[2 * i]     = static_cast<int32_t>(l * FS);
            buf[2 * i + 1] = static_cast<int32_t>(r * FS);
        }
        t.process(buf.data(), ssl::BLOCK);
    }
    const auto &o = t.out();
    std::printf("  atraso %+d: s=%+.3f (esperado %+.3f)  sigma=%.3f  tdoa=%+.2f  coer=%.2f  pol=%+.2f\n", delay, o.s,
                expect_s, o.sigma, o.tdoa, o.coherence, o.polarity);
    CHECK_NEAR(o.tdoa, static_cast<float>(delay), 0.5f);
    CHECK_NEAR(o.s, expect_s, 0.12f);
    CHECK(o.sigma < 0.35f);   // FOLLOW_SIGMA: o display seguiria
    CHECK(o.coherence > 0.65f);
    // Mics com a mesma polaridade: 'pol' > 0 (o valor cai com o atraso porque o
    // ruído branco vaza acima de 300 Hz pelos filtros de 1ª ordem)
    CHECK(o.polarity > 0.0f);
}

int main()
{
    test_crc();
    test_encode();
    test_parser();
    test_anim_validate();
    test_anim_player();
    const float tau_max = 0.15f / 343.0f * ssl::SAMPLE_RATE;
    for (int d : {-5, -2, 0, 3, 6}) test_ssl_direction(d, std::fmin(1.0f, std::fmax(-1.0f, d / tau_max)));

    std::printf("%d ok, %d falhas\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
