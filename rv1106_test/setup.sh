#!/usr/bin/env bash
# Baixa o que o ambiente da Luckfox Pico Max (RV1106) precisa:
#   sdk/luckfox-pico            só o toolchain do SDK (sparse checkout, ~320 MB)
#   third_party/luckfox_pico_rknn_example   RetinaFace + MobileFaceNet (reconhecimento)
#   third_party/luckfox_pico_rkmpi_example  câmera -> RTSP com detecção de rosto
# Os commits são fixos para o ambiente ser reproduzível. Rodar de novo não baixa
# o que já está no commit certo.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SDK_REV=824b817f889c2cbff1d48fcdb18ab494a68f69d1
RKNN_EXAMPLE_REV=16ef54a2ad56211287d6064d5a5cd52570f50746
RKMPI_EXAMPLE_REV=55178250c05542b156ac94c8c08cecef46589abf

# fetch <pasta> <url> <commit> [caminhos do sparse checkout...]
fetch() {
    local dir=$1 url=$2 rev=$3
    shift 3
    if [ "$(git -C "$dir" rev-parse HEAD 2>/dev/null)" = "$rev" ]; then
        echo "ok: ${dir#"$ROOT"/} já está em ${rev:0:7}"
        return
    fi
    echo "baixando ${dir#"$ROOT"/} (${rev:0:7})..."
    if [ ! -d "$dir/.git" ]; then
        git init -q "$dir"
        git -C "$dir" remote add origin "$url"
    fi
    if [ $# -gt 0 ]; then
        git -C "$dir" sparse-checkout set "$@"
        git -C "$dir" fetch -q --depth 1 --filter=blob:none origin "$rev"
    else
        git -C "$dir" fetch -q --depth 1 origin "$rev"
    fi
    git -C "$dir" checkout -q --detach FETCH_HEAD
}

fetch "$ROOT/sdk/luckfox-pico" https://github.com/LuckfoxTECH/luckfox-pico.git \
    "$SDK_REV" tools/linux/toolchain
fetch "$ROOT/third_party/luckfox_pico_rknn_example" \
    https://github.com/LuckfoxTECH/luckfox_pico_rknn_example.git "$RKNN_EXAMPLE_REV"
fetch "$ROOT/third_party/luckfox_pico_rkmpi_example" \
    https://github.com/LuckfoxTECH/luckfox_pico_rkmpi_example.git "$RKMPI_EXAMPLE_REV"

source "$ROOT/env.sh"
"${CROSS_COMPILE}gcc" --version | head -1
echo "pronto. Em cada terminal novo: source env.sh"
