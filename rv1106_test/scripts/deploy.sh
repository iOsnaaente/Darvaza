#!/usr/bin/env bash
# Copia uma pasta para a placa, por adb ou ssh conforme BOARD_LINK (ver env.sh).
# Uso: scripts/deploy.sh <pasta_local> [pasta_na_placa]   (padrão: /root/<nome da pasta>)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

src=${1:?uso: scripts/deploy.sh <pasta_local> [pasta_na_placa]}
dst=${2:-/root/$(basename "$(realpath "$src")")}

if [ "$BOARD_LINK" = adb ]; then
    adb shell "mkdir -p '$dst'"
    adb push "$src/." "$dst/"
    echo "copiado para $dst (adb)"
else
    # tar pelo pipe: não depende de scp/sftp na placa.
    tar -C "$src" -cf - . | ssh "$BOARD_USER@$BOARD" "mkdir -p '$dst' && tar -C '$dst' -xf -"
    echo "copiado para $BOARD_USER@$BOARD:$dst (ssh)"
fi
