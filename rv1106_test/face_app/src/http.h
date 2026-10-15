/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Servidor HTTP/1.1 mínimo: uma thread por conexão, uma requisição por
 * conexão (Connection: close). Basta para a página, o SSE do /events e os
 * poucos endpoints de controle.
 */
#pragma once

#include <functional>
#include <string>

struct HttpRequest {
    int fd;
    std::string method;
    std::string path;
    std::string query;   /* sem o "?" */
};

using HttpHandler = std::function<void(HttpRequest &)>;

class HttpServer {
public:
    void on(const char *method, const char *path, HttpHandler handler);
    /* Escuta em todas as interfaces; atende numa thread própria. */
    bool start(int port);
};

/* false = o cliente foi embora. */
bool http_send_all(int fd, const char *data, size_t len);
bool http_respond(int fd, const char *status, const char *type, const std::string &body);
bool http_query_value(const std::string &query, const char *key, std::string &out);
