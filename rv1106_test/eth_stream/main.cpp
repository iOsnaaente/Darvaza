/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Teste: câmera da Luckfox (MIS5001) -> JPEG por hardware -> TCP pela
 * Ethernet até a Raspberry Pi (W5500 no eth1 dela), que repassa a imagem
 * pelo Wi-Fi (raspi/cam_relay.py).
 *
 * Protocolo: cada quadro é um cabeçalho de 16 bytes, big-endian,
 *   "FRM1"  u32 seq  u32 ms_desde_o_boot  u32 tamanho
 * seguido do JPEG. Só isso, sem resposta: quem recebe só lê. Com --bench
 * os blocos vêm com "RAW1" e zeros no lugar do JPEG, sem câmera: mede só o
 * link.
 *
 * Uma thread pega quadros da câmera e guarda só o mais novo (segura um
 * buffer do VI só); a outra converte, codifica e manda. Se a rede for mais
 * lenta que a câmera, os quadros do meio caem aqui e a imagem não acumula
 * atraso. Sem conexão, tenta de novo a cada segundo.
 */
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <opencv2/highgui.hpp>     /* imencode no OpenCV-mobile */

#include "camera.h"
#include "util.h"

static const char *TAG = "eth_stream";

struct options_t {
    std::string host = "192.168.77.1";
    int port = 5000;
    int width = 1280, height = 960;
    int fps = 0;
    int power_hz = 60;
    int quality = 60;
    int secs = 0;       /* > 0: para depois disso e mostra a média */
    bool bench = false; /* blocos brutos, sem câmera */
};

static std::atomic<bool> s_run{true};

static std::mutex s_lock;
static std::condition_variable s_cv;
static FramePtr s_latest;

static void on_signal(int) { s_run = false; }

static void usage(const char *argv0)
{
    fprintf(stderr,
            "uso: %s [--host IP] [--port N] [--width N --height N] [--fps N] [--hz 50|60] [--q 1..100]\n"
            "          [--secs N] [--bench]\n"
            "  padrão: --host 192.168.77.1 --port 5000 --width 1280 --height 960 --q 60\n"
            "  --secs N  para depois de N s e mostra a média\n"
            "  --bench   sem câmera: blocos de 64 KB o mais rápido possível (teto do link)\n",
            argv0);
}

static bool parse(int argc, char **argv, options_t &o)
{
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--bench") {
            o.bench = true;
            continue;
        }
        if (i + 1 >= argc) {
            return false;
        }
        const char *v = argv[++i];
        if (a == "--host") o.host = v;
        else if (a == "--port") o.port = atoi(v);
        else if (a == "--width") o.width = atoi(v);
        else if (a == "--height") o.height = atoi(v);
        else if (a == "--fps") o.fps = atoi(v);
        else if (a == "--hz") o.power_hz = atoi(v);
        else if (a == "--q") o.quality = atoi(v);
        else if (a == "--secs") o.secs = atoi(v);
        else return false;
    }
    return o.quality >= 1 && o.quality <= 100;
}

/* A imagem da Luckfox sobe o rkipc (câmera + RTSP) no boot, e ele segura a
 * câmera. */
static void stop_rkipc()
{
    if (system("pidof rkipc >/dev/null 2>&1") == 0) {
        LOGI(TAG, "parando o rkipc para liberar a câmera");
        if (system("RkLunch-stop.sh >/dev/null 2>&1 || /oem/usr/bin/RkLunch-stop.sh >/dev/null 2>&1") != 0) {
            LOGW(TAG, "RkLunch-stop.sh falhou");
        }
    }
}

static void capture_loop()
{
    while (s_run) {
        FramePtr f = camera_get(1000);
        if (!f) {
            LOGW(TAG, "câmera sem quadro em 1 s");
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(s_lock);
            s_latest = std::move(f);    /* o anterior, se ninguém pegou, volta para o VI */
        }
        s_cv.notify_one();
    }
}

static int connect_to(const options_t &o)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)o.port);
    if (inet_pton(AF_INET, o.host.c_str(), &addr.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    /* Buffer de envio pequeno: no máximo ~1 quadro parado na fila, senão o
     * kernel guarda vários e a imagem chega atrasada. */
    int sndbuf = 128 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    return fd;
}

static bool send_all(int fd, const void *buf, size_t len)
{
    const char *p = static_cast<const char *>(buf);
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static void send_loop(const options_t &o)
{
    std::vector<int> params = {cv::IMWRITE_JPEG_QUALITY, o.quality};
    std::vector<uchar> jpg;
    uint32_t seq = 0;
    int fd = -1;
    int64_t win_start = now_us();
    uint32_t win_frames = 0;
    uint64_t win_bytes = 0;
    int64_t total_start = 0;
    uint32_t total_frames = 0;
    uint64_t total_bytes = 0;

    while (s_run) {
        if (fd < 0) {
            fd = connect_to(o);
            if (fd < 0) {
                LOGW(TAG, "sem conexão com %s:%d - tentando de novo em 1 s", o.host.c_str(), o.port);
                sleep(1);
                continue;
            }
            LOGI(TAG, "conectado a %s:%d", o.host.c_str(), o.port);
            win_start = now_us();
            win_frames = 0;
            win_bytes = 0;
            if (!total_start) {
                total_start = win_start;
            }
        }

        FramePtr f;
        {
            std::unique_lock<std::mutex> lock(s_lock);
            s_cv.wait_for(lock, std::chrono::milliseconds(500), [] { return s_latest != nullptr; });
            f = std::move(s_latest);
        }
        if (!f) {
            continue;
        }
        uint32_t t_ms = (uint32_t)(f->t_us / 1000);
        cv::Mat bgr = frame_to_bgr_full(*f);
        f.reset();
        if (bgr.empty() || !cv::imencode(".jpg", bgr, jpg, params)) {
            LOGW(TAG, "falha ao codificar o quadro");
            continue;
        }

        uint8_t hdr[16] = {'F', 'R', 'M', '1'};
        put_u32(hdr + 4, ++seq);
        put_u32(hdr + 8, t_ms);
        put_u32(hdr + 12, (uint32_t)jpg.size());
        if (!send_all(fd, hdr, sizeof(hdr)) || !send_all(fd, jpg.data(), jpg.size())) {
            LOGW(TAG, "conexão caiu (%s)", strerror(errno));
            close(fd);
            fd = -1;
            continue;
        }

        win_frames++;
        win_bytes += sizeof(hdr) + jpg.size();
        total_frames++;
        total_bytes += sizeof(hdr) + jpg.size();
        int64_t now = now_us();
        if (o.secs > 0 && now - total_start >= (int64_t)o.secs * 1000000) {
            double s = (now - total_start) / 1e6;
            LOGI(TAG, "MEDIA camera q%d: %.1f quadros/s, %.2f Mbit/s, %u KB por quadro (%.0f s)", o.quality,
                 total_frames / s, total_bytes * 8 / s / 1e6, (unsigned)(total_bytes / total_frames / 1024), s);
            s_run = false;
        }
        if (now - win_start >= 5000000) {
            double s = (now - win_start) / 1e6;
            LOGI(TAG, "%.1f quadros/s, %.2f Mbit/s, %u KB por quadro", win_frames / s, win_bytes * 8 / s / 1e6,
                 (unsigned)(win_bytes / win_frames / 1024));
            win_start = now;
            win_frames = 0;
            win_bytes = 0;
        }
    }
    if (fd >= 0) {
        close(fd);
    }
}

/* Teto do link: blocos de 64 KB sem pausa. A conta é do lado de cá (bytes
 * aceitos pelo kernel); o buffer de envio é pequeno, então em 10+ s a
 * diferença para o que chegou é desprezível. */
static int bench_loop(const options_t &o)
{
    int fd = connect_to(o);
    if (fd < 0) {
        LOGE(TAG, "sem conexão com %s:%d", o.host.c_str(), o.port);
        return 1;
    }
    const size_t block = 64 * 1024;
    std::vector<uint8_t> buf(16 + block, 0);
    memcpy(buf.data(), "RAW1", 4);
    put_u32(buf.data() + 12, (uint32_t)block);
    int secs = o.secs > 0 ? o.secs : 10;
    LOGI(TAG, "bench: blocos de %u KB por %d s -> %s:%d", (unsigned)(block / 1024), secs, o.host.c_str(), o.port);

    int64_t start = now_us(), win_start = start;
    uint64_t total = 0, win = 0;
    uint32_t seq = 0;
    while (s_run) {
        put_u32(buf.data() + 4, ++seq);
        put_u32(buf.data() + 8, (uint32_t)(now_us() / 1000));
        if (!send_all(fd, buf.data(), buf.size())) {
            LOGE(TAG, "conexão caiu (%s)", strerror(errno));
            break;
        }
        total += buf.size();
        win += buf.size();
        int64_t now = now_us();
        if (now - win_start >= 2000000) {
            LOGI(TAG, "%.2f Mbit/s", win * 8 / ((now - win_start) / 1e6) / 1e6);
            win_start = now;
            win = 0;
        }
        if (now - start >= (int64_t)secs * 1000000) {
            break;
        }
    }
    double s = (now_us() - start) / 1e6;
    LOGI(TAG, "MEDIA bench: %.2f Mbit/s (%.1f MB em %.1f s)", total * 8 / s / 1e6, total / 1e6, s);
    close(fd);
    return 0;
}

int main(int argc, char **argv)
{
    options_t opt;
    if (!parse(argc, argv, opt)) {
        usage(argv[0]);
        return 2;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (opt.bench) {
        return bench_loop(opt);
    }

    stop_rkipc();
    if (!camera_open(opt.width, opt.height, opt.power_hz, opt.fps)) {
        LOGE(TAG, "a câmera não abriu");
        return 1;
    }
    LOGI(TAG, "câmera %dx%d, JPEG q%d -> %s:%d", opt.width, opt.height, opt.quality, opt.host.c_str(), opt.port);

    std::thread cap(capture_loop);
    send_loop(opt);
    cap.join();
    {
        std::lock_guard<std::mutex> lock(s_lock);
        s_latest.reset();
    }
    camera_close();
    return 0;
}
