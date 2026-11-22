// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <algorithm>
#include <array>
#include <execution>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace webmiere {
namespace alpha_resize {

// For reduction, integrate the covered source pixels. For enlargement,
// interpolate between pixel centers, extending edge samples at the borders.
struct Span {
	int first, last;
	int leftWeight, innerWeight, rightWeight, totalWeight;

	int Weight(int index) const noexcept
	{
		if (index == first) return leftWeight;
		return index == last ? rightWeight : innerWeight;
	}
};

inline Span SourceSpan(int index, int src, int dst) noexcept
{
	if (src >= dst) {
		if (src % dst == 0) {
			const int count = src / dst;
			return {index * count, (index + 1) * count - 1, 1, 1, 1, count};
		}
		const int start = index * src, end = start + src;
		const int first = start / dst, last = (end - 1) / dst;
		return {first, last, (first + 1) * dst - start, dst, end - last * dst, src};
	}
	const int unit = 2 * dst;
	const int center = (2 * index + 1) * src - dst;
	if (center <= 0) return {0, 0, 1, 1, 1, 1};
	if (center >= (src - 1) * unit) return {src - 1, src - 1, 1, 1, 1, 1};
	const int first = center / unit, fraction = center % unit;
	return {first, first + 1, unit - fraction, 0, fraction, unit};
}

// The common half-resolution request has four equal spatial weights.
inline void HalfRow(const uint8_t *top, const uint8_t *bottom, const uint8_t *topMask,
					const uint8_t *bottomMask, uint8_t *out, int width) noexcept
{
	for (int x = 0; x < width; ++x, top += 8, bottom += 8, topMask += 2, bottomMask += 2, out += 4) {
		const unsigned a = topMask[0], b = topMask[1], c = bottomMask[0], d = bottomMask[1];
		const unsigned sum = a + b + c + d;
		out[3] = static_cast<uint8_t>((sum + 2) / 4);
		if (!out[3]) {
			out[0] = out[1] = out[2] = 0;
		} else if (a == b && a == c && a == d) {
			for (int k = 0; k < 3; ++k)
				out[k] = static_cast<uint8_t>((top[k] + top[k+4] + bottom[k] + bottom[k+4] + 2) / 4);
		} else {
			for (int k = 0; k < 3; ++k) {
				const unsigned weighted = a*top[k] + b*top[k+4] + c*bottom[k] + d*bottom[k+4];
				out[k] = static_cast<uint8_t>((weighted + sum/2) / sum);
			}
		}
	}
}

inline void Resize(const uint8_t *bgra, int colorPitch, const uint8_t *alpha, int alphaPitch,
				   int srcW, int srcH, uint8_t *dst, int dstPitch, int dstW, int dstH)
{
	const bool half = srcW == 2 * dstW && srcH == 2 * dstH;
	std::vector<Span> columns(half ? 0 : dstW);
	if (!half) {
		for (int x = 0; x < dstW; ++x) columns[x] = SourceSpan(x, srcW, dstW);
	}
	auto renderRow = [&](int y) noexcept {
		const Span rows = SourceSpan(y, srcH, dstH);
		uint8_t *out = dst + static_cast<ptrdiff_t>(dstH - 1 - y) * dstPitch;
		if (half) {
			const uint8_t *top = bgra + static_cast<ptrdiff_t>(2 * y) * colorPitch;
			const uint8_t *mask = alpha + static_cast<ptrdiff_t>(2 * y) * alphaPitch;
			HalfRow(top, top + colorPitch, mask, mask + alphaPitch, out, dstW);
			return;
		}
		for (const Span &cols : columns) {
			uint64_t coverage = 0, blue = 0, green = 0, red = 0;
			for (int sy = rows.first; sy <= rows.last; ++sy) {
				const uint8_t *colorRow = bgra + static_cast<ptrdiff_t>(sy) * colorPitch;
				const uint8_t *maskRow = alpha + static_cast<ptrdiff_t>(sy) * alphaPitch;
				const unsigned wy = rows.Weight(sy);
				for (int sx = cols.first; sx <= cols.last; ++sx) {
					const uint64_t w = static_cast<uint64_t>(wy) * cols.Weight(sx) * maskRow[sx];
					const uint8_t *pixel = colorRow + 4 * sx;
					coverage += w;
					blue += pixel[0] * w;
					green += pixel[1] * w;
					red += pixel[2] * w;
				}
			}
			const uint64_t area = static_cast<uint64_t>(rows.totalWeight) * cols.totalWeight;
			out[3] = static_cast<uint8_t>((coverage + area / 2) / area);
			if (out[3]) {
				// Divide only once. Keeping the products unquantized preserves faint colors.
				const double inv = 1.0 / static_cast<double>(coverage);
				out[0] = static_cast<uint8_t>(blue * inv + 0.5);
				out[1] = static_cast<uint8_t>(green * inv + 0.5);
				out[2] = static_cast<uint8_t>(red * inv + 0.5);
			} else {
				out[0] = out[1] = out[2] = 0;
			}
			out += 4;
		}
	};
	if (srcW * srcH < 1024 * 1024 || dstH < 16) {
		for (int y = 0; y < dstH; ++y) renderRow(y);
		return;
	}
	// Bound parallelism when Premiere requests more than one clip at a time.
	const std::array<int, 8> bands = {0, 1, 2, 3, 4, 5, 6, 7};
	std::for_each(std::execution::par, bands.begin(), bands.end(), [&](int band) noexcept {
		for (int y = dstH * band / 8; y < dstH * (band + 1) / 8; ++y) renderRow(y);
	});
}
} // namespace alpha_resize
} // namespace webmiere
