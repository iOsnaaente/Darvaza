/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Retângulos e texto (fonte 5x7 ampliada) num quadro BGR, para as caixas da
 * foto (/capture.jpg). Mesma fonte do overlay.c do P4: só dígitos, letras
 * maiúsculas e "% - . : ?".
 */
#pragma once

#include <opencv2/core.hpp>

void overlay_fill(cv::Mat &img, int x0, int y0, int x1, int y1, const cv::Scalar &color);
void overlay_rect(cv::Mat &img, int x0, int y0, int x1, int y1, int thick, const cv::Scalar &color);
void overlay_text(cv::Mat &img, int x, int y, const char *s, int scale, const cv::Scalar &color);
int overlay_text_width(const char *s, int scale);
int overlay_text_height(int scale);
