#pragma once
#include "ao.hpp"

namespace storage {

// Acha a partição "anims" e monta o índice das animações gravadas (app::init antes).
// Sem a partição (tabela antiga gravada), só os parâmetros na NVS funcionam.
ao::ActiveObject *create();

}  // namespace storage
