#pragma once
#include "ao.hpp"

namespace audio {

// Configura o I2S dos INMP441 e lê os parâmetros da NVS (app::init antes)
ao::ActiveObject *create();

}  // namespace audio
