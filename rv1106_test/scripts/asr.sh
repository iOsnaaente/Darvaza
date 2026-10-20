#!/usr/bin/env bash
# Mede reconhecimento de fala na placa: whisper.cpp (tiny) e Vosk (small pt).
# Uso: scripts/asr.sh <comando>
#   setup        baixa whisper.cpp, modelos, Vosk, runtime glibc, qemu-arm e as
#                frases de teste (tudo em third_party/)
#   build        compila e monta build/asr_bench_deploy
#   deploy       build + copia para /root/asr_bench na placa (só o que mudou)
#   run [motor]  roda os motores nas frases de teste na placa, sem a página
#   qemu [motor] o mesmo no PC, emulado: confere que funciona (tempos não valem)
# A página: http://<placa>/asr (o face_app tem que estar rodando).
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

TP=$RV1106_ROOT/third_party
ASR=$TP/asr
DEPLOY=$RV1106_ROOT/build/asr_bench_deploy
APP_DIR=/root/asr_bench

WHISPER_TAG=v1.9.5
WHISPER_REV=d1be6fde11ac6e0407606b4e42fe72d34add8037
WHISPER_MODEL=ggml-tiny-q8_0.bin
WHISPER_MODEL_URL=https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/$WHISPER_MODEL
WHISPER_MODEL_SHA=c2085835d3f50733e2ff6e4b41ae8a2b8d8110461e18821b09a15c40c42d1cca
VOSK_LIB=vosk-linux-armv7l-0.3.45
VOSK_LIB_SHA=10b795ae478ef1d530fcbfbbea9ccbbbf3b7e7c244bd5fd3176f4a6af32f3c8c
VOSK_MODEL=vosk-model-small-pt-0.3
VOSK_MODEL_SHA=6e1ce909032e1afa7a88e68a3d628ecafff302bdf195befab308826c395e93b7

GLIBC=$ASR/glibc/root
GLIBC_ARCH=arm-linux-gnueabihf
GLIBC_RUNTIME="ld-linux-armhf.so.3 libc.so.6 libm.so.6 libstdc++.so.6 libgcc_s.so.1 libatomic.so.1"
QEMU=$ASR/qemu/root/usr/bin/qemu-arm-static
UCLIBC_SYSROOT=$RV1106_TOOLCHAIN/arm-rockchip830-linux-uclibcgnueabihf/sysroot

board() {
    if [ "$BOARD_LINK" = adb ]; then
        adb shell "$*"
    else
        ssh "$BOARD_USER@$BOARD" "$*"
    fi
}

# download <url> <arquivo> <sha256>
download() {
    if [ -f "$2" ] && echo "$3  $2" | sha256sum -c --quiet 2>/dev/null; then
        echo "ok: ${2#"$TP"/}"
        return
    fi
    echo "baixando ${2#"$TP"/}..."
    mkdir -p "$(dirname "$2")"
    curl -fL --progress-bar -o "$2.part" "$1"
    if ! echo "$3  $2.part" | sha256sum -c --quiet; then
        echo "SHA-256 não confere: $1" >&2
        exit 1
    fi
    mv "$2.part" "$2"
}

# Os motores do engines.tsv que batem com o filtro: "nome<TAB>comando" por linha.
engines() {
    grep -v '^#' "$RV1106_ROOT/asr_bench/engines.tsv" | grep -v '^$' | awk -F'\t' -v f="${1:-}" 'f == "" || $1 == f'
}

case "${1:-}" in
setup)
    if [ "$(git -C "$TP/whisper.cpp" rev-parse HEAD 2>/dev/null)" != "$WHISPER_REV" ]; then
        echo "baixando whisper.cpp $WHISPER_TAG..."
        rm -rf "$TP/whisper.cpp"
        git -c advice.detachedHead=false clone -q --depth 1 --branch "$WHISPER_TAG" \
            https://github.com/ggml-org/whisper.cpp.git "$TP/whisper.cpp"
        [ "$(git -C "$TP/whisper.cpp" rev-parse HEAD)" = "$WHISPER_REV" ] || { echo "whisper.cpp: commit inesperado" >&2; exit 1; }
    fi
    echo "ok: whisper.cpp $WHISPER_TAG"
    download "$WHISPER_MODEL_URL" "$ASR/models/$WHISPER_MODEL" "$WHISPER_MODEL_SHA"
    download "https://github.com/alphacep/vosk-api/releases/download/v0.3.45/$VOSK_LIB.zip" "$ASR/$VOSK_LIB.zip" "$VOSK_LIB_SHA"
    download "https://alphacephei.com/vosk/models/$VOSK_MODEL.zip" "$ASR/$VOSK_MODEL.zip" "$VOSK_MODEL_SHA"
    [ -f "$ASR/$VOSK_LIB/libvosk.so" ] || python3 -I -m zipfile -e "$ASR/$VOSK_LIB.zip" "$ASR"
    [ -d "$ASR/models/$VOSK_MODEL" ] || python3 -I -m zipfile -e "$ASR/$VOSK_MODEL.zip" "$ASR/models"
    python3 -I "$RV1106_ROOT/asr_bench/tools/debian_debs.py" armhf "$ASR/glibc" \
        libc6 libc6-dev linux-libc-dev libstdc++6 libgcc-s1 libatomic1
    [ -x "$QEMU" ] || python3 -I "$RV1106_ROOT/asr_bench/tools/debian_debs.py" amd64 "$ASR/qemu" qemu-user-static
    [ -f "$ASR/clips/fleurs-08.wav" ] || python3 -I "$RV1106_ROOT/asr_bench/tools/fleurs_clips.py" "$ASR/clips" 8
    echo "pronto. Agora: scripts/asr.sh build"
    ;;
build)
    cmake -S "$RV1106_ROOT/asr_bench" -B "$RV1106_ROOT/build/asr_bench" \
        -DCMAKE_TOOLCHAIN_FILE="$RV1106_ROOT/cmake/rv1106.cmake" \
        -DCMAKE_INSTALL_PREFIX="$DEPLOY" >/dev/null
    cmake --build "$RV1106_ROOT/build/asr_bench" -j"$(nproc)"
    rm -rf "$DEPLOY"
    cmake --install "$RV1106_ROOT/build/asr_bench" >/dev/null

    # asr_vosk: o GCC da Rockchip, mas contra a glibc do Debian (crt, headers
    # e libc.so do sysroot). O loader declarado é o da glibc; na placa ele é
    # chamado direto pelo vosk.sh.
    "${CROSS_COMPILE}gcc" -O2 -Wall -Wextra -mcpu=cortex-a7 --sysroot="$GLIBC" \
        -B"$GLIBC/usr/lib/$GLIBC_ARCH" -isystem "$GLIBC/usr/include/$GLIBC_ARCH" \
        -I"$RV1106_ROOT/asr_bench/src" -I"$ASR/$VOSK_LIB" \
        "$RV1106_ROOT/asr_bench/src/asr_vosk.c" \
        -L"$GLIBC/usr/lib/$GLIBC_ARCH" -L"$GLIBC/lib/$GLIBC_ARCH" -L"$ASR/$VOSK_LIB" -lvosk \
        -Wl,-rpath-link,"$GLIBC/lib/$GLIBC_ARCH:$GLIBC/usr/lib/$GLIBC_ARCH" \
        -Wl,--dynamic-linker=/lib/ld-linux-armhf.so.3 -static-libgcc \
        -o "$DEPLOY/bin/asr_vosk"
    "${CROSS_COMPILE}strip" "$DEPLOY/bin/asr_whisper" "$DEPLOY/bin/asr_vosk"

    mkdir -p "$DEPLOY/lib" "$DEPLOY/glibc" "$DEPLOY/models" "$DEPLOY/clips"
    cp "$ASR/$VOSK_LIB/libvosk.so" "$DEPLOY/lib/"
    for lib in $GLIBC_RUNTIME; do
        cp -L "$(find "$GLIBC" -name "$lib" -print -quit)" "$DEPLOY/glibc/"
    done
    # Modelos por hard link: não copia 90 MB a cada build, e a data fica a
    # mesma (o "adb push --sync" do deploy pula o que não mudou).
    cp -al "$ASR/models/$WHISPER_MODEL" "$ASR/models/$VOSK_MODEL" "$DEPLOY/models/"
    # Vosk "rápido": o mesmo modelo, no formato novo (só links, 0 MB), para ler
    # um conf/model.conf. No formato antigo do small-pt-0.3 a libvosk fixa a
    # busca em max-active 7000 / beam 13; o vosk-rapido.conf usa a dos modelos
    # pequenos novos do Vosk (3000 / 10).
    R="$DEPLOY/models/$VOSK_MODEL-rapido"
    mkdir -p "$R/am" "$R/graph/phones" "$R/conf"
    ln -s "../../$VOSK_MODEL/final.mdl" "$R/am/final.mdl"
    for f in HCLr.fst Gr.fst disambig_tid.int; do
        ln -s "../../$VOSK_MODEL/$f" "$R/graph/$f"
    done
    ln -s "../../../$VOSK_MODEL/word_boundary.int" "$R/graph/phones/word_boundary.int"
    ln -s "../../$VOSK_MODEL/mfcc.conf" "$R/conf/mfcc.conf"
    ln -s "../$VOSK_MODEL/ivector" "$R/ivector"
    cp "$RV1106_ROOT/asr_bench/vosk-rapido.conf" "$R/conf/model.conf"
    cp -a "$ASR/clips/." "$DEPLOY/clips/"
    echo "pronto: build/asr_bench_deploy ($(du -sh "$DEPLOY" | cut -f1))"
    ;;
deploy)
    "$0" build
    need=$(du -sk "$DEPLOY" | cut -f1)
    free=$(board "df -k / | tail -1" | awk '{print $4}')
    echo "na placa: $((free / 1024)) MB livres em /, o asr_bench ocupa $((need / 1024)) MB"
    if [ "$BOARD_LINK" = adb ]; then
        board "mkdir -p $APP_DIR"
        adb push --sync "$DEPLOY/." "$APP_DIR/"
    else
        "$RV1106_ROOT/scripts/deploy.sh" "$DEPLOY" "$APP_DIR"
    fi
    echo "copiado para $APP_DIR"
    ;;
run)
    engines "${2:-}" | while IFS=$'\t' read -r name cmd; do
        echo "== $name" >&2
        board "cd $APP_DIR && $cmd clips/*.wav" < /dev/null
    done
    ;;
qemu)
    [ -x "$QEMU" ] || { echo "sem qemu-arm: rode scripts/asr.sh setup" >&2; exit 1; }
    cd "$DEPLOY"
    engines "${2:-}" | while IFS=$'\t' read -r name cmd; do
        echo "== $name (qemu)" >&2
        case "$cmd" in
        *.sh\ *) ASR_EMU="$QEMU" sh $cmd clips/*.wav ;;
        *) "$QEMU" -L "$UCLIBC_SYSROOT" $cmd clips/*.wav ;;
        esac < /dev/null
    done
    ;;
*)
    sed -n '2,11p' "$0"
    exit 1
    ;;
esac
