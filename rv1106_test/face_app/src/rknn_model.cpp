/*
 * SPDX-License-Identifier: Apache-2.0
 */
#include "rknn_model.h"

#include "util.h"

static const char *TAG = "rknn";

RknnModel::~RknnModel()
{
    if (!ctx_) {
        return;
    }
    if (in_mem_) {
        rknn_destroy_mem(ctx_, in_mem_);
    }
    for (auto *m : out_mem_) {
        rknn_destroy_mem(ctx_, m);
    }
    rknn_destroy(ctx_);
}

bool RknnModel::load(const std::string &path)
{
    /* Tamanho 0 = o argumento é o caminho do arquivo. */
    int ret = rknn_init(&ctx_, (void *)path.c_str(), 0, 0, nullptr);
    if (ret < 0) {
        LOGE(TAG, "rknn_init(%s) falhou: %d", path.c_str(), ret);
        ctx_ = 0;
        return false;
    }

    rknn_input_output_num io;
    if (rknn_query(ctx_, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) != RKNN_SUCC || io.n_input != 1) {
        LOGE(TAG, "%s: esperava 1 entrada", path.c_str());
        return false;
    }

    in_attr_.index = 0;
    if (rknn_query(ctx_, RKNN_QUERY_NATIVE_INPUT_ATTR, &in_attr_, sizeof(in_attr_)) != RKNN_SUCC ||
        in_attr_.n_dims != 4 || in_attr_.dims[3] != 3) {
        LOGE(TAG, "%s: entrada não é NHWC de 3 canais", path.c_str());
        return false;
    }
    in_h_ = (int)in_attr_.dims[1];
    in_w_ = (int)in_attr_.dims[2];
    /* UINT8: a NPU faz (pixel - mean) / std e quantiza. Sem isto a entrada
     * fica INT8 e pixel >= 128 vira negativo (o demo da Luckfox tem esse erro
     * no MobileFaceNet). */
    in_attr_.type = RKNN_TENSOR_UINT8;
    in_attr_.fmt = RKNN_TENSOR_NHWC;
    in_stride_ = (size_t)(in_attr_.w_stride ? in_attr_.w_stride : in_w_) * 3;
    in_mem_ = rknn_create_mem(ctx_, in_attr_.size_with_stride);
    if (!in_mem_ || rknn_set_io_mem(ctx_, in_mem_, &in_attr_) != RKNN_SUCC) {
        LOGE(TAG, "%s: falha preparando a entrada", path.c_str());
        return false;
    }

    out_attr_.resize(io.n_output);
    for (uint32_t i = 0; i < io.n_output; i++) {
        out_attr_[i] = {};
        out_attr_[i].index = i;
        if (rknn_query(ctx_, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR, &out_attr_[i], sizeof(rknn_tensor_attr)) !=
                RKNN_SUCC ||
            out_attr_[i].type != RKNN_TENSOR_INT8) {
            LOGE(TAG, "%s: saída %u não é INT8", path.c_str(), i);
            return false;
        }
        rknn_tensor_mem *m = rknn_create_mem(ctx_, out_attr_[i].size_with_stride);
        if (!m || rknn_set_io_mem(ctx_, m, &out_attr_[i]) != RKNN_SUCC) {
            LOGE(TAG, "%s: falha preparando a saída %u", path.c_str(), i);
            return false;
        }
        out_mem_.push_back(m);
    }
    return true;
}

bool RknnModel::run()
{
    rknn_mem_sync(ctx_, in_mem_, RKNN_MEMORY_SYNC_TO_DEVICE);
    int ret = rknn_run(ctx_, nullptr);
    if (ret < 0) {
        LOGE(TAG, "rknn_run falhou: %d", ret);
        return false;
    }
    for (auto *m : out_mem_) {
        rknn_mem_sync(ctx_, m, RKNN_MEMORY_SYNC_FROM_DEVICE);
    }
    return true;
}
