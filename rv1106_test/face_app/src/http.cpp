/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "http.h"

#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "util.h"

static const char *TAG = "http";

#define MAX_CONNECTIONS   16
#define MAX_HEADER        8192
#define MAX_BODY          (4 * 1024 * 1024)  /* gravação da página /asr: 2 min de 16 kHz mono */
#define RECV_TIMEOUT_S    5

static std::map<std::string, HttpHandler> s_routes;   /* "GET /status" -> handler */
static std::atomic<int> s_connections{0};

void HttpServer::on(const char *method, const char *path, HttpHandler handler)
{
    s_routes[std::string(method) + " " + path] = std::move(handler);
}

bool http_send_all(int fd, const char *data, size_t len)
{
    while (len > 0) {
        ssize_t n = send(fd, data, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        data += n;
        len -= n;
    }
    return true;
}

bool http_respond(int fd, const char *status, const char *type, const void *body, size_t len, const char *extra)
{
    std::string head;
    appendf(head,
            "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nCache-Control: no-store\r\n"
            "%sConnection: close\r\n\r\n",
            status, type, (unsigned)len, extra);
    return http_send_all(fd, head.data(), head.size()) && http_send_all(fd, (const char *)body, len);
}

bool http_respond(int fd, const char *status, const char *type, const std::string &body)
{
    return http_respond(fd, status, type, body.data(), body.size(), "");
}

bool http_query_value(const std::string &query, const char *key, std::string &out)
{
    size_t klen = strlen(key);
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t end = query.find('&', pos);
        if (end == std::string::npos) {
            end = query.size();
        }
        if (query.compare(pos, klen, key) == 0 && pos + klen < query.size() && query[pos + klen] == '=') {
            out = query.substr(pos + klen + 1, end - pos - klen - 1);
            return true;
        }
        pos = end + 1;
    }
    return false;
}

std::string http_url_decode(const std::string &s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && isxdigit((unsigned char)s[i + 1]) &&
                   isxdigit((unsigned char)s[i + 2])) {
            out += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

static void serve(int fd)
{
    timeval tv = {RECV_TIMEOUT_S, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string req;
    char buf[2048];
    size_t head_end;
    while ((head_end = req.find("\r\n\r\n")) == std::string::npos) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0 || req.size() + n > MAX_HEADER) {
            return;
        }
        req.append(buf, n);
    }

    /* Corpo lido inteiro antes de responder: fechar com dados não lidos faz o
     * kernel mandar RST, e o cliente pode perder a resposta. */
    HttpRequest r;
    r.fd = fd;
    const char *cl = strcasestr(req.c_str(), "\r\nContent-Length:");
    size_t want = cl && (size_t)(cl - req.c_str()) < head_end ? strtoul(cl + 17, nullptr, 10) : 0;
    if (want > MAX_BODY) {
        http_respond(fd, "413 Payload Too Large", "text/plain", "corpo grande demais\n");
        return;
    }
    r.body = req.substr(head_end + 4);
    while (r.body.size() < want) {
        ssize_t n = recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) {
            http_respond(fd, "400 Bad Request", "text/plain", "corpo incompleto\n");
            return;
        }
        r.body.append(buf, n);
    }
    r.body.resize(want);

    size_t sp1 = req.find(' '), sp2 = req.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos || sp2 > head_end) {
        http_respond(fd, "400 Bad Request", "text/plain", "requisicao invalida\n");
        return;
    }
    r.method = req.substr(0, sp1);
    std::string target = req.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t q = target.find('?');
    r.path = target.substr(0, q);
    if (q != std::string::npos) {
        r.query = target.substr(q + 1);
    }

    auto it = s_routes.find(r.method + " " + r.path);
    if (it == s_routes.end()) {
        http_respond(fd, "404 Not Found", "text/plain", "nao encontrado\n");
        return;
    }
    it->second(r);
}

bool HttpServer::start(int port)
{
    int ls = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (ls < 0) {
        LOGE(TAG, "socket: %s", strerror(errno));
        return false;
    }
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(ls, (sockaddr *)&addr, sizeof(addr)) != 0 || listen(ls, 16) != 0) {
        LOGE(TAG, "porta %d: %s", port, strerror(errno));
        close(ls);
        return false;
    }

    std::thread([ls] {
        while (true) {
            int fd = accept4(ls, nullptr, nullptr, SOCK_CLOEXEC);
            if (fd < 0) {
                if (errno != EINTR) {
                    usleep(10000);
                }
                continue;
            }
            if (s_connections.fetch_add(1) >= MAX_CONNECTIONS) {
                http_respond(fd, "503 Service Unavailable", "text/plain", "ocupado\n");
                close(fd);
                s_connections.fetch_sub(1);
                continue;
            }
            std::thread([fd] {
                serve(fd);
                close(fd);
                s_connections.fetch_sub(1);
            }).detach();
        }
    }).detach();
    return true;
}
