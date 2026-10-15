/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Um modelo .rknn com a API zero-copy (a única do RV1106): entrada UINT8
 * NHWC, que a NPU normaliza e quantiza sozinha com o mean/std da conversão, e
 * saídas INT8 no layout nativo.
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "rknn_api.h"

class RknnModel {
public:
    ~RknnModel();

    bool load(const std::string &path);

    int in_w() const { return in_w_; }
    int in_h() const { return in_h_; }
    /* Entrada BGR/RGB intercalada, in_stride() bytes por linha. */
    uint8_t *input() const { return (uint8_t *)in_mem_->virt_addr; }
    size_t in_stride() const { return in_stride_; }
    /* A mesma entrada como dma-buf, para o RGA escrever direto nela. */
    int in_fd() const { return in_mem_->fd; }
    /* Depois de escrever na entrada pela CPU, antes de o RGA escrever nela:
     * senão o cache sujo da CPU sobrescreve o que o RGA pôs. */
    void sync_input() { rknn_mem_sync(ctx_, in_mem_, RKNN_MEMORY_SYNC_TO_DEVICE); }

    /* Sincroniza a entrada, roda e sincroniza as saídas. */
    bool run();

    size_t n_outputs() const { return out_mem_.size(); }
    const int8_t *output(size_t i) const { return (const int8_t *)out_mem_[i]->virt_addr; }
    const rknn_tensor_attr &out_attr(size_t i) const { return out_attr_[i]; }

private:
    rknn_context ctx_ = 0;
    rknn_tensor_attr in_attr_ = {};
    rknn_tensor_mem *in_mem_ = nullptr;
    std::vector<rknn_tensor_attr> out_attr_;
    std::vector<rknn_tensor_mem *> out_mem_;
    int in_w_ = 0, in_h_ = 0;
    size_t in_stride_ = 0;
};
