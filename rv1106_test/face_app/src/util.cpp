/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "util.h"

#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>

static std::mutex s_out_lock;

/* Com a saída num arquivo (o app como serviço: >> /tmp/face_app.log, que fica
 * na RAM), zera o arquivo quando passa de LOG_MAX_BYTES. Com O_APPEND a
 * escrita seguinte volta para o começo. Chamar com s_out_lock. */
#define LOG_MAX_BYTES   (2 * 1024 * 1024)
static void limit_log()
{
    static unsigned calls;
    if (++calls % 256) {
        return;
    }
    struct stat st;
    if (fstat(STDOUT_FILENO, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > LOG_MAX_BYTES) {
        fflush(stdout);
        if (ftruncate(STDOUT_FILENO, 0) != 0) {
            return;
        }
    }
}

int64_t now_us()
{
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000000LL + t.tv_nsec / 1000;
}

void log_line(char level, const char *tag, const char *fmt, ...)
{
    std::lock_guard<std::mutex> lock(s_out_lock);
    limit_log();
    printf("%c (%lld) %s: ", level, (long long)(now_us() / 1000), tag);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    fflush(stdout);
}

void out_line(const char *fmt, ...)
{
    std::lock_guard<std::mutex> lock(s_out_lock);
    limit_log();
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    fflush(stdout);
}

std::string exe_dir()
{
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n <= 0) {
        return "./";
    }
    path[n] = '\0';
    char *slash = strrchr(path, '/');
    if (slash) {
        slash[1] = '\0';
    }
    return path;
}

void appendf(std::string &s, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    if ((size_t)n < sizeof(buf)) {
        s.append(buf, n);
        return;
    }
    size_t old = s.size();
    s.resize(old + n + 1);
    va_start(ap, fmt);
    vsnprintf(&s[old], n + 1, fmt, ap);
    va_end(ap);
    s.resize(old + n);
}

std::string base64(const uint8_t *data, size_t len)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(4 * ((len + 2) / 3));
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = (data[i] << 16) | (data[i + 1] << 8) | data[i + 2];
        out += T[v >> 18];
        out += T[(v >> 12) & 63];
        out += T[(v >> 6) & 63];
        out += T[v & 63];
    }
    if (i < len) {
        uint32_t v = data[i] << 16;
        if (i + 1 < len) {
            v |= data[i + 1] << 8;
        }
        out += T[v >> 18];
        out += T[(v >> 12) & 63];
        out += i + 1 < len ? T[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

uint64_t mem_available()
{
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) {
        return 0;
    }
    char line[128];
    unsigned long long kb = 0;
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
            break;
        }
    }
    fclose(f);
    return kb * 1024;
}
