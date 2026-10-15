#include "ssl.hpp"

#include <algorithm>
#include <cmath>

namespace ssl {
namespace {

// Localização do som: diferença de tempo de chegada (TDOA) entre os mics via GCC-PHAT
constexpr float HPF_HZ         = 300.0f;   // corta ronco/hum; voz e palmas ficam
constexpr float MIN_LEVEL      = 0.0005f;  // ~ -66 dBFS: abaixo disso ignora mesmo que acima do fundo
constexpr float MIC_DIST_M     = 0.15f;    // distância entre os mics
constexpr float SOUND_MPS      = 343.0f;
constexpr float TAU_MAX        = MIC_DIST_M / SOUND_MPS * SAMPLE_RATE;   // ~7 amostras: som a 90°
constexpr int   MAX_LAG        = static_cast<int>(TAU_MAX) + 2;          // busca com folga
constexpr float GCC_LO_HZ      = 300.0f;   // faixa usada na correlação
constexpr float GCC_HI_HZ      = 4000.0f;
constexpr float XSPEC_ALPHA    = 0.8f;     // média do espectro cruzado entre blocos (~80 ms de memória)
constexpr float XSPEC_DECAY    = 0.5f;     // no silêncio a média some rápido: som novo não herda o antigo
// Efeito de precedência: o som direto domina no começo/pico de cada som; quando ele
// decai, os ecos das paredes (que vêm de outras direções) passam a dominar.
constexpr float PEAK_DECAY     = 0.8f;     // pico de energia cai ~1 dB/bloco, mais devagar que a reverberação
constexpr float MIN_WEIGHT     = 0.2f;     // bloco abaixo disso (decaindo, ~3.5 dB abaixo do pico) é ignorado
constexpr float ONSET_BOOST    = 2.0f;     // bloco com +6 dB de subida vale o dobro
constexpr float MIN_COHERENCE  = 0.30f;    // pico abaixo disso = sem fonte clara (ruído puro dá ~0.2)
constexpr float GOOD_COHERENCE = 0.65f;    // pico a partir do qual a medida vale peso cheio
// Rastreador (Kalman 1D) do seno do ângulo: 0 = frente, +1 = direita, -1 = esquerda
constexpr float TRACK_Q        = 0.0006f;  // quanto a fonte pode andar por bloco (variância): maior = segue mais rápido
constexpr float MEAS_SIGMA     = 0.12f;    // erro de uma medida com coerência boa (em seno)
constexpr float GATE_SIGMAS    = 3.0f;     // medida mais longe que isso é tratada como eco/salto
constexpr int   JUMP_AFTER     = 5;        // ...a não ser que se repita: 5 blocos (80 ms) = fonte nova

constexpr int NFFT = 2 * BLOCK;   // zero-padding: correlação linear, não circular
constexpr int K_LO = static_cast<int>(GCC_LO_HZ * NFFT / SAMPLE_RATE);
constexpr int K_HI = static_cast<int>(GCC_HI_HZ * NFFT / SAMPLE_RATE);
static_assert(K_HI < BLOCK, "espectro cruzado guardado só até BLOCK");

float s_cos[NFFT / 2], s_sin[NFFT / 2], s_win[BLOCK];
float s_lre[NFFT], s_lim[NFFT], s_rre[NFFT], s_rim[NFFT];   // rascunho da FFT (fora da pilha)

inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

void tables_init()
{
    constexpr float PI = 3.14159265f;
    for (int k = 0; k < NFFT / 2; ++k) {
        s_cos[k] = cosf(2.0f * PI * k / NFFT);
        s_sin[k] = sinf(2.0f * PI * k / NFFT);
    }
    for (int i = 0; i < BLOCK; ++i)   // Hann
        s_win[i] = 0.5f - 0.5f * cosf(2.0f * PI * i / (BLOCK - 1));
}

// FFT radix-2 in-place (inverse sem o fator 1/N)
void fft(float *re, float *im, bool inverse)
{
    for (int i = 1, j = 0; i < NFFT; ++i) {
        int bit = NFFT >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap(re[i], re[j]); std::swap(im[i], im[j]); }
    }
    for (int len = 2; len <= NFFT; len <<= 1) {
        const int half = len / 2, step = NFFT / len;
        for (int i = 0; i < NFFT; i += len) {
            for (int k = 0; k < half; ++k) {
                const float wr = s_cos[k * step];
                const float wi = inverse ? s_sin[k * step] : -s_sin[k * step];
                const int a = i + k, b = a + half;
                const float tr = re[b] * wr - im[b] * wi;
                const float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;  im[b] = im[a] - ti;
                re[a] += tr;         im[a] += ti;
            }
        }
    }
}

}  // namespace

Tracker::Tracker() { tables_init(); }

// Um bloco de BLOCK amostras (já filtradas). active = há som acima do fundo.
void Tracker::doa_update(const float *l, const float *r, bool active)
{
    float *const lre = s_lre, *const lim = s_lim, *const rre = s_rre, *const rim = s_rim;

    P_ = fminf(P_ + TRACK_Q, 1.0f);   // predição: a fonte pode ter andado
    if (!active) {
        for (int k = K_LO; k <= K_HI; ++k) { xspec_r_[k] *= XSPEC_DECAY; xspec_i_[k] *= XSPEC_DECAY; }
        peak_ *= PEAK_DECAY;
        prev_e_ = 0.0f;   // o próximo som conta como começo
        return;
    }

    // Peso do bloco pela posição no envelope: subindo/no pico = som direto
    float e = 0.0f;
    for (int i = 0; i < BLOCK; ++i) e += l[i] * l[i] + r[i] * r[i];
    peak_ = fmaxf(e, peak_ * PEAK_DECAY);
    float w = (e / (peak_ + 1e-20f)) * (e / (peak_ + 1e-20f));
    if (e > 4.0f * prev_e_) w *= ONSET_BOOST;
    prev_e_ = e;
    if (w < MIN_WEIGHT) return;   // cauda de reverberação: nem acumula nem mede

    for (int i = 0; i < NFFT; ++i) {
        const bool in = i < BLOCK;
        lre[i] = in ? l[i] * s_win[i] : 0.0f;
        rre[i] = in ? r[i] * s_win[i] : 0.0f;
        lim[i] = rim[i] = 0.0f;
    }
    fft(lre, lim, false);
    fft(rre, rim, false);
    for (int k = K_LO; k <= K_HI; ++k) {
        xspec_r_[k] = XSPEC_ALPHA * xspec_r_[k] + w * (lre[k] * rre[k] + lim[k] * rim[k]);
        xspec_i_[k] = XSPEC_ALPHA * xspec_i_[k] + w * (lim[k] * rre[k] - lre[k] * rim[k]);
    }

    // PHAT sobre a média; IFFT(X)[k] = sum l[n+k] r[n] -> pico em k > 0 = som da direita
    for (int k = 0; k < NFFT; ++k) lre[k] = lim[k] = 0.0f;
    for (int k = K_LO; k <= K_HI; ++k) {
        const float mag = sqrtf(xspec_r_[k] * xspec_r_[k] + xspec_i_[k] * xspec_i_[k]) + 1e-20f;
        lre[k] = xspec_r_[k] / mag;     lim[k] = xspec_i_[k] / mag;
        lre[NFFT - k] = lre[k];         lim[NFFT - k] = -lim[k];
    }
    fft(lre, lim, true);

    int best = 0;
    float peak = -1e30f;
    for (int k = -MAX_LAG; k <= MAX_LAG; ++k) {
        const float v = lre[(k + NFFT) % NFFT];
        if (v > peak) { peak = v; best = k; }
    }
    float delta = 0.0f;   // interpolação parabólica: resolução abaixo de 1 amostra
    if (best > -MAX_LAG && best < MAX_LAG) {
        const float y0 = lre[(best - 1 + NFFT) % NFFT];
        const float y2 = lre[(best + 1 + NFFT) % NFFT];
        const float den = y0 - 2.0f * peak + y2;
        if (den < 0.0f) delta = clampf(0.5f * (y0 - y2) / den, -0.5f, 0.5f);
    }
    out_.tdoa      = best + delta - cfg.tdoa_trim;
    out_.coherence = peak / (2.0f * (K_HI - K_LO + 1));   // 1 = fases perfeitamente alinhadas

    // Kalman: medida z com erro proporcional a 1/confiança
    const float conf = clampf((out_.coherence - MIN_COHERENCE) / (GOOD_COHERENCE - MIN_COHERENCE), 0.0f, 1.0f);
    if (conf <= 0.0f) return;
    const float z  = clampf(out_.tdoa / TAU_MAX, -1.0f, 1.0f);
    const float Rm = (MEAS_SIGMA / conf) * (MEAS_SIGMA / conf);
    const float y  = z - s_;
    const float S  = P_ + Rm;
    if (y * y > GATE_SIGMAS * GATE_SIGMAS * S) {
        // Longe demais: eco isolado... ou a fonte mudou de lugar de verdade
        if (++outliers_ >= JUMP_AFTER) { s_ = z; P_ = Rm; outliers_ = 0; }
        return;
    }
    outliers_ = 0;
    const float K = P_ / S;
    s_ = clampf(s_ + K * y, -1.0f, 1.0f);
    P_ *= 1.0f - K;
}

const Output &Tracker::process(const int32_t *buf, int frames)
{
    constexpr float FS = 8388608.0f;   // 2^23 = full scale de 24 bits
    // Passa-altas de 1ª ordem: tira o DC do INMP441 e o grave, que tem
    // comprimento de onda longo demais para dar fase útil a 15 cm
    constexpr float RC     = 1.0f / (2.0f * 3.14159265f * HPF_HZ);
    constexpr float DT     = 1.0f / SAMPLE_RATE;
    constexpr float HPF_A  = RC / (RC + DT);
    constexpr float POL_RC = 1.0f / (2.0f * 3.14159265f * 80.0f);
    constexpr float POL_A  = POL_RC / (POL_RC + DT);

    frames = std::min(frames, BLOCK);
    if (frames <= 0) return out_;

    float acc_l = 0.0f, acc_r = 0.0f;
    float pxy = 0.0f, pxx = 0.0f, pyy = 0.0f;
    for (int i = 0; i < frames; ++i) {
        const float l = static_cast<float>(buf[2 * i]     >> 8);
        const float r = static_cast<float>(buf[2 * i + 1] >> 8);
        yl_ = HPF_A * (yl_ + l - xl_);  xl_ = l;
        yr_ = HPF_A * (yr_ + r - xr_);  xr_ = r;
        // Polaridade: em 80-300 Hz os dois mics têm que andar juntos. (l - yl) = abaixo
        // de 300 Hz; o passa-altas de 80 Hz tira DC e o ruído próprio de cada mic.
        const float ul = l - yl_, ur = r - yr_;
        ql_ = POL_A * (ql_ + ul - pl_);  pl_ = ul;
        qr_ = POL_A * (qr_ + ur - pr_);  pr_ = ur;
        pxy += ql_ * qr_;  pxx += ql_ * ql_;  pyy += qr_ * qr_;
        fl_l_[i] = yl_;
        fl_r_[i] = yr_;
        acc_l += yl_ * yl_;
        acc_r += yr_ * yr_;
    }
    pol_xy_ = 0.98f * pol_xy_ + pxy;  pol_xx_ = 0.98f * pol_xx_ + pxx;  pol_yy_ = 0.98f * pol_yy_ + pyy;
    out_.polarity = pol_xy_ / (sqrtf(pol_xx_ * pol_yy_) + 1e-20f);

    const float L = sqrtf(acc_l / frames) / FS;
    const float R = sqrtf(acc_r / frames) / FS;
    out_.level_l = L;
    out_.level_r = R;

    // Ruído de fundo: desce rápido, sobe devagar (~80 s) -> se adapta à sala
    // sem "aprender" uma voz, ou uma fonte tocando sem parar, como fundo
    auto track = [](float &fl, float v) {
        if (fl < 0.0f)  fl = v;
        else if (v < fl) fl += 0.05f   * (v - fl);
        else             fl += 0.0002f * (v - fl);
    };
    track(floor_l_, L);
    track(floor_r_, R);
    const float fl = fmaxf(floor_l_, floor_r_);
    out_.floor = fl;

    const float loud = fmaxf(L, R);
    out_.active = frames == BLOCK && loud > MIN_LEVEL && loud > fl * powf(10.0f, cfg.trigger_db / 20.0f);
    if (frames == BLOCK) doa_update(fl_l_, fl_r_, out_.active);
    out_.s     = s_;
    out_.sigma = sqrtf(P_);
    return out_;
}

}  // namespace ssl
