/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ver wifi.h. Baseado no imx_wifi do esp32-p4-imx-camera, testado com o C6
 * das placas Waveshare.
 */
#include <stdio.h>
#include <string.h>
#include "wifi.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#if !__has_include("wifi_credentials.h")
#error "Falta main/wifi_credentials.h: copie main/wifi_credentials.h.example e preencha SSID/senha."
#endif
#include "wifi_credentials.h"

#define STATUS_EVERY_MS 10000
#define GOT_IP_BIT      BIT0
/* Associado e sem IP por mais que isso: derruba e associa de novo. Visto na
 * placa: o C6 (firmware de fábrica) às vezes associa e o DHCP nunca termina,
 * sem nenhum evento de desconexão - sem isto, fica sem rede para sempre. */
#define DHCP_TIMEOUT_US (15 * 1000 * 1000)

static const char *TAG = "wifi";

static EventGroupHandle_t s_events;
static volatile uint8_t s_last_reason;
static volatile int64_t s_assoc_us;     /* associou e ainda sem IP; 0 = não */
static char s_ip[16];

static const char *reason_hint(uint8_t reason)
{
    switch (reason) {
    case 0:
        return "ainda tentando";
    case WIFI_REASON_NO_AP_FOUND:
        return "SSID não encontrado. O C6 só enxerga 2,4 GHz - rede só 5 GHz é invisível";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "autenticação falhou - quase sempre senha errada";
    case WIFI_REASON_ASSOC_FAIL:
        return "o AP recusou a associação (filtro de MAC?)";
    default:
        return "ver WIFI_REASON_* em esp_wifi_types.h";
    }
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        /* Conectar só depois do START: antes dá ESP_ERR_WIFI_NOT_STARTED. */
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        s_assoc_us = esp_timer_get_time();
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        s_assoc_us = 0;
        s_last_reason = e->reason;
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        ESP_LOGW(TAG, "desconectado, motivo %u (%s) - reconectando", e->reason, reason_hint(e->reason));
        esp_wifi_connect();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_LOST_IP) {
        /* Chega ~2 min depois de uma desconexão; quem reconecta é o
         * DISCONNECTED. Aqui só registra - derrubar o link agora atrapalharia
         * uma associação em andamento. */
        ESP_LOGW(TAG, "IP perdido");
        xEventGroupClearBits(s_events, GOT_IP_BIT);
        return;
    }
    ip_event_got_ip_t *e = data;
    s_assoc_us = 0;
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
    ESP_LOGI(TAG, "IP %s  gateway " IPSTR, s_ip, IP2STR(&e->ip_info.gw));
    xEventGroupSetBits(s_events, GOT_IP_BIT);
}

/* Roda sempre, não só no boot: a mesma trava pode acontecer numa reconexão. */
static void dhcp_watchdog_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        int64_t since = s_assoc_us;
        if (since && esp_timer_get_time() - since > DHCP_TIMEOUT_US) {
            ESP_LOGW(TAG, "associado há %d s sem IP (DHCP travado) - reconectando",
                     (int)(DHCP_TIMEOUT_US / 1000000));
            s_assoc_us = 0;
            esp_wifi_disconnect();
        }
    }
}

esp_err_t wifi_connect(char *ip_out, size_t ip_len)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_sta();

#ifdef WIFI_STATIC_IP
    /* IP fixo (opcional, em wifi_credentials.h): para redes cujo DHCP não
     * responde. Com o DHCP parado, o esp_netif gera o GOT_IP ao associar. */
    esp_netif_dhcpc_stop(netif);
    esp_netif_ip_info_t ip_info = {0};
    ip_info.ip.addr = esp_ip4addr_aton(WIFI_STATIC_IP);
    ip_info.gw.addr = esp_ip4addr_aton(WIFI_STATIC_GATEWAY);
    ip_info.netmask.addr = esp_ip4addr_aton(WIFI_STATIC_NETMASK);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip_info));
    ESP_LOGI(TAG, "IP fixo %s (DHCP desligado)", WIFI_STATIC_IP);
#else
    (void)netif;
#endif

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_ip_event, NULL, NULL));

    /* Falha aqui é o C6 que não respondeu pelo SDIO, não a rede. */
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init falhou: %s - o ESP32-C6 não respondeu", esp_err_to_name(err));
        return ESP_ERR_WIFI_NOT_INIT;
    }

    wifi_config_t sta = { 0 };
    strncpy((char *)sta.sta.ssid, WIFI_SSID, sizeof(sta.sta.ssid) - 1);
    strncpy((char *)sta.sta.password, WIFI_PASSWORD, sizeof(sta.sta.password) - 1);
    /* authmode mínimo fica no padrão (aberto): subir para WPA2 filtra APs em
     * silêncio e aparece como NO_AP_FOUND. */
    sta.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta));
    /* Antes do start: o CONNECTED pode chegar a qualquer momento depois dele. */
    xTaskCreate(dhcp_watchdog_task, "wifi_wdt", 3072, NULL, 3, NULL);
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "conectando a \"%s\" ...", WIFI_SSID);

    while (!(xEventGroupWaitBits(s_events, GOT_IP_BIT, pdFALSE, pdFALSE,
                                 pdMS_TO_TICKS(STATUS_EVERY_MS)) & GOT_IP_BIT)) {
        ESP_LOGW(TAG, "ainda sem IP - último motivo %u: %s", s_last_reason, reason_hint(s_last_reason));
    }

    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        ESP_LOGI(TAG, "associado a \"%s\"  canal %u  rssi %d dBm", (const char *)ap.ssid, ap.primary, ap.rssi);
    }
    /* Power save faz o AP segurar pacotes entre beacons: vídeo vira rajadas. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    strncpy(ip_out, s_ip, ip_len - 1);
    ip_out[ip_len - 1] = '\0';
    return ESP_OK;
}
