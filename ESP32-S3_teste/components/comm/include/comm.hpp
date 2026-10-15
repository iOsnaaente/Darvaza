#pragma once
#include <cstdint>

#include "ao.hpp"
#include "driver/uart.h"

namespace comm {

struct Config {
    uart_port_t port   = UART_NUM_0;
    uint32_t    baud   = 2000000;
    int         tx_pin = UART_PIN_NO_CHANGE;   // UART0: pinos padrão (CH343)
    int         rx_pin = UART_PIN_NO_CHANGE;
};

// Instala a UART e passa o ESP_LOG para frames LOG. Chamar antes de tudo em
// app_main: o que for logado depois já sai no protocolo.
ao::ActiveObject *create(const Config &cfg);

// Áudio -> host: copia um bloco (L,R intercalado) para o buffer de TX e toca a
// campainha do comm. Não bloqueia; false = buffer cheio, bloco perdido (o host
// vê o buraco no seq). Só a task de áudio chama.
bool audio_push(uint16_t seq, const int16_t *lr, int frames);

}  // namespace comm
