// core/launcher/launcher_image.cpp - the launcher's image fitting: an RGBA
// image with straight alpha resampled to the size it is drawn at, so the
// page's linearly filtered, unmipmapped textures sample it near one texel
// per pixel.
//
// The resample is an area (box) filter per axis over premultiplied colour in
// linear light: each output texel is the coverage-weighted mean of the
// source texels its footprint overlaps. The result is stored with straight
// alpha, as imgui's pipeline blends it, and every texel left at zero alpha
// takes the mean colour of its nonzero neighbours, so a linear tap across an
// edge mixes the edge's own colour and draws no dark or palette-coloured
// fringe.

#include "launcher_internal.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

namespace {

/** @brief Decode an sRGB-encoded 8-bit value to linear light.
 *
 * @param v The value.
 * @return Linear light, 0..1. */
float srgb_to_linear(unsigned char v) {
    static float lut[256];
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 256; i++) {
            const float c = (float)i / 255.0f;
            lut[i] = c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
        }
        ready = true;
    }
    return lut[v];
}

/** @brief Encode linear light as an sRGB 8-bit value, rounded.
 *
 * @param c Linear light; clamped to 0..1.
 * @return The value. */
unsigned char linear_to_srgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    const float s = c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f;
    return (unsigned char)std::lround(std::clamp(s, 0.0f, 1.0f) * 255.0f);
}

/** @brief Area-resample one axis of a premultiplied float RGBA image.
 *
 * Output texel i covers source [i * n_in / n_out, (i + 1) * n_in / n_out);
 * each source texel weighs by its overlap with that span.
 * @param in The source, @p lines lines of @p n_in texels each, @p in_step floats between texels along the axis and @p in_line between lines.
 * @param n_in The source texels along the axis.
 * @param n_out The output texels along the axis, at most @p n_in.
 * @param lines The lines.
 * @param in_step Floats between neighbouring source texels on a line.
 * @param in_line Floats between source lines.
 * @param out The output, laid out with @p out_step and @p out_line.
 * @param out_step Floats between neighbouring output texels on a line.
 * @param out_line Floats between output lines. */
void area_axis(const float* in, int n_in, int n_out, int lines, size_t in_step, size_t in_line, float* out,
               size_t out_step, size_t out_line) {
    const double scale = (double)n_in / (double)n_out;
    for (int i = 0; i < n_out; i++) {
        const double a = i * scale, b = (i + 1) * scale;
        const int s0 = (int)std::floor(a), s1 = std::min(n_in, (int)std::ceil(b));
        for (int l = 0; l < lines; l++) {
            float acc[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            for (int s = s0; s < s1; s++) {
                const float w = (float)((std::min(b, (double)(s + 1)) - std::max(a, (double)s)) / scale);
                const float* p = in + (size_t)l * in_line + (size_t)s * in_step;
                for (int c = 0; c < 4; c++) acc[c] += p[c] * w;
            }
            float* o = out + (size_t)l * out_line + (size_t)i * out_step;
            for (int c = 0; c < 4; c++) o[c] = acc[c];
        }
    }
}

/** @brief Return the mean colour of a texel's filled 8-neighbours.
 *
 * @param px The image, w * h RGBA.
 * @param filled One flag per texel: its colour is set.
 * @param w The width.
 * @param h The height.
 * @param x The texel's column.
 * @param y The texel's row.
 * @param rgb Receives the mean, rounded; untouched when no neighbour is filled.
 * @return The filled neighbours counted. */
int neighbour_mean(const unsigned char* px, const std::vector<unsigned char>& filled, int w, int h, int x, int y,
                   unsigned char rgb[3]) {
    int sum[3] = {0, 0, 0}, n = 0;
    for (int k = 0; k < 9; k++) {
        const int nx = x + k % 3 - 1, ny = y + k / 3 - 1;
        if (k == 4 || nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
        const size_t j = (size_t)ny * w + nx;
        if (!filled[j]) continue;
        for (int c = 0; c < 3; c++) sum[c] += px[4 * j + c];
        n++;
    }
    for (int c = 0; n && c < 3; c++) rgb[c] = (unsigned char)((sum[c] + n / 2) / n);
    return n;
}

/** @brief Give each zero-alpha texel adjacent to a nonzero one the mean colour of those neighbours, in two passes.
 *
 * Two passes cover every texel a linear tap next to the edge can read.
 * @param px The image, w * h RGBA with straight alpha, rewritten in place.
 * @param w The width.
 * @param h The height. */
void bleed(unsigned char* px, int w, int h) {
    const size_t n = (size_t)w * h;
    std::vector<unsigned char> filled(n);
    for (size_t i = 0; i < n; i++) filled[i] = px[4 * i + 3] != 0;
    for (int pass = 0; pass < 2; pass++) {
        std::vector<unsigned char> next = filled;
        for (size_t i = 0; i < n; i++) {
            if (!filled[i]) next[i] = neighbour_mean(px, filled, w, h, (int)(i % w), (int)(i / w), px + 4 * i) > 0;
        }
        filled.swap(next);
    }
}

}  // namespace

void launcher_image_fit(const unsigned char* src, int sw, int sh, int dw, int dh, unsigned char* out, int* ow,
                        int* oh) {
    dw = std::clamp(dw, 1, sw);
    dh = std::clamp(dh, 1, sh);
    *ow = dw;
    *oh = dh;
    std::vector<float> pre((size_t)sw * sh * 4u);
    for (size_t i = 0; i < (size_t)sw * sh; i++) {
        const float a = (float)src[4 * i + 3] / 255.0f;
        for (int c = 0; c < 3; c++) pre[4 * i + c] = srgb_to_linear(src[4 * i + c]) * a;
        pre[4 * i + 3] = a;
    }
    std::vector<float> mid((size_t)dw * sh * 4u), fin((size_t)dw * dh * 4u);
    area_axis(pre.data(), sw, dw, sh, 4, (size_t)sw * 4u, mid.data(), 4, (size_t)dw * 4u);
    area_axis(mid.data(), sh, dh, dw, (size_t)dw * 4u, 4, fin.data(), (size_t)dw * 4u, 4);
    std::fill(out, out + (size_t)dw * dh * 4u, (unsigned char)0);
    for (size_t i = 0; i < (size_t)dw * dh; i++) {
        const float a = fin[4 * i + 3];
        const unsigned char a8 = (unsigned char)std::lround(std::clamp(a, 0.0f, 1.0f) * 255.0f);
        out[4 * i + 3] = a8;
        if (!a8) continue;
        for (int c = 0; c < 3; c++) out[4 * i + c] = linear_to_srgb(fin[4 * i + c] / a);
    }
    bleed(out, dw, dh);
}
