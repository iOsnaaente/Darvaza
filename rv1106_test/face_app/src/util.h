/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Relógio e log no mesmo formato do ESP-IDF ("I (12345) tag: ..."), para a
 * saída ficar igual à do ESP32-P4.
 */
#pragma once

#include <cstdint>
#include <string>

/* Microssegundos desde o boot (CLOCK_MONOTONIC), como o esp_timer. */
int64_t now_us();

void log_line(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#define LOGI(tag, ...) log_line('I', tag, __VA_ARGS__)
#define LOGW(tag, ...) log_line('W', tag, __VA_ARGS__)
#define LOGE(tag, ...) log_line('E', tag, __VA_ARGS__)

/* printf para linhas sem prefixo (tracking, resumo "#"), sem misturar com
 * linhas de outras threads. */
void out_line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Pasta do executável, com "/" no fim. */
std::string exe_dir();

/* snprintf que acumula numa std::string. */
void appendf(std::string &s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

std::string base64(const uint8_t *data, size_t len);

/* MemAvailable do /proc/meminfo, em bytes. */
uint64_t mem_available();
