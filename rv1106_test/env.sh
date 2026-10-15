# Prepara o shell para compilar para a Luckfox Pico Max (RV1106).
# Uso: source env.sh
# BOARD e BOARD_USER podem ser sobrescritos antes (ex.: BOARD=192.168.0.50 para Ethernet).

RV1106_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export RV1106_ROOT
export LUCKFOX_SDK_PATH="$RV1106_ROOT/sdk/luckfox-pico"
export RV1106_TOOLCHAIN="$LUCKFOX_SDK_PATH/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf"
export CROSS_COMPILE=arm-rockchip830-linux-uclibcgnueabihf-
case ":$PATH:" in
    *":$RV1106_TOOLCHAIN/bin:"*) ;;
    *) export PATH="$RV1106_TOOLCHAIN/bin:$PATH" ;;
esac

# Ligação com a placa: "adb" (USB passada para a WSL com usbipd) ou "ssh"
# (Ethernet, IP por DHCP: BOARD_LINK=ssh BOARD=<ip>).
export BOARD_LINK="${BOARD_LINK:-adb}"
export BOARD="${BOARD:-172.32.0.93}"
export BOARD_USER="${BOARD_USER:-root}"
export ADB="${ADB:-$HOME/android-sdk/platform-tools/adb}"

# Com a rede espelhada da WSL, conectar numa porta fechada de 127.0.0.1 trava em
# vez de falhar, e o adb trava procurando o servidor dele. Sobe o servidor antes.
adb_up() {
    ss -ltn 'sport = :5037' | grep -q LISTEN && return 0
    (setsid nohup "$ADB" server nodaemon > /dev/null 2>&1 < /dev/null &)
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sleep 0.3
        ss -ltn 'sport = :5037' | grep -q LISTEN && return 0
    done
    echo "adb_up: o servidor adb não subiu" >&2
    return 1
}
adb() { adb_up && "$ADB" "$@"; }

if [ ! -x "$RV1106_TOOLCHAIN/bin/${CROSS_COMPILE}gcc" ]; then
    echo "env.sh: toolchain não encontrado, rode ./setup.sh" >&2
fi
