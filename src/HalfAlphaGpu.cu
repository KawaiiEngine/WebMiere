// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#include <cuda_runtime.h>

namespace {
// Fixed-point coefficients for the shipped swscale limited-range BT.709 path.
// Chroma contributions are quantized separately before luma table conversion.
__device__ int ChromaOffset(int value, int coefficient)
{
    return ((value * coefficient) >> 16) - (coefficient >> 9);
}

__device__ int Channel(int luma, int offset)
{
    const int value = (((326 + luma + offset) * 76309 + 32768) >> 16) - 400;
    return min(255, max(0, value));
}

struct CoverageSpan {
    int first, end;
    int left, middle, right, total;

    __device__ int Weight(int index) const
    {
        if (index == first) return left;
        return index + 1 == end ? right : middle;
    }
};

// Integer overlaps give the same source areas as alpha_resize::SourceSpan.
// Both axes may shrink independently, including an unchanged axis.
__device__ CoverageSpan SourceSpan(int index, int sourceSize, int outputSize)
{
    if (sourceSize % outputSize == 0) {
        const int count = sourceSize / outputSize;
        return {index * count, (index + 1) * count, 1, 1, 1, count};
    }
    const int begin = index * sourceSize;
    const int end = begin + sourceSize;
    const int first = begin / outputSize;
    const int last = (end - 1) / outputSize;
    return {first, last + 1, (first + 1) * outputSize - begin, outputSize,
            end - last * outputSize, sourceSize};
}

struct AlphaPlanes {
    const unsigned char *luma, *chroma, *alpha;
    int colorPitch, maskPitch, height;

    __device__ uchar4 ReadPixel(int x, int y) const
    {
        const int chromaY = y / 2;
        const int neighbor = min(height / 2 - 1, max(0, chromaY + ((y & 1) ? 1 : -1)));
        const auto uv = chroma + size_t(chromaY) * colorPitch + (x & ~1);
        const auto side = chroma + size_t(neighbor) * colorPitch + (x & ~1);
        const int u = (3 * uv[0] + side[0] + 2) / 4;
        const int v = (3 * uv[1] + side[1] + 2) / 4;
        const int value = luma[size_t(y) * colorPitch + x];
        return make_uchar4(Channel(value, ChromaOffset(u, 118894)),
                           Channel(value, ChromaOffset(u, -12001) + ChromaOffset(v, -29993)),
                           Channel(value, ChromaOffset(v, 100902)), alpha[size_t(y) * maskPitch + x]);
    }
};

template<typename T>
struct WeightedColor {
    T blue = 0, green = 0, red = 0, coverage = 0;

    __device__ void Add(uchar4 pixel, T weight)
    {
        const T alphaWeight = weight * pixel.w;
        coverage += alphaWeight;
        blue += pixel.x * alphaWeight;
        green += pixel.y * alphaWeight;
        red += pixel.z * alphaWeight;
    }

    __device__ uchar4 StraightBGRA(unsigned area) const
    {
        const unsigned alpha = (coverage + area / 2) / area;
        if (alpha == 0) return make_uchar4(0, 0, 0, 0);
        // Match the CPU area integrator's separately rounded multiply and add.
        const double inverse = __ddiv_rn(1.0, static_cast<double>(coverage));
        return make_uchar4(int(__dadd_rn(__dmul_rn(blue, inverse), 0.5)),
                           int(__dadd_rn(__dmul_rn(green, inverse), 0.5)),
                           int(__dadd_rn(__dmul_rn(red, inverse), 0.5)), alpha);
    }
};

__device__ uchar4 IntegerBox(const AlphaPlanes &source, int x, int y, int factor)
{
    // At most 32 x 32 equal weights, even 255 * 255 per pixel fits in 32 bits.
    WeightedColor<unsigned> sum;
    #pragma unroll 1
    for (int dy = 0; dy < factor; ++dy) {
        for (int dx = 0; dx < factor; ++dx)
            sum.Add(source.ReadPixel(x * factor + dx, y * factor + dy), 1);
    }
    return sum.StraightBGRA(factor * factor);
}

__device__ uchar4 AreaBox(const AlphaPlanes &source, int x, int y, int srcWidth, int width, int height)
{
    const CoverageSpan cols = SourceSpan(x, srcWidth, width);
    const CoverageSpan rows = SourceSpan(y, source.height, height);
    WeightedColor<unsigned long long> sum;
    for (int sy = rows.first; sy < rows.end; ++sy) {
        const unsigned long long rowWeight = rows.Weight(sy);
        for (int sx = cols.first; sx < cols.end; ++sx)
            sum.Add(source.ReadPixel(sx, sy), rowWeight * cols.Weight(sx));
    }
    return sum.StraightBGRA(cols.total * rows.total);
}
} // namespace

extern "C" __global__ void ResizeAlpha709(const unsigned char *luma, const unsigned char *chroma,
    const unsigned char *alpha, int colorPitch, int maskPitch, int width, int height,
    unsigned char *destination, size_t dstPitch, int srcWidth, int srcHeight, int firstRow, int rows)
{
    const int x = blockIdx.x * blockDim.x + threadIdx.x;
    const int bandY = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || bandY >= rows) return;
    const int y = firstRow + bandY;
    auto outputRow = reinterpret_cast<uchar4 *>(destination + size_t(rows - 1 - bandY) * dstPitch);

    if (srcWidth != 2 * width || srcHeight != 2 * height) {
        const AlphaPlanes planes{luma, chroma, alpha, colorPitch, maskPitch, srcHeight};
        const int factor = srcWidth / width;
        if (srcWidth == factor * width && srcHeight == factor * height)
            outputRow[x] = IntegerBox(planes, x, y, factor);
        else
            outputRow[x] = AreaBox(planes, x, y, srcWidth, width, height);
        return;
    }

    const int sx = 2 * x;
    const unsigned char *uv = chroma + size_t(y) * colorPitch + sx;
    int blue = 0, green = 0, red = 0, coverage = 0;
    for (int dy = 0; dy < 2; ++dy) {
        const int neighbor = dy ? min(y + 1, height - 1) : max(y - 1, 0);
        const unsigned char *side = chroma + size_t(neighbor) * colorPitch + sx;
        const int u = (3 * uv[0] + side[0] + 2) / 4;
        const int v = (3 * uv[1] + side[1] + 2) / 4;
        const int bo = ChromaOffset(u, 118894);
        const int go = ChromaOffset(u, -12001) + ChromaOffset(v, -29993);
        const int ro = ChromaOffset(v, 100902);
        const size_t sy = 2 * y + dy;
        for (int dx = 0; dx < 2; ++dx) {
            const int a = alpha[sy * maskPitch + sx + dx];
            const int l = luma[sy * colorPitch + sx + dx];
            blue += a * Channel(l, bo);
            green += a * Channel(l, go);
            red += a * Channel(l, ro);
            coverage += a;
        }
    }
    const int a = (coverage + 2) / 4;
    uchar4 result = make_uchar4(0, 0, 0, a);
    if (a) {
        result.x = (blue + coverage / 2) / coverage;
        result.y = (green + coverage / 2) / coverage;
        result.z = (red + coverage / 2) / coverage;
    }
    outputRow[x] = result;
}
