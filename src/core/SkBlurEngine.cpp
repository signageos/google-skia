/*
 * Copyright 2024 Google LLC
 *
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "src/core/SkBlurEngine.h"

#include "include/core/SkAlphaType.h"
#include "include/core/SkBlendMode.h"
#include "include/core/SkClipOp.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkM44.h"
#include "include/core/SkMatrix.h"
#include "include/core/SkPaint.h"
#include "include/core/SkRect.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkScalar.h"
#include "include/core/SkShader.h"
#include "include/effects/SkRuntimeEffect.h"
#include "include/private/base/SkAssert.h"
#include "include/private/base/SkMath.h"
#include "include/private/base/SkTo.h"
#include "src/core/SkDevice.h"
#include "src/core/SkKnownRuntimeEffects.h"
#include "src/core/SkSpecialImage.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

void SkShaderBlurAlgorithm::Compute2DBlurKernel(SkSize sigma,
                                                SkISize radius,
                                                SkSpan<float> kernel) {
    // Callers likely had to calculate the radius prior to filling out the kernel value, which is
    // why it's provided; but make sure it's consistent with expectations.
    SkASSERT(SkBlurEngine::SigmaToRadius(sigma.width()) == radius.width() &&
             SkBlurEngine::SigmaToRadius(sigma.height()) == radius.height());

    // Callers are responsible for downscaling large sigmas to values that can be processed by the
    // effects, so ensure the radius won't overflow 'kernel'
    const int width = KernelWidth(radius.width());
    const int height = KernelWidth(radius.height());
    const size_t kernelSize = SkTo<size_t>(sk_64_mul(width, height));
    SkASSERT(kernelSize <= kernel.size());

    // And the definition of an identity blur should be sufficient that 2sigma^2 isn't near zero
    // when there's a non-trivial radius.
    const float twoSigmaSqrdX = 2.0f * sigma.width() * sigma.width();
    const float twoSigmaSqrdY = 2.0f * sigma.height() * sigma.height();
    SkASSERT((radius.width() == 0 || !SkScalarNearlyZero(twoSigmaSqrdX)) &&
             (radius.height() == 0 || !SkScalarNearlyZero(twoSigmaSqrdY)));

    // Setting the denominator to 1 when the radius is 0 automatically converts the remaining math
    // to the 1D Gaussian distribution. When both radii are 0, it correctly computes a weight of 1.0
    const float sigmaXDenom = radius.width() > 0 ? 1.0f / twoSigmaSqrdX : 1.f;
    const float sigmaYDenom = radius.height() > 0 ? 1.0f / twoSigmaSqrdY : 1.f;

    float sum = 0.0f;
    for (int x = 0; x < width; x++) {
        float xTerm = static_cast<float>(x - radius.width());
        xTerm = xTerm * xTerm * sigmaXDenom;
        for (int y = 0; y < height; y++) {
            float yTerm = static_cast<float>(y - radius.height());
            float xyTerm = std::exp(-(xTerm + yTerm * yTerm * sigmaYDenom));
            // Note that the constant term (1/(sqrt(2*pi*sigma^2)) of the Gaussian
            // is dropped here, since we renormalize the kernel below.
            kernel[y * width + x] = xyTerm;
            sum += xyTerm;
        }
    }
    // Normalize the kernel
    float scale = 1.0f / sum;
    for (size_t i = 0; i < kernelSize; ++i) {
        kernel[i] *= scale;
    }
    // Zero remainder of the array
    memset(kernel.data() + kernelSize, 0, sizeof(float)*(kernel.size() - kernelSize));
}

void SkShaderBlurAlgorithm::Compute2DBlurKernel(SkSize sigma,
                                                SkISize radii,
                                                std::array<SkV4, kMaxSamples/4>& kernel) {
    static_assert(sizeof(kernel) == sizeof(std::array<float, kMaxSamples>));
    static_assert(alignof(float) == alignof(SkV4));
    float* data = kernel[0].ptr();
    Compute2DBlurKernel(sigma, radii, SkSpan<float>(data, kMaxSamples));
}

void SkShaderBlurAlgorithm::Compute2DBlurOffsets(SkISize radius,
                                                 std::array<SkV4, kMaxSamples/2>& offsets) {
    const int kernelArea = KernelWidth(radius.width()) * KernelWidth(radius.height());
    SkASSERT(kernelArea <= kMaxSamples);

    SkSpan<float> offsetView{offsets[0].ptr(), kMaxSamples*2};

    int i = 0;
    for (int y = -radius.height(); y <= radius.height(); ++y) {
        for (int x = -radius.width(); x <= radius.width(); ++x) {
            offsetView[2*i]   = x;
            offsetView[2*i+1] = y;
            ++i;
        }
    }
    SkASSERT(i == kernelArea);
    const int lastValidOffset = 2*(kernelArea - 1);
    for (; i < kMaxSamples; ++i) {
        offsetView[2*i]   = offsetView[lastValidOffset];
        offsetView[2*i+1] = offsetView[lastValidOffset+1];
    }
}

void SkShaderBlurAlgorithm::Compute1DBlurLinearKernel(
        float sigma,
        int radius,
        std::array<SkV4, kMaxSamples/2>& offsetsAndKernel) {
    SkASSERT(sigma <= kMaxLinearSigma);
    SkASSERT(radius == SkBlurEngine::SigmaToRadius(sigma));
    SkASSERT(LinearKernelWidth(radius) <= kMaxSamples);

    // Given 2 adjacent gaussian points, they are blended as: Wi * Ci + Wj * Cj.
    // The GPU will mix Ci and Cj as Ci * (1 - x) + Cj * x during sampling.
    // Compute W', x such that W' * (Ci * (1 - x) + Cj * x) = Wi * Ci + Wj * Cj.
    // Solving W' * x = Wj, W' * (1 - x) = Wi:
    // W' = Wi + Wj
    // x = Wj / (Wi + Wj)
    auto get_new_weight = [](float* new_w, float* offset, float wi, float wj) {
        *new_w = wi + wj;
        *offset = wj / (wi + wj);
    };

    // Create a temporary standard kernel. The maximum blur radius that can be passed to this
    // function is (kMaxBlurSamples-1), so make an array large enough to hold the full kernel width.
    static constexpr int kMaxKernelWidth = KernelWidth(kMaxSamples - 1);
    SkASSERT(KernelWidth(radius) <= kMaxKernelWidth);
    std::array<float, kMaxKernelWidth> fullKernel;
    Compute1DBlurKernel(sigma, radius, SkSpan<float>{fullKernel.data(), KernelWidth(radius)});

    std::array<float, kMaxSamples> kernel;
    std::array<float, kMaxSamples> offsets;
    // Note that halfsize isn't just size / 2, but radius + 1. This is the size of the output array.
    int halfSize = LinearKernelWidth(radius);
    int halfRadius = halfSize / 2;
    int lowIndex = halfRadius - 1;

    // Compute1DGaussianKernel produces a full 2N + 1 kernel. Since the kernel can be mirrored,
    // compute only the upper half and mirror to the lower half.

    int index = radius;
    if (radius & 1) {
        // If N is odd, then use two samples.
        // The centre texel gets sampled twice, so halve its influence for each sample.
        // We essentially sample like this:
        // Texel edges
        // v    v    v    v
        // |    |    |    |
        // \-----^---/ Lower sample
        //      \---^-----/ Upper sample
        get_new_weight(&kernel[halfRadius],
                       &offsets[halfRadius],
                       fullKernel[index] * 0.5f,
                       fullKernel[index + 1]);
        kernel[lowIndex] = kernel[halfRadius];
        offsets[lowIndex] = -offsets[halfRadius];
        index++;
        lowIndex--;
    } else {
        // If N is even, then there are an even number of texels on either side of the centre texel.
        // Sample the centre texel directly.
        kernel[halfRadius] = fullKernel[index];
        offsets[halfRadius] = 0.0f;
    }
    index++;

    // Every other pair gets one sample.
    for (int i = halfRadius + 1; i < halfSize; index += 2, i++, lowIndex--) {
        get_new_weight(&kernel[i], &offsets[i], fullKernel[index], fullKernel[index + 1]);
        offsets[i] += static_cast<float>(index - radius);

        // Mirror to lower half.
        kernel[lowIndex] = kernel[i];
        offsets[lowIndex] = -offsets[i];
    }

    // Zero out remaining values in the kernel
    memset(kernel.data() + halfSize, 0, sizeof(float)*(kMaxSamples - halfSize));
    // But copy the last valid offset into the remaining offsets, to increase the chance that
    // over-iteration in a fragment shader will have a cache hit.
    for (int i = halfSize; i < kMaxSamples; ++i) {
        offsets[i] = offsets[halfSize - 1];
    }

    // Interleave into the output array to match the 1D SkSL effect
    for (int i = 0; i < kMaxSamples / 2; ++i) {
        offsetsAndKernel[i] = SkV4{offsets[2*i], kernel[2*i], offsets[2*i+1], kernel[2*i+1]};
    }
}

static SkKnownRuntimeEffects::StableKey to_stablekey(int kernelWidth, uint32_t baseKey) {
    SkASSERT(kernelWidth >= 2 && kernelWidth <= SkShaderBlurAlgorithm::kMaxSamples);
    switch(kernelWidth) {
        // Batch on multiples of 4 (skipping width=1, since that can't happen)
        case 2:  [[fallthrough]];
        case 3:  [[fallthrough]];
        case 4:  return static_cast<SkKnownRuntimeEffects::StableKey>(baseKey);
        case 5:  [[fallthrough]];
        case 6:  [[fallthrough]];
        case 7:  [[fallthrough]];
        case 8:  return static_cast<SkKnownRuntimeEffects::StableKey>(baseKey+1);
        case 9:  [[fallthrough]];
        case 10: [[fallthrough]];
        case 11: [[fallthrough]];
        case 12: return static_cast<SkKnownRuntimeEffects::StableKey>(baseKey+2);
        case 13: [[fallthrough]];
        case 14: [[fallthrough]];
        case 15: [[fallthrough]];
        case 16: return static_cast<SkKnownRuntimeEffects::StableKey>(baseKey+3);
        case 17: [[fallthrough]];
        case 18: [[fallthrough]];
        case 19: [[fallthrough]];
        // With larger kernels, batch on multiples of eight so up to 7 wasted samples.
        case 20: return static_cast<SkKnownRuntimeEffects::StableKey>(baseKey+4);
        case 21: [[fallthrough]];
        case 22: [[fallthrough]];
        case 23: [[fallthrough]];
        case 24: [[fallthrough]];
        case 25: [[fallthrough]];
        case 26: [[fallthrough]];
        case 27: [[fallthrough]];
        case 28: return static_cast<SkKnownRuntimeEffects::StableKey>(baseKey+5);
        default:
            SkUNREACHABLE;
    }
}

const SkRuntimeEffect* SkShaderBlurAlgorithm::GetLinearBlur1DEffect(int radius) {
    return GetKnownRuntimeEffect(
            to_stablekey(LinearKernelWidth(radius),
                         static_cast<uint32_t>(SkKnownRuntimeEffects::StableKey::k1DBlurBase)));
}

const SkRuntimeEffect* SkShaderBlurAlgorithm::GetBlur2DEffect(const SkISize& radii) {
    int kernelArea = KernelWidth(radii.width()) * KernelWidth(radii.height());
    return GetKnownRuntimeEffect(
            to_stablekey(kernelArea,
                         static_cast<uint32_t>(SkKnownRuntimeEffects::StableKey::k2DBlurBase)));
}

sk_sp<SkSpecialImage> SkShaderBlurAlgorithm::renderBlur(sk_sp<SkShader> blurEffect,
                                                        const SkIRect& dstRect,
                                                        SkColorType colorType,
                                                        sk_sp<SkColorSpace> colorSpace) const {
    SkImageInfo outII = SkImageInfo::Make({dstRect.width(), dstRect.height()},
                                          colorType, kPremul_SkAlphaType, std::move(colorSpace));
    sk_sp<SkDevice> device = this->makeDevice(outII);
    if (!device) {
        return nullptr;
    }

    // TODO(b/294102201): This is very much like AutoSurface in SkImageFilterTypes.cpp
    SkIRect subset = SkIRect::MakeSize(dstRect.size());
    device->clipRect(SkRect::Make(subset), SkClipOp::kIntersect, /*aa=*/false);
    device->setLocalToDevice(SkM44::Translate(-dstRect.left(), -dstRect.top()));
    SkPaint paint;
    paint.setBlendMode(SkBlendMode::kSrc);
    paint.setShader(std::move(blurEffect));
    device->drawPaint(paint);
    return device->snapSpecial(subset);
}

sk_sp<SkSpecialImage> SkShaderBlurAlgorithm::evalBlur2D(SkSize sigma,
                                                        SkISize radii,
                                                        sk_sp<SkSpecialImage> input,
                                                        const SkIRect& srcRect,
                                                        SkTileMode tileMode,
                                                        const SkIRect& dstRect) const {
    std::array<SkV4, kMaxSamples/4> kernel;
    std::array<SkV4, kMaxSamples/2> offsets;
    Compute2DBlurKernel(sigma, radii, kernel);
    Compute2DBlurOffsets(radii, offsets);

    SkRuntimeShaderBuilder builder{sk_ref_sp(GetBlur2DEffect(radii))};
    builder.uniform("kernel") = kernel;
    builder.uniform("offsets") = offsets;
    // TODO(b/294102201): This is very much like FilterResult::asShader()...
    builder.child("child") =
            input->makeSubset(srcRect)->asShader(tileMode,
                                                 SkFilterMode::kNearest,
                                                 SkMatrix::Translate(srcRect.left(),srcRect.top()));

    return this->renderBlur(builder.makeShader(), dstRect,
                            input->colorType(), input->colorInfo().refColorSpace());
}

sk_sp<SkSpecialImage> SkShaderBlurAlgorithm::evalBlur1D(float sigma,
                                                        int radius,
                                                        SkV2 dir,
                                                        sk_sp<SkSpecialImage> input,
                                                        SkIRect srcRect,
                                                        SkTileMode tileMode,
                                                        SkIRect dstRect) const {
    std::array<SkV4, kMaxSamples/2> offsetsAndKernel;
    Compute1DBlurLinearKernel(sigma, radius, offsetsAndKernel);

    SkRuntimeShaderBuilder builder{sk_ref_sp(GetLinearBlur1DEffect(radius))};
    builder.uniform("offsetsAndKernel") = offsetsAndKernel;
    builder.uniform("dir") = dir;
    // TODO(b/294102201): This is very much like FilterResult::asShader()...
    builder.child("child") =
            input->makeSubset(srcRect)->asShader(tileMode,
                                                 SkFilterMode::kLinear,
                                                 SkMatrix::Translate(srcRect.left(),srcRect.top()));

    return this->renderBlur(builder.makeShader(), dstRect,
                            input->colorType(), input->colorInfo().refColorSpace());
}

sk_sp<SkSpecialImage> SkShaderBlurAlgorithm::blur(SkSize sigma,
                                                  sk_sp<SkSpecialImage> src,
                                                  const SkIRect& srcRect,
                                                  SkTileMode tileMode,
                                                  const SkIRect& dstRect) const {
    SkASSERT(sigma.width() <= kMaxLinearSigma &&  sigma.height() <= kMaxLinearSigma);

    int radiusX = SkBlurEngine::SigmaToRadius(sigma.width());
    int radiusY = SkBlurEngine::SigmaToRadius(sigma.height());
    const int kernelArea = KernelWidth(radiusX) * KernelWidth(radiusY);
    if (kernelArea <= kMaxSamples && radiusX > 0 && radiusY > 0) {
        // Use a single-pass 2D kernel if it fits and isn't just 1D already
        return this->evalBlur2D(sigma,
                                {radiusX, radiusY},
                                std::move(src),
                                srcRect,
                                tileMode,
                                dstRect);
    } else {
        // Use two passes of a 1D kernel (one per axis).
        SkIRect intermediateSrcRect = srcRect;
        SkIRect intermediateDstRect = dstRect;
        if (radiusX > 0) {
            if (radiusY > 0) {
                // Outset the output size of dstRect by the radius required for the next Y pass
                intermediateDstRect.outset(0, radiusY);
                if (!intermediateDstRect.intersect(srcRect.makeOutset(radiusX, radiusY))) {
                    return nullptr;
                }
            }

            src = this->evalBlur1D(sigma.width(),
                                   radiusX,
                                   /*dir=*/{1.f, 0.f},
                                   std::move(src),
                                   srcRect,
                                   tileMode,
                                   intermediateDstRect);
            if (!src) {
                return nullptr;
            }
            intermediateSrcRect = SkIRect::MakeWH(src->width(), src->height());
            intermediateDstRect = dstRect.makeOffset(-intermediateDstRect.left(),
                                                     -intermediateDstRect.top());
        }

        if (radiusY > 0) {
            src = this->evalBlur1D(sigma.height(),
                                   radiusY,
                                   /*dir=*/{0.f, 1.f},
                                   std::move(src),
                                   intermediateSrcRect,
                                   tileMode,
                                   intermediateDstRect);
        }

        return src;
    }
}
