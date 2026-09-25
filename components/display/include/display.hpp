#pragma once
#include "ao.hpp"

namespace display {

// Inicializa backlight e os dois GC9A01 e lê o brilho da NVS (app::init antes).
// A task tem que rodar no core `core` (a ISR do SPI é presa nele).
ao::ActiveObject *create(BaseType_t core);

}  // namespace display
