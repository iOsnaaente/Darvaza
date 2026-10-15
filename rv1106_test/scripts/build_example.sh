#!/usr/bin/env bash
# Compila um exemplo oficial da Luckfox sem o menu interativo do build.sh deles
# (sempre uclibc, que é o que a imagem Buildroot usa).
# Uso: scripts/build_example.sh <exemplo> [-Dopção_cmake ...]
# Saída: third_party/<repo>/install/uclibc/<exemplo>_demo/  (pasta pronta para o deploy.sh)
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

repos=(luckfox_pico_rknn_example luckfox_pico_rkmpi_example)
name=${1:-}
if [ -z "$name" ]; then
    echo "uso: scripts/build_example.sh <exemplo> [-Dopção_cmake ...]"
    for repo in "${repos[@]}"; do
        echo "$repo:"
        ls "$RV1106_ROOT/third_party/$repo/example" | sed 's/^/  /'
    done
    exit 1
fi
shift

for repo in "${repos[@]}"; do
    dir=$RV1106_ROOT/third_party/$repo
    [ -d "$dir/example/$name" ] || continue
    rm -rf "$dir/build"
    cmake -S "$dir" -B "$dir/build" -DEXAMPLE_DIR="example/$name" -DEXAMPLE_NAME="$name" \
        -DLIBC_TYPE=uclibc "$@"
    cmake --build "$dir/build" -j"$(nproc)" --target install
    # O CMakeLists deles escolhe o nome da pasta (_demo, _pro_max_demo...); lê de volta.
    out=$(grep -m1 -o 'DESTINATION "[^"]*"' "$dir/build/cmake_install.cmake" | cut -d'"' -f2)
    # Os demos têm RPATH $ORIGIN/lib: o librknnmrt 1.6.0 dos modelos vai junto,
    # porque a imagem da placa pode ter um mais velho (a nossa tem 1.4.1).
    mkdir -p "$out/lib"
    cp "$dir/lib/uclibc/librknnmrt.so" "$out/lib/"
    echo "pronto: $out"
    exit 0
done
echo "exemplo não encontrado: $name (rode sem argumentos para ver a lista)" >&2
exit 1
