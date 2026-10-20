/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ajuste do ISP ao vivo, pela página. Os parâmetros são os do JSON de tuning
 * (cfg/ov5647_noaf.json): gamma, contraste, saturação, matriz de cor, filtro
 * de ruído e nitidez - mais o alvo de exposição, que na OV5647 é do sensor.
 *
 * Como: uma cópia em RAM da configuração do IPA que o esp_video carregou é
 * editada e reaplicada com esp_video_isp_pipeline_set_ipa_config(). O próprio
 * IPA programa o ISP, então nada briga com ele a cada quadro; e os valores
 * finais são exatamente os que vão para o JSON.
 *
 * Os valores salvos ficam na NVS e voltam no boot: com isso a placa já fica
 * ajustada sem recompilar. Para virar o padrão do firmware, a página gera o
 * JSON para colar no cfg/.
 *
 * Exposição pelo rosto (parâmetros "face" e "face_luma"): com rosto na imagem,
 * uma task própria move o alvo do AE do sensor até o miolo do maior rosto
 * ficar com o brilho face_luma - corrige o contraluz, que o AE do sensor (que
 * mede a imagem inteira) não corrige. Sem rosto, volta ao "ae" do slider.
 *
 * isp_tune_set/save/reset/json: só pela task do httpd. isp_tune_ae_status:
 * de qualquer task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* cam_fd = /dev/video0 já aberto; sensor = nome do sensor no JSON de tuning
 * ("OV5647", "IMX708"). Aplica o que estiver salvo na NVS. */
esp_err_t isp_tune_init(int cam_fd, const char *sensor);

/* Aplica "chave=valor&..." (os nomes do JSON do isp_tune_json). Chave
 * desconhecida ou valor fora da faixa: ESP_ERR_INVALID_ARG, nada muda. */
esp_err_t isp_tune_set(const char *query);

/* Grava os valores atuais na NVS / volta ao padrão do firmware (e apaga). */
esp_err_t isp_tune_save(void);
esp_err_t isp_tune_reset(void);

/* Estado em JSON: valores, faixas, o que este sensor permite ajustar e o que
 * a página precisa para gerar o JSON do cfg/. Devolve os bytes escritos
 * (>= cap = não coube). */
size_t isp_tune_json(char *buf, size_t cap);

typedef struct {
    /* "none" = sem controle de AE (sensor sem alvo de exposição); "off" =
     * exposição pelo rosto desligada; "idle" = sem rosto, vale o slider;
     * "face" = seguindo o rosto; "hold" = rosto sumiu, segurando o ajuste;
     * "back" = voltando ao slider. */
    const char *mode;
    int sensor_ae;      /* alvo do AE no sensor agora; -1 = sem controle */
    int face_luma;      /* brilho do rosto na imagem agora; -1 = sem rosto */
    int target;         /* face_luma buscado */
} isp_ae_status_t;

/* Estado ao vivo da exposição, para a página. */
void isp_tune_ae_status(isp_ae_status_t *out);

#ifdef __cplusplus
}
#endif
