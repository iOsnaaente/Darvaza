#pragma once
/*
 * Cola da aplicação: sinais, endereços dos AOs e os canais que não são fila
 * de eventos (mailbox do SSL). Todos os AOs dependem só disto, não uns dos outros.
 */
#include <cstddef>
#include <cstdint>

#include "ao.hpp"
#include "proto.hpp"

namespace app {

enum Sig : uint16_t {
    // Comandos do host (CommRx -> AO dono; trazem seq/op para a resposta)
    SIG_PING = 1,
    SIG_STATS,
    SIG_RESET,
    SIG_ANIM_PLAY,      // -> display
    SIG_BACKLIGHT,      // -> display
    SIG_STREAM,         // -> audio
    SIG_AUDIO_CFG,      // -> audio
    SIG_UPLOAD_BEGIN,   // -> storage
    SIG_UPLOAD_CHUNK,   // -> storage (blob: offset u32 + dados)
    SIG_UPLOAD_END,     // -> storage
    SIG_ANIM_LIST,      // -> storage
    SIG_ANIM_DELETE,    // -> storage

    // Internos
    SIG_RSP,            // qualquer AO -> comm: resposta a um comando (data[0] = Status)
    SIG_EVT,            // qualquer AO -> comm: notificação para o host (op = Evt)
    SIG_LOG_READY,      // hook do log -> comm (campainha; o texto está no buffer de log)
    SIG_AUDIO_READY,    // audio -> comm (campainha; o PCM está no buffer de áudio)
    SIG_ANIM_LOAD,      // display -> storage: carregar animação gravada (AnimId)
    SIG_ANIM_LOADED,    // storage -> display: data[0] = Status, blob = animação
    SIG_CFG_SAVE,       // audio/display -> storage: gravar proto::Cfg na NVS
};

// Endereços dos AOs: app_main preenche antes de dar start em qualquer um
extern ao::ActiveObject *comm;
extern ao::ActiveObject *audio;
extern ao::ActiveObject *display;
extern ao::ActiveObject *storage;

void init();   // NVS + mailbox do SSL; chamar primeiro

// Parâmetros persistentes (NVS). Ler: qualquer task, em geral só na criação do
// AO. Gravar: só o storage (flash tem um dono), os outros pedem com save_cfg.
float cfg_get(proto::CfgKey key, float def);
bool  cfg_put(proto::CfgKey key, float value);

// Resposta ao comando req (pelo comm, único que escreve na UART). data vai
// logo depois do status: até 19 bytes aqui; mais que isso, use reply_blob.
void reply(const ao::Event &req, proto::Status st, const void *data = nullptr, size_t len = 0);
void reply_blob(const ao::Event &req, proto::Status st, ao::Blob *payload);   // o blob vai junto
// Notificação espontânea para o host (canal EVT)
void notify(proto::Evt evt, const void *data = nullptr, size_t len = 0);
// Pede ao storage para gravar um parâmetro (fire-and-forget)
void save_cfg(proto::CfgKey key, float value);

// ---------------------------------------------------------------------------
// Mailbox do SSL: o áudio publica o estado a cada bloco (16 ms), quem quiser lê
// o último valor. Dado contínuo de alta taxa: fila de 1 com sobrescrita, não evento.
// ---------------------------------------------------------------------------
struct SslState {
    float   level_l   = 0.0f;   // RMS filtrado, 0..1 do full scale
    float   level_r   = 0.0f;
    float   floor     = 0.0f;   // ruído de fundo (maior dos dois canais)
    float   s         = 0.0f;   // seno do ângulo estimado (-1 esq .. +1 dir)
    float   sigma     = 1.0f;   // incerteza do rastreador (desvio, em seno)
    float   tdoa      = 0.0f;   // último atraso medido (amostras, + = direita primeiro)
    float   coherence = 0.0f;   // altura do pico do GCC-PHAT (0..1)
    float   polarity  = 0.0f;   // correlação L/R abaixo de 300 Hz: ~+1 ok, ~-1 invertido
    int64_t sound_ms  = -1;     // instante do último som acima do fundo
};
void ssl_publish(const SslState &s);
bool ssl_read(SslState &s);   // false = o áudio ainda não publicou nada

}  // namespace app
