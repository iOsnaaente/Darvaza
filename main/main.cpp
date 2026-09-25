/*
 * Darvaza — 2x GC9A01 (olhos) + 2x INMP441 (localização do som) no ESP32-S3,
 * comandado por um host (Raspberry Pi / PC) pela UART.
 *
 * Arquitetura: Active Objects (components/ao). Cada subsistema é uma task com
 * fila de eventos própria e estado privado; ninguém mexe no estado do outro,
 * só posta eventos. Os comandos da UART são roteados por tabela (comm.cpp).
 *
 *   comm     UART: roteia comandos, único que escreve (RSP/EVT/LOG/AUDIO)
 *   audio    I2S + GCC-PHAT + Kalman -> mailbox do SSL, EVT_SOUND, stream
 *   display  render dos olhos a 30 fps: automático ou animação
 *   storage  dono da flash: animações (partição "anims") e parâmetros (NVS)
 *
 * Protocolo: components/proto/include/proto.hpp (espelho em tools/proto.py)
 * Requer ESP-IDF >= 5.1 e espressif/esp_lcd_gc9a01 (ver main/idf_component.yml)
 */
#include "esp_log.h"
#include "esp_system.h"

#include "app.hpp"
#include "audio.hpp"
#include "comm.hpp"
#include "display.hpp"
#include "storage.hpp"

static const char *TAG = "main";

// Streaming do áudio pela serial (UART0 / CH343) para ouvir no PC: tools/ouvir_mics.py
// O idf.py monitor a 115200 deixa de ler o log: use tools/darvaza_cli.py monitor.
constexpr uint32_t   UART_BAUD    = 2000000;   // 64 kB/s de áudio = ~32% da banda
constexpr BaseType_t AUDIO_CORE   = 0;
constexpr BaseType_t DISPLAY_CORE = 1;         // task de render E ISRs do SPI dos LCDs

extern "C" void app_main(void)
{
    // Primeiro a UART: todo log daqui em diante já sai em frames LOG
    app::init();
    comm::Config uart;
    uart.baud = UART_BAUD;
    app::comm = comm::create(uart);

    // Travou e reiniciou sozinho? O motivo aparece aqui (BROWNOUT = queda de tensão)
    static const char *const REASONS[] = {
        "UNKNOWN", "POWERON", "EXT", "SW", "PANIC", "INT_WDT",
        "TASK_WDT", "WDT", "DEEPSLEEP", "BROWNOUT", "SDIO"
    };
    const esp_reset_reason_t rr = esp_reset_reason();
    ESP_LOGW(TAG, "reset: %s", rr < sizeof(REASONS) / sizeof(REASONS[0]) ? REASONS[rr] : "?");

    app::storage = storage::create();
    app::audio   = audio::create();
    app::display = display::create(DISPLAY_CORE);

    // Todos os endereços prontos: agora as tasks podem conversar
    //                  prio  pilha  core
    app::audio->start(  10,   6144,  AUDIO_CORE);
    app::comm->start(    8,   4096,  0);
    app::display->start( 5,   6144,  DISPLAY_CORE);   // printf de float come pilha
    app::storage->start( 3,   4096,  0);
}
