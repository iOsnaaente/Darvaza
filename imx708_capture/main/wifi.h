/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Wi-Fi em modo estação pelo ESP32-C6 da placa. O esp_wifi_remote encaminha
 * as chamadas esp_wifi_* para o C6 por SDIO; netif, DHCP e LwIP rodam no P4,
 * então depois de conectar os sockets são sockets comuns.
 */
#pragma once

#include <stddef.h>
#include "esp_err.h"

/**
 * Conecta à rede de wifi_credentials.h e bloqueia até ter IP.
 *
 * Depois de conectado, reconecta sozinho se a rede cair.
 *
 * @param[out] ip_out  endereço em texto ("192.168.0.10"), 16 bytes bastam
 * @return ESP_OK com IP; ESP_ERR_WIFI_NOT_INIT se o C6 não respondeu;
 *         ESP_ERR_TIMEOUT se não associou/obteve IP a tempo.
 */
esp_err_t wifi_connect(char *ip_out, size_t ip_len);
