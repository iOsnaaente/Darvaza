#pragma once
/*
 * Direção do som (sem dependência do ESP-IDF: testável no PC)
 *  1) GCC-PHAT sobre o espectro cruzado MÉDIO: soma L·conj(R) de vários blocos
 *     antes de normalizar. O som direto soma sempre na mesma fase; ruído e ecos
 *     variam de bloco para bloco e se cancelam. Um bloco de 16 ms sozinho é
 *     dominado por eles (coerência ~0.15 e atraso batendo no limite).
 *     Só entram blocos no começo/pico de cada som (precedência): na cauda,
 *     os ecos das paredes dominam e apontariam para elas.
 *  2) Filtro de Kalman 1D no seno do ângulo: acompanha a fonte andando, pesa
 *     cada medida pela coerência e ignora saltos isolados (ecos).
 */
#include <cstdint>

namespace ssl {

constexpr uint32_t SAMPLE_RATE = 16000;
constexpr int      BLOCK       = 256;   // quadros por bloco: 16 ms

struct Config {
    float trigger_db = 6.0f;   // quanto acima do ruído de fundo conta como som
    float tdoa_trim  = 0.0f;   // atraso (amostras) medido com o som bem à frente -> zera o centro
};

struct Output {
    float level_l   = 0.0f;   // RMS filtrado, 0..1 do full scale
    float level_r   = 0.0f;
    float floor     = 0.0f;   // ruído de fundo (maior dos dois canais)
    float s         = 0.0f;   // seno do ângulo estimado (-1 esq .. +1 dir)
    float sigma     = 1.0f;   // incerteza do rastreador (desvio, em seno)
    float tdoa      = 0.0f;   // último atraso medido com som (amostras, + = direita primeiro)
    float coherence = 0.0f;   // altura do pico do GCC-PHAT nessa medida (0..1)
    float polarity  = 0.0f;   // correlação L/R abaixo de 300 Hz: ~+1 ok, ~-1 invertido
    bool  active    = false;  // este bloco tem som acima do fundo
};

class Tracker {
public:
    Tracker();
    Config cfg;

    // buf: quadros (L,R) int32 intercalados, 24 bits alinhados à esquerda,
    // polaridade do mic direito já corrigida
    const Output &process(const int32_t *buf, int frames);
    const Output &out() const { return out_; }

private:
    void doa_update(const float *l, const float *r, bool active);

    // Passa-altas e medidor de polaridade
    float xl_ = 0, xr_ = 0, yl_ = 0, yr_ = 0;
    float pl_ = 0, pr_ = 0, ql_ = 0, qr_ = 0, pol_xy_ = 0, pol_xx_ = 0, pol_yy_ = 0;
    float floor_l_ = -1.0f, floor_r_ = -1.0f;   // ruído de fundo por canal (RMS)
    float fl_l_[BLOCK], fl_r_[BLOCK];           // bloco filtrado para o GCC

    // Estado do GCC-PHAT + Kalman
    float xspec_r_[BLOCK] = {}, xspec_i_[BLOCK] = {};   // espectro cruzado médio L·conj(R) (só K_LO..K_HI)
    float s_ = 0.0f, P_ = 1.0f;                 // Kalman: seno do ângulo e variância
    float peak_ = 0.0f, prev_e_ = 0.0f;         // energia: pico recente e bloco anterior
    int   outliers_ = 0;

    Output out_;
};

}  // namespace ssl
