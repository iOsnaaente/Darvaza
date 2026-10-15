/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ver models.h. O decodificador do RetinaFace segue o Pytorch_Retinaface
 * (priors, variâncias 0.1/0.2) e o exemplo da Luckfox, mas descarta pelo
 * valor INT8 cru antes de converter: só as poucas âncoras com rosto passam
 * por float.
 */
#include "models.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <opencv2/imgproc.hpp>

#include "im2d.hpp"
#include "util.h"

static const char *TAG = "models";

#define DET_SCORE_THR   0.5f   /* confiança mínima de rosto */
#define DET_NMS_IOU     0.4f   /* sobreposição para descartar duplicata */

static const float VARIANCE[2] = {0.1f, 0.2f};

/* ---- RetinaFace ---------------------------------------------------------------- */

bool RetinaFace::load(const std::string &path)
{
    if (!m_.load(path) || m_.n_outputs() != 3) {
        return false;
    }
    /* Âncoras do Pytorch_Retinaface: passos 8/16/32, dois tamanhos por passo. */
    static const int STEPS[3] = {8, 16, 32};
    static const int SIZES[3][2] = {{16, 32}, {64, 128}, {256, 512}};
    const int w = m_.in_w(), h = m_.in_h();
    priors_.clear();
    for (int k = 0; k < 3; k++) {
        int fh = (h + STEPS[k] - 1) / STEPS[k], fw = (w + STEPS[k] - 1) / STEPS[k];
        for (int i = 0; i < fh; i++) {
            for (int j = 0; j < fw; j++) {
                for (int s : SIZES[k]) {
                    priors_.push_back({(j + 0.5f) * STEPS[k] / w, (i + 0.5f) * STEPS[k] / h, (float)s / w, (float)s / h});
                }
            }
        }
    }
    if (m_.out_attr(0).n_elems != priors_.size() * 4) {
        LOGE(TAG, "RetinaFace: %u valores de caixa para %u âncoras", m_.out_attr(0).n_elems,
             (unsigned)priors_.size());
        return false;
    }
    return true;
}

static float iou(const Det &a, const Det &b)
{
    int ix = std::min(a.box[2], b.box[2]) - std::max(a.box[0], b.box[0]);
    int iy = std::min(a.box[3], b.box[3]) - std::max(a.box[1], b.box[1]);
    if (ix <= 0 || iy <= 0) {
        return 0;
    }
    float inter = (float)ix * iy;
    return inter / (a.area() + b.area() - inter);
}

bool RetinaFace::detect(const Frame &f, std::vector<Det> &out, size_t max_faces)
{
    out.clear();
    const int mw = m_.in_w(), mh = m_.in_h();

    /* Letterbox: o quadro inteiro, sem distorcer, centrado. A borda fica com
     * a média da conversão (104,117,123), que vira zero para a rede; é
     * pintada pela CPU uma vez, e o RGA só escreve no meio. */
    if (f.w != frame_w_ || f.h != frame_h_) {
        frame_w_ = f.w;
        frame_h_ = f.h;
        scale_ = std::min((float)mw / frame_w_, (float)mh / frame_h_);
        pad_x_ = (mw - (int)std::lround(frame_w_ * scale_)) / 2;
        pad_y_ = (mh - (int)std::lround(frame_h_ * scale_)) / 2;
        cv::Mat input(mh, mw, CV_8UC3, m_.input(), m_.in_stride());
        input.setTo(cv::Scalar(104, 117, 123));
        m_.sync_input();
    }
    rga_buffer_t src = wrapbuffer_fd(f.fd, f.w, f.h, RK_FORMAT_YCbCr_420_SP, f.stride, f.vstride);
    rga_buffer_t dst = wrapbuffer_fd(m_.in_fd(), mw, mh, RK_FORMAT_BGR_888, (int)m_.in_stride() / 3, mh);
    rga_buffer_t pat = {};
    im_rect srect = {0, 0, f.w, f.h};
    im_rect drect = {pad_x_, pad_y_, mw - 2 * pad_x_, mh - 2 * pad_y_};
    im_rect prect = {};
    IM_STATUS st = improcess(src, dst, pat, srect, drect, prect, IM_SYNC);
    if (st != IM_STATUS_SUCCESS) {
        LOGE(TAG, "RGA falhou: %s", imStrError(st));
        return false;
    }

    if (!m_.run()) {
        return false;
    }

    const rknn_tensor_attr &la = m_.out_attr(0), &sa = m_.out_attr(1), &ka = m_.out_attr(2);
    const int8_t *loc = m_.output(0), *score = m_.output(1), *lmk = m_.output(2);
    /* score >= DET_SCORE_THR, direto no valor quantizado. */
    const int q_thr = (int)std::ceil(DET_SCORE_THR / sa.scale + sa.zp);

    std::vector<Det> cand;
    for (size_t i = 0; i < priors_.size(); i++) {
        if (score[i * 2 + 1] < q_thr) {
            continue;
        }
        const auto &p = priors_[i];
        auto dq = [](int8_t q, const rknn_tensor_attr &a) { return (q - a.zp) * a.scale; };
        float cx = p[0] + dq(loc[i * 4 + 0], la) * VARIANCE[0] * p[2];
        float cy = p[1] + dq(loc[i * 4 + 1], la) * VARIANCE[0] * p[3];
        float bw = p[2] * std::exp(dq(loc[i * 4 + 2], la) * VARIANCE[1]);
        float bh = p[3] * std::exp(dq(loc[i * 4 + 3], la) * VARIANCE[1]);

        /* Do espaço do modelo (0..1) para pixels do quadro. */
        auto fx = [&](float v) { return (int)std::lround((v * mw - pad_x_) / scale_); };
        auto fy = [&](float v) { return (int)std::lround((v * mh - pad_y_) / scale_); };
        Det d;
        d.score = dq(score[i * 2 + 1], sa);
        d.box[0] = std::clamp(fx(cx - bw / 2), 0, frame_w_ - 1);
        d.box[1] = std::clamp(fy(cy - bh / 2), 0, frame_h_ - 1);
        d.box[2] = std::clamp(fx(cx + bw / 2), 0, frame_w_ - 1);
        d.box[3] = std::clamp(fy(cy + bh / 2), 0, frame_h_ - 1);
        if (d.box[2] <= d.box[0] || d.box[3] <= d.box[1]) {
            continue;
        }
        for (int j = 0; j < 5; j++) {
            d.kp[2 * j] = std::clamp(fx(p[0] + dq(lmk[i * 10 + 2 * j], ka) * VARIANCE[0] * p[2]), 0, frame_w_ - 1);
            d.kp[2 * j + 1] = std::clamp(fy(p[1] + dq(lmk[i * 10 + 2 * j + 1], ka) * VARIANCE[0] * p[3]), 0, frame_h_ - 1);
        }
        cand.push_back(d);
    }

    std::sort(cand.begin(), cand.end(), [](const Det &a, const Det &b) { return a.score > b.score; });
    for (const Det &d : cand) {
        bool dup = false;
        for (const Det &k : out) {
            if (iou(d, k) > DET_NMS_IOU) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out.push_back(d);
        }
    }
    std::sort(out.begin(), out.end(), [](const Det &a, const Det &b) { return a.area() > b.area(); });
    if (out.size() > max_faces) {
        out.resize(max_faces);
    }
    return true;
}

/* ---- FaceNet ------------------------------------------------------------------- */

bool FaceNet::load(const std::string &path)
{
    if (!m_.load(path) || m_.n_outputs() != 1) {
        return false;
    }
    feat_len_ = (int)m_.out_attr(0).n_elems;
    return feat_len_ > 0;
}

bool FaceNet::extract(const Frame &f, const Det &d, float *out)
{
    cv::Rect r = frame_even_rect(f, cv::Rect(d.box[0], d.box[1], d.box[2] - d.box[0], d.box[3] - d.box[1]));
    if (r.width < 8 || r.height < 8) {
        return false;
    }
    cv::Mat crop = frame_to_bgr(f, r);

    /* Uma transformação só, direto em mw x mh: gira em volta do centro do
     * recorte até os olhos ficarem nivelados (Alignment_1 do
     * facenet-retinaface) e encaixa sem distorcer, borda cinza (128). Em dois
     * passos (girar no tamanho do rosto, depois reduzir) custava ~25 ms. */
    const int mw = m_.in_w(), mh = m_.in_h();
    float dx = (float)(d.kp[0] - d.kp[2]), dy = (float)(d.kp[1] - d.kp[3]);
    float angle = dx != 0 ? std::atan(dy / dx) * 180.0f / (float)M_PI : 0.0f;
    float s = std::min((float)mw / r.width, (float)mh / r.height);
    cv::Mat m = cv::getRotationMatrix2D(cv::Point2f(r.width / 2.0f, r.height / 2.0f), angle, s);
    m.at<double>(0, 2) += mw / 2.0 - r.width / 2.0;
    m.at<double>(1, 2) += mh / 2.0 - r.height / 2.0;
    cv::Mat face;
    cv::warpAffine(crop, face, m, cv::Size(mw, mh), cv::INTER_LINEAR, cv::BORDER_CONSTANT, cv::Scalar(128, 128, 128));
    cv::Mat input(mh, mw, CV_8UC3, m_.input(), m_.in_stride());
    cv::cvtColor(face, input, cv::COLOR_BGR2RGB);

    if (!m_.run()) {
        return false;
    }
    const rknn_tensor_attr &a = m_.out_attr(0);
    const int8_t *q = m_.output(0);
    float norm = 0;
    for (int i = 0; i < feat_len_; i++) {
        out[i] = (q[i] - a.zp) * a.scale;
        norm += out[i] * out[i];
    }
    norm = std::sqrt(norm);
    if (norm <= 0) {
        return false;
    }
    for (int i = 0; i < feat_len_; i++) {
        out[i] /= norm;
    }
    return true;
}
