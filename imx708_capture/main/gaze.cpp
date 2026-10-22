/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ver gaze.h. Tudo em unidades da distância entre os olhos (d), que não
 * depende de quão longe o rosto está.
 *
 * Cabeça: um rosto "médio" com os olhos em x = ±0,5, o nariz NOSE_Z à frente
 * e NOSE_Y abaixo da linha dos olhos, e a boca MOUTH_Z à frente e MOUTH_Y
 * abaixo. Girando a cabeça de yaw, o nariz anda NOSE_Z·sen(yaw) para o lado
 * e a distância entre os olhos encolhe para cos(yaw): u = NOSE_Z·tan(yaw).
 * Inclinando de pitch, a altura do nariz relativa à da boca muda; isolando
 * tan(pitch) dá a fórmula em head_pitch(). Os números são médios: a
 * calibração tira o que sobra de cada pessoa.
 *
 * Olhos: a janela em volta de cada olho (EYE_W x EYE_H) vira cinza (Y do
 * RGB565), é suavizada e os EYE_DARK_FRAC pixels mais escuros, pesados pelo
 * quanto são escuros e por estarem perto do meio, dão o centro da pupila. O
 * olho gira em volta do seu centro com raio EYE_R: deslocamento =
 * EYE_R·sen(ângulo). O ponto do olho do detector já acompanha um pouco a
 * pupila, então o deslocamento medido é menor que o real: EYE_GAIN compensa.
 *
 * Diferença para o RV1106: lá a janela sai do plano Y do NV12 com OpenCV;
 * aqui o quadro é RGB565 e não há OpenCV, então a janela é convertida e
 * suavizada à mão num buffer de no máximo EYE_BUF_W x EYE_BUF_H (rosto muito
 * perto: lê um pixel a cada s, sem mudar o resultado em unidades de d).
 */
#include "gaze.h"

#include <algorithm>
#include <cmath>

#define NOSE_Y              0.55f
#define NOSE_Z              0.60f
#define MOUTH_Y             1.05f
#define MOUTH_Z             0.20f

#define EYE_MIN_D           24      /* olhos mais perto que isso (px): só a cabeça */
#define EYE_MAX_HEAD        35.0f   /* cabeça mais virada que isso (quase perfil): só a cabeça */
#define EYE_W               0.45f
#define EYE_H               0.26f
#define EYE_DARK_FRAC       0.12f
#define EYE_MIN_CONTRAST    18      /* média - mínimo; menos que isso = olho fechado ou sem luz */
#define EYE_R               0.19f   /* raio do globo ocular (12 mm / 63 mm entre os olhos) */
#define EYE_GAIN            1.6f
#define EYE_BUF_W           128     /* maior janela lida pixel a pixel */
#define EYE_BUF_H           80

#define SMOOTH              0.5f    /* peso do quadro novo na média móvel */

static uint8_t s_y[EYE_BUF_W * EYE_BUF_H];
static uint8_t s_blur[EYE_BUF_W * EYE_BUF_H];

static float deg(float rad)
{
    return rad * 180.0f / (float)M_PI;
}

static float head_pitch(float ratio)
{
    return deg(std::atan((NOSE_Y - ratio * MOUTH_Y) / (NOSE_Z - ratio * MOUTH_Z)));
}

/* Y = (77R + 150G + 29B) / 256, como o face_luma de face.cpp. */
static uint8_t luma565(uint16_t p)
{
    int r = (p >> 11) & 0x1f, g = (p >> 5) & 0x3f, b = p & 0x1f;
    return (uint8_t)((77 * ((r << 3) | (r >> 2)) + 150 * ((g << 2) | (g >> 4)) + 29 * ((b << 3) | (b >> 2))) >> 8);
}

/* Centro da pupila na janela em volta de (cx, cy), em pixels do quadro. */
static bool find_pupil(const uint8_t *img, int iw, int ih, float cx, float cy, float d, float *px, float *py)
{
    const int w = (int)std::lround(d * EYE_W), h = (int)std::lround(d * EYE_H);
    const int x0 = std::max(0, (int)std::lround(cx - w / 2.0f)), y0 = std::max(0, (int)std::lround(cy - h / 2.0f));
    const int x1 = std::min(iw, (int)std::lround(cx - w / 2.0f) + w);
    const int y1 = std::min(ih, (int)std::lround(cy - h / 2.0f) + h);
    if (x1 - x0 < 8 || y1 - y0 < 5) {
        return false;
    }
    const int s = std::max({1, (x1 - x0 + EYE_BUF_W - 1) / EYE_BUF_W, (y1 - y0 + EYE_BUF_H - 1) / EYE_BUF_H});
    const int cols = (x1 - x0) / s, rows = (y1 - y0) / s;

    const uint16_t *pix = (const uint16_t *)img;
    for (int i = 0; i < rows; i++) {
        const uint16_t *row = pix + (size_t)(y0 + i * s) * iw + x0;
        for (int j = 0; j < cols; j++) {
            s_y[i * cols + j] = luma565(row[j * s]);
        }
    }
    /* Gaussiana 3x3 ([1 2 1] nos dois eixos), borda repetida. */
    int hist[256] = {};
    int vmin = 255, sum = 0;
    for (int i = 0; i < rows; i++) {
        const uint8_t *up = s_y + std::max(i - 1, 0) * cols, *mid = s_y + i * cols;
        const uint8_t *dn = s_y + std::min(i + 1, rows - 1) * cols;
        for (int j = 0; j < cols; j++) {
            const int l = std::max(j - 1, 0), r = std::min(j + 1, cols - 1);
            int v = (up[l] + 2 * up[j] + up[r] + 2 * (mid[l] + 2 * mid[j] + mid[r]) + dn[l] + 2 * dn[j] + dn[r] + 8) >> 4;
            s_blur[i * cols + j] = (uint8_t)v;
            hist[v]++;
            sum += v;
            vmin = std::min(vmin, v);
        }
    }
    const int n = rows * cols;
    if (sum / n - vmin < EYE_MIN_CONTRAST) {
        return false;
    }
    int thr = vmin, acc = 0;
    while (thr < 255 && (acc += hist[thr]) < n * EYE_DARK_FRAC) {
        thr++;
    }

    /* Peso: quanto mais escuro, mais conta; e uma gaussiana em volta do meio,
     * para sombra no canto do olho ou sobrancelha na borda não puxarem. */
    const float mx = (cx - x0) / s, my = (cy - y0) / s;
    const float sx2 = 2 * (0.35f * cols) * (0.35f * cols), sy2 = 2 * (0.6f * rows) * (0.6f * rows);
    float sw = 0, swx = 0, swy = 0;
    for (int i = 0; i < rows; i++) {
        const uint8_t *p = s_blur + i * cols;
        for (int j = 0; j < cols; j++) {
            if (p[j] > thr) {
                continue;
            }
            float wt = (thr - p[j] + 1) * std::exp(-(j - mx) * (j - mx) / sx2 - (i - my) * (i - my) / sy2);
            sw += wt;
            swx += wt * j;
            swy += wt * i;
        }
    }
    if (sw <= 0) {
        return false;
    }
    *px = x0 + s * swx / sw;
    *py = y0 + s * swy / sw;
    return true;
}

void gaze_measure(const uint8_t *img, int iw, int ih, const int kp[10], gaze_t *out)
{
    *out = {};
    std::fill(out->pupil, out->pupil + 4, -1);

    const float lx = kp[0], ly = kp[1], rx = kp[2], ry = kp[3], nx = kp[4], ny = kp[5];
    const float mx = (kp[6] + kp[8]) / 2.0f, my = (kp[7] + kp[9]) / 2.0f;
    const float d = std::hypot(rx - lx, ry - ly);
    if (d < 4) {
        return;
    }
    /* Eixo dos olhos (da esquerda para a direita da imagem) e a perpendicular
     * para baixo: o rosto inclinado de lado (roll) não atrapalha. */
    const float ax = (rx - lx) / d, ay = (ry - ly) / d, bx = -ay, by = ax;
    const float ex = (lx + rx) / 2, ey = (ly + ry) / 2;

    float u = ((nx - ex) * ax + (ny - ey) * ay) / d;
    float v = ((nx - ex) * bx + (ny - ey) * by) / d;
    float vm = ((mx - ex) * bx + (my - ey) * by) / d;
    out->head_yaw = deg(std::atan(u / NOSE_Z));
    out->head_pitch = vm > 0.2f ? head_pitch(v / vm) : 0.0f;

    if (d < EYE_MIN_D || std::fabs(out->head_yaw) > EYE_MAX_HEAD || std::fabs(out->head_pitch) > EYE_MAX_HEAD) {
        return;
    }
    float ox = 0, oy = 0;
    int found = 0;
    const float eye[2][2] = {{lx, ly}, {rx, ry}};
    for (int k = 0; k < 2; k++) {
        float px, py;
        if (!find_pupil(img, iw, ih, eye[k][0], eye[k][1], d, &px, &py)) {
            continue;
        }
        out->pupil[2 * k] = (int16_t)std::lround(px);
        out->pupil[2 * k + 1] = (int16_t)std::lround(py);
        ox += ((px - eye[k][0]) * ax + (py - eye[k][1]) * ay) / d;
        oy += ((px - eye[k][0]) * bx + (py - eye[k][1]) * by) / d;
        found++;
    }
    if (found) {
        out->eyes = true;
        ox *= EYE_GAIN / (found * EYE_R);
        oy *= EYE_GAIN / (found * EYE_R);
        out->eye_yaw = deg(std::asin(std::clamp(ox, -1.0f, 1.0f)));
        out->eye_pitch = deg(std::asin(std::clamp(-oy, -1.0f, 1.0f)));
    }
}

void gaze_smooth(gaze_t *acc, const gaze_t *m, bool first)
{
    auto mix = [first](float &a, float b) { a = first ? b : a + SMOOTH * (b - a); };
    mix(acc->head_yaw, m->head_yaw);
    mix(acc->head_pitch, m->head_pitch);
    if (m->eyes) {
        mix(acc->eye_yaw, m->eye_yaw);
        mix(acc->eye_pitch, m->eye_pitch);
    } else if (first) {
        acc->eye_yaw = acc->eye_pitch = 0;
    }
    acc->eyes = m->eyes;
    std::copy(m->pupil, m->pupil + 4, acc->pupil);
}
