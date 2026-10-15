#pragma once
/*
 * Protocolo da UART (fonte da verdade; espelhado em tools/proto.py).
 *
 * Frame (little-endian):
 *   A5 5A | ch u8 | op u8 | seq u16 | len u16 | payload (len bytes) | crc16 u16
 *   crc16 = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF) de ch até o fim do payload
 *
 * Canais:
 *   CMD   host -> ESP   comando; op = Op, seq escolhido pelo host
 *   RSP   ESP -> host   resposta: mesmo op e seq do comando; payload[0] = Status
 *   EVT   ESP -> host   notificação espontânea; op = Evt
 *   AUDIO ESP -> host   PCM 16 kHz estéreo i16 (L,R intercalado); seq conta blocos
 *                       (buraco no seq = bloco perdido)
 *   LOG   ESP -> host   texto do ESP_LOG; é um fluxo: junte os payloads e quebre em '\n'
 *
 * Sem dependência do ESP-IDF: compila e testa no PC.
 */
#include <cstddef>
#include <cstdint>

namespace proto {

constexpr uint8_t VERSION     = 1;
constexpr uint8_t SOF0        = 0xA5;
constexpr uint8_t SOF1        = 0x5A;
constexpr size_t  HEADER      = 8;      // SOF(2) + ch + op + seq(2) + len(2)
constexpr size_t  TRAILER     = 2;      // crc16
constexpr size_t  MAX_PAYLOAD = 1040;   // cabe um chunk de upload (4 + 1024) e um bloco de áudio (1024)
constexpr size_t  MAX_FRAME   = HEADER + MAX_PAYLOAD + TRAILER;
constexpr size_t  CHUNK_MAX   = 1024;   // dados por UPLOAD_CHUNK

enum Channel : uint8_t { CH_CMD = 1, CH_RSP = 2, CH_EVT = 3, CH_AUDIO = 4, CH_LOG = 5 };

// Comandos (host -> ESP) e o payload de cada um
enum Op : uint8_t {
    OP_PING         = 0x01,   // -               -> [versão u8]
    OP_STATS        = 0x02,   // -               -> Stats
    OP_RESET        = 0x03,   // -               -> OK e reinicia
    OP_ANIM_PLAY    = 0x10,   // AnimId          -> OK | NOT_FOUND | BAD_ARG (0 = automático)
    OP_BACKLIGHT    = 0x11,   // Backlight       -> OK | BAD_ARG
    OP_STREAM       = 0x20,   // Stream          -> OK
    OP_AUDIO_CFG    = 0x21,   // Cfg             -> OK | BAD_ARG
    OP_UPLOAD_BEGIN = 0x30,   // UploadBegin     -> OK | BAD_ARG | NO_SPACE
    OP_UPLOAD_CHUNK = 0x31,   // offset u32+dados -> OK [recebidos u32] | BAD_ARG [esperado u32] | BAD_STATE
    OP_UPLOAD_END   = 0x32,   // -               -> OK | BAD_CRC | BAD_STATE
    OP_ANIM_LIST    = 0x33,   // -               -> OK [n u16, ids u16 x n]
    OP_ANIM_DELETE  = 0x34,   // AnimId          -> OK | NOT_FOUND
};

// Eventos (ESP -> host, canal EVT)
enum Evt : uint8_t {
    EVT_BOOT      = 0x80,   // EvtBoot: o ESP (re)iniciou
    EVT_SOUND     = 0x81,   // EvtSound: som acima do fundo (no começo e a cada 100 ms)
    EVT_ANIM_DONE = 0x82,   // AnimId: animação sem loop terminou
};

// Primeiro byte de toda resposta
enum Status : uint8_t {
    ST_OK         = 0,
    ST_UNKNOWN_OP = 1,
    ST_BAD_LEN    = 2,
    ST_BUSY       = 3,   // fila ou memória cheia: tente de novo
    ST_BAD_ARG    = 4,
    ST_NOT_FOUND  = 5,
    ST_NO_SPACE   = 6,
    ST_BAD_STATE  = 7,
    ST_BAD_CRC    = 8,
    ST_STORAGE    = 9,   // erro de flash/NVS
};

// Parâmetros ajustáveis (OP_AUDIO_CFG); ficam gravados na NVS
enum CfgKey : uint8_t {
    CFG_TRIGGER_DB   = 1,   // dB acima do fundo que contam como som (0..40)
    CFG_MIC_POLARITY = 2,   // +1 ou -1 (mic direito invertido)
    CFG_TDOA_TRIM    = 3,   // atraso (amostras) medido com o som bem à frente (-8..8)
    CFG_STREAM_SHIFT = 4,   // 24 -> 16 bit: 8 = sem ganho, 7 = +6 dB... (0..8)
    CFG_BACKLIGHT    = 5,   // interno: último OP_BACKLIGHT (0..100)
};

// ---------------------------------------------------------------------------
// Payloads
// ---------------------------------------------------------------------------
#pragma pack(push, 1)
struct AnimId      { uint16_t id; };
struct Backlight   { uint8_t percent; };
struct Stream      { uint8_t on; };
struct Cfg         { uint8_t key; float value; };
struct UploadBegin { uint16_t id; uint32_t size; uint32_t crc32; };   // crc32 = zlib.crc32 do arquivo
struct EvtBoot     { uint8_t proto; uint8_t reset_reason; };          // esp_reset_reason_t
struct EvtSound    { float s; float sigma; float coherence; float level_db; };   // s = seno do ângulo (+ = direita)

struct StatsAo     { uint16_t stack_free; uint8_t queued; uint8_t queue_peak; uint16_t dropped; };
struct Stats {
    uint32_t uptime_ms, heap_free, heap_min, blob_bytes;
    uint16_t rx_frames, rx_bad, audio_drop, log_drop;
    StatsAo  ao[4];   // comm, audio, display, storage
};
#pragma pack(pop)
static_assert(sizeof(Cfg) == 5 && sizeof(UploadBegin) == 10 && sizeof(EvtSound) == 16 && sizeof(Stats) == 48,
              "layout do protocolo mudou: atualize tools/proto.py");

// ---------------------------------------------------------------------------
uint16_t crc16(const uint8_t *p, size_t n, uint16_t crc = 0xFFFF);   // CRC-16/CCITT-FALSE
uint32_t crc32(const uint8_t *p, size_t n, uint32_t crc = 0);        // = zlib.crc32 (encadeável)

// Monta o frame em out (>= HEADER + len + TRAILER). Retorna o tamanho total.
size_t encode(uint8_t *out, Channel ch, uint8_t op, uint16_t seq, const void *payload, uint16_t len);
// Para escrever o payload direto no buffer: payload_of(out) aponta onde escrever,
// seal(out, ch, op, seq, len) preenche cabeçalho e CRC e retorna o tamanho total.
inline uint8_t *payload_of(uint8_t *out) { return out + HEADER; }
size_t seal(uint8_t *out, Channel ch, uint8_t op, uint16_t seq, uint16_t len);

struct Frame {
    uint8_t        ch, op;
    uint16_t       seq, len;
    const uint8_t *payload;   // válido até a próxima chamada de feed
};

// Parser incremental: um byte por vez; lixo entre frames é ignorado
class Parser {
public:
    bool feed(uint8_t b, Frame &out);   // true = fechou um frame válido
    void reset() { st_ = S_SOF0; }
    uint32_t bad() const { return bad_; }   // CRC ou tamanho inválido

private:
    enum State : uint8_t { S_SOF0, S_SOF1, S_HDR, S_BODY } st_ = S_SOF0;
    uint8_t  buf_[MAX_FRAME];
    size_t   n_   = 0;
    size_t   end_ = 0;   // tamanho total do frame em curso
    uint32_t bad_ = 0;
};

}  // namespace proto
