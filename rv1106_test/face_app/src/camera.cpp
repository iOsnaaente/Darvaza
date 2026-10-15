/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Ver camera.h. A sequência (ISP antes do VI) é a dos exemplos RKMPI da
 * Luckfox.
 */
#include "camera.h"

#include <algorithm>
#include <string>

#include <opencv2/imgproc.hpp>

#include "rk_aiq_user_api2_ae.h"
#include "rk_aiq_user_api2_imgproc.h"
#include "rk_aiq_user_api2_sysctl.h"
#include "rk_mpi_mb.h"
#include "rk_mpi_sys.h"
#include "rk_mpi_vi.h"

#include "util.h"

static const char *TAG = "camera";

#define CAM_ID          0
#define VI_DEV          0
#define VI_PIPE         0
#define VI_CHN          0
/* Até 3 quadros ficam com o app ao mesmo tempo (IA, fila dos recortes,
 * captura) e o ISP precisa de 2 livres para não perder quadro. */
#define VI_BUF_COUNT    5
#define VI_DEPTH        2       /* quadros prontos esperando o GetChnFrame */
#define IQ_DIR          "/etc/iqfiles"

static rk_aiq_sys_ctx_t *s_aiq;
static bool s_sys, s_chn;

/* 3A (rkaiq) direto, em vez do SAMPLE_COMM_ISP dos exemplos: ele esconde o
 * contexto, e sem o contexto não dá para trocar o antiflicker. */
static bool isp_start(int power_hz, int fps)
{
    rk_aiq_static_info_t info = {};
    if (rk_aiq_uapi2_sysctl_enumStaticMetas(CAM_ID, &info) != XCAM_RETURN_NO_ERROR || !info.sensor_info.sensor_name[0]) {
        LOGE(TAG, "nenhum sensor (a MIS5001 aparece em dmesg | grep mis5001?)");
        return false;
    }
    s_aiq = rk_aiq_uapi2_sysctl_init(info.sensor_info.sensor_name, IQ_DIR, nullptr, nullptr);
    if (!s_aiq || rk_aiq_uapi2_sysctl_prepare(s_aiq, 0, 0, RK_AIQ_WORKING_MODE_NORMAL) != XCAM_RETURN_NO_ERROR ||
        rk_aiq_uapi2_sysctl_start(s_aiq) != XCAM_RETURN_NO_ERROR) {
        LOGE(TAG, "rkaiq não iniciou para %s", info.sensor_info.sensor_name);
        return false;
    }
    /* O IQ da MIS5001 vem com 50 Hz. Com rede de 60 Hz (Brasil) isso dá
     * faixas na imagem e prende a câmera em 25 quadros/s. */
    rk_aiq_uapi2_setExpPwrLineFreqMode(s_aiq, power_hz == 50 ? EXP_PWR_LINE_FREQ_50HZ : EXP_PWR_LINE_FREQ_60HZ);

    /* Taxa fixa: com pouca luz o AE alonga a exposição e baixa os quadros/s
     * (vimos 20). Para tracking, melhor taxa constante e mais ganho. */
    Uapi_ExpSwAttrV2_t ae;
    if (fps > 0 && rk_aiq_user_api2_ae_getExpSwAttr(s_aiq, &ae) == XCAM_RETURN_NO_ERROR) {
        ae.stAuto.stFrmRate.isFpsFix = true;
        ae.stAuto.stFrmRate.FpsValue = (float)fps;
        if (rk_aiq_user_api2_ae_setExpSwAttr(s_aiq, ae) != XCAM_RETURN_NO_ERROR) {
            LOGW(TAG, "não consegui fixar %d quadros/s", fps);
        }
    }
    LOGI(TAG, "sensor %s, antiflicker %d Hz, %s", info.sensor_info.sensor_name, power_hz == 50 ? 50 : 60,
         fps > 0 ? (std::to_string(fps) + " quadros/s fixos").c_str() : "quadros/s automático");
    return true;
}

bool camera_open(int width, int height, int power_hz, int fps)
{
    if (!isp_start(power_hz, fps)) {
        return false;
    }
    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        LOGE(TAG, "RK_MPI_SYS_Init falhou");
        return false;
    }
    s_sys = true;

    VI_DEV_ATTR_S dev = {};
    if (RK_MPI_VI_GetDevAttr(VI_DEV, &dev) == RK_ERR_VI_NOT_CONFIG && RK_MPI_VI_SetDevAttr(VI_DEV, &dev) != RK_SUCCESS) {
        LOGE(TAG, "RK_MPI_VI_SetDevAttr falhou");
        return false;
    }
    if (RK_MPI_VI_GetDevIsEnable(VI_DEV) != RK_SUCCESS) {
        VI_DEV_BIND_PIPE_S bind = {};
        bind.u32Num = 1;
        bind.PipeId[0] = VI_PIPE;
        if (RK_MPI_VI_EnableDev(VI_DEV) != RK_SUCCESS || RK_MPI_VI_SetDevBindPipe(VI_DEV, &bind) != RK_SUCCESS) {
            LOGE(TAG, "VI não habilitou");
            return false;
        }
    }

    VI_CHN_ATTR_S chn = {};
    chn.stIspOpt.u32BufCount = VI_BUF_COUNT;
    chn.stIspOpt.enMemoryType = VI_V4L2_MEMORY_TYPE_DMABUF;
    chn.stSize.u32Width = width;
    chn.stSize.u32Height = height;
    chn.enPixelFormat = RK_FMT_YUV420SP;
    chn.enCompressMode = COMPRESS_MODE_NONE;
    chn.u32Depth = VI_DEPTH;
    if (RK_MPI_VI_SetChnAttr(VI_PIPE, VI_CHN, &chn) != RK_SUCCESS || RK_MPI_VI_EnableChn(VI_PIPE, VI_CHN) != RK_SUCCESS) {
        LOGE(TAG, "canal VI %dx%d não habilitou", width, height);
        return false;
    }
    s_chn = true;
    return true;
}

static void release_frame(const Frame *f)
{
    auto *info = static_cast<VIDEO_FRAME_INFO_S *>(f->priv);
    RK_MPI_VI_ReleaseChnFrame(VI_PIPE, VI_CHN, info);
    delete info;
    delete f;
}

FramePtr camera_get(int timeout_ms)
{
    auto *info = new VIDEO_FRAME_INFO_S();
    if (RK_MPI_VI_GetChnFrame(VI_PIPE, VI_CHN, info, timeout_ms) != RK_SUCCESS) {
        delete info;
        return nullptr;
    }
    MB_BLK blk = info->stVFrame.pMbBlk;
    /* A CPU lê recortes deste buffer: descarta linhas de cache de um quadro
     * anterior que usou a mesma memória. */
    RK_MPI_SYS_MmzFlushCache(blk, RK_TRUE);

    auto *f = new Frame();
    f->w = (int)info->stVFrame.u32Width;
    f->h = (int)info->stVFrame.u32Height;
    f->stride = (int)info->stVFrame.u32VirWidth;
    f->vstride = (int)info->stVFrame.u32VirHeight;
    f->y = static_cast<uint8_t *>(RK_MPI_MB_Handle2VirAddr(blk));
    f->uv = f->y + (size_t)f->stride * f->vstride;
    f->fd = RK_MPI_MB_Handle2Fd(blk);
    f->t_us = now_us();
    f->priv = info;
    return FramePtr(f, release_frame);
}

void camera_close()
{
    if (s_chn) {
        RK_MPI_VI_DisableChn(VI_PIPE, VI_CHN);
        RK_MPI_VI_DisableDev(VI_DEV);
    }
    if (s_aiq) {
        rk_aiq_uapi2_sysctl_stop(s_aiq, false);
        rk_aiq_uapi2_sysctl_deinit(s_aiq);
        s_aiq = nullptr;
    }
    if (s_sys) {
        RK_MPI_SYS_Exit();
    }
    s_chn = s_sys = false;
}

cv::Rect frame_even_rect(const Frame &f, cv::Rect r)
{
    int x1 = std::min((r.x + r.width + 1) & ~1, f.w & ~1);
    int y1 = std::min((r.y + r.height + 1) & ~1, f.h & ~1);
    r.x = std::max(r.x, 0) & ~1;
    r.y = std::max(r.y, 0) & ~1;
    r.width = std::max(x1 - r.x, 0);
    r.height = std::max(y1 - r.y, 0);
    return r;
}

cv::Mat frame_to_bgr(const Frame &f, const cv::Rect &r, cv::Size out)
{
    if (r.width <= 0 || r.height <= 0) {
        return cv::Mat();
    }
    cv::Mat y(f.h, f.w, CV_8UC1, f.y, f.stride);
    cv::Mat uv(f.h / 2, f.w / 2, CV_8UC2, f.uv, f.stride);
    cv::Mat bgr;
    cv::cvtColorTwoPlane(y(r), uv(cv::Rect(r.x / 2, r.y / 2, r.width / 2, r.height / 2)), bgr,
                         cv::COLOR_YUV2BGR_NV12);
    if (out.area() > 0 && out != bgr.size()) {
        cv::Mat small;
        cv::resize(bgr, small, out, 0, 0, cv::INTER_NEAREST);
        return small;
    }
    return bgr;
}
