#!/usr/bin/env bash
# Compila, manda e controla o face_app na placa.
# Uso: scripts/face_app.sh <comando>
#   build     compila e monta build/face_app_deploy
#   deploy    build + para o app + copia para /root/face_app na placa
#   start     inicia em segundo plano (saída em /tmp/face_app.log, na placa)
#   stop      para
#   restart   stop + start
#   monitor   segue o terminal do app: [ID,X,Y,dX,dY,time_detected], "#" e avisos
#   run       roda em primeiro plano, com a saída inteira (Ctrl+C para)
#   web       pelo adb: porta 80 da placa em http://127.0.0.1:8080/ (WSL e Windows)
# Opções do app vão em FACE_APP_ARGS (ex.: FACE_APP_ARGS="--hz 50 --thr 0.55").
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/../env.sh"

APP_DIR=/root/face_app
LOG=/tmp/face_app.log
ARGS=${FACE_APP_ARGS:-}

board() {
    if [ "$BOARD_LINK" = adb ]; then
        adb shell "$*"
    else
        ssh "$BOARD_USER@$BOARD" "$*"
    fi
}

case "${1:-}" in
build)
    cmake -S "$RV1106_ROOT/face_app" -B "$RV1106_ROOT/build/face_app" \
        -DCMAKE_TOOLCHAIN_FILE="$RV1106_ROOT/cmake/rv1106.cmake" \
        -DCMAKE_INSTALL_PREFIX="$RV1106_ROOT/build/face_app_deploy" >/dev/null
    cmake --build "$RV1106_ROOT/build/face_app" -j"$(nproc)"
    rm -rf "$RV1106_ROOT/build/face_app_deploy"
    cmake --install "$RV1106_ROOT/build/face_app" >/dev/null
    echo "pronto: build/face_app_deploy"
    ;;
deploy)
    "$0" build
    "$0" stop
    "$RV1106_ROOT/scripts/deploy.sh" "$RV1106_ROOT/build/face_app_deploy" "$APP_DIR"
    ;;
start)
    # O "sleep" segura a sessão do adb até o app se desligar dela (setsid);
    # sem ele, o app morre junto quando o adb shell termina.
    board "cd $APP_DIR && setsid ./face_app $ARGS >> $LOG 2>&1 < /dev/null & sleep 2;
           pidof face_app >/dev/null && echo 'face_app rodando (saída em $LOG)' || { echo 'face_app não subiu:'; tail -20 $LOG; }"
    ;;
stop)
    board "killall face_app 2>/dev/null; sleep 1; true"
    ;;
restart)
    "$0" stop
    "$0" start
    ;;
monitor)
    board "tail -n 30 -f $LOG" | sed -u 's/\r$//' | grep --line-buffered -E '^\[|^#|^[IWE] \('
    ;;
run)
    "$0" stop
    board "cd $APP_DIR && ./face_app $ARGS"
    ;;
web)
    # O adb escuta só em 127.0.0.1. Com a rede espelhada da WSL o navegador do
    # Windows chega nele por 127.0.0.1, mas não por "localhost" (que o Windows
    # tenta primeiro em IPv6, ::1, e a WSL não repassa).
    adb forward tcp:8080 tcp:80 >/dev/null
    echo "abra http://127.0.0.1:8080/ (no navegador do Windows ou da WSL)"
    ;;
*)
    sed -n '2,12p' "$0"
    exit 1
    ;;
esac
