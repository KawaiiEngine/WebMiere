// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#include "HalfAlphaConverter.h"
#include "AlphaResize.h"

#include <algorithm>
#include <cstddef>
#include <execution>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

int AlphaDownloadLayout::Transfer(AVFrame *host, const AVFrame *device)
{
	const auto *pool = device->hw_frames_ctx
		? reinterpret_cast<const AVHWFramesContext*>(device->hw_frames_ctx->data) : nullptr;
	if (!pool || pool->sw_format != AV_PIX_FMT_NV12) {
		// Preserve FFmpeg's format selection for other surface types.
		av_frame_unref(host);
		allocatedWidth = allocatedHeight = 0;
		return av_hwframe_transfer_data(host, device, 0);
	}
	if (device->width <= 0 || device->height <= 0 ||
		device->width > pool->width || device->height > pool->height)
		return AVERROR(EINVAL);

	if (!host->buf[0] || allocatedWidth != pool->width || allocatedHeight != pool->height) {
		av_frame_unref(host);
		host->format = AV_PIX_FMT_NV12;
		host->width = pool->width;
		host->height = pool->height;
		int result = av_frame_get_buffer(host, 0);
		if (result < 0) return result;
		allocatedWidth = pool->width;
		allocatedHeight = pool->height;
	}
	host->width = device->width;
	host->height = device->height;
	return av_hwframe_transfer_data(host, device, 0);
}

bool EnsureAlphaBgraBuffer(AVFrame *&buffer, int width, int rows)
{
	if (!buffer) buffer = av_frame_alloc();
	if (!buffer) return false;
	if (buffer->buf[0] && buffer->width == width && buffer->height >= rows) return true;

	av_frame_unref(buffer);
	buffer->format = AV_PIX_FMT_BGRA;
	buffer->width = width;
	buffer->height = rows;
	return av_frame_get_buffer(buffer, 0) >= 0;
}

HalfAlphaConverter::~HalfAlphaConverter()
{
	for (Band &band : mBands) sws_freeContext(band.scaler);
}

bool HalfAlphaConverter::Convert(const AVFrame *color, const AVFrame *mask, AVFrame *&scratch,
								 uint8_t *dst, int dstPitch, int flags, const char *&fallbackReason)
{
	const int width = color->width, height = color->height;
	const int dstHeight = height / 2;
	const int bandCount = static_cast<int>(mBands.size());
	int scratchRows = 0;
	for (int i = 0; i < bandCount; ++i) {
		Band &band = mBands[i];
		band.firstRow = i * dstHeight / bandCount;
		band.endRow = (i + 1) * dstHeight / bandCount;
		// Include neighboring chroma rows and preserve the full frame's row phase.
		band.inputStart = std::max(0, (2 * band.firstRow - 4) / 8 * 8);
		const int inputEnd = std::min(height, (2 * band.endRow + 4 + 7) / 8 * 8);
		const int rows = inputEnd - band.inputStart;
		band.scratchRow = scratchRows;
		// Leave padding after every band for vectorized swscale stores.
		scratchRows += rows + 1;
		if (band.scaler && band.width == width && band.rows == rows) continue;
		band.width = width;
		band.rows = rows;
		band.scaler = sws_getCachedContext(band.scaler, width, rows, AV_PIX_FMT_NV12,
			width, rows, AV_PIX_FMT_BGRA, flags, nullptr, nullptr, nullptr);
		if (!band.scaler) {
			fallbackReason = "sws_context";
			return false;
		}
		// Use the same matrix and range as ConvertColorToBGRA on the downloaded frame.
		const int *matrix = sws_getCoefficients(SWS_CS_ITU709);
		const int srcRange = color->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
		sws_setColorspaceDetails(band.scaler, matrix, srcRange, matrix, 1, 0, 1 << 16, 1 << 16);
	}

	if (!EnsureAlphaBgraBuffer(scratch, width, scratchRows)) return false;
	std::for_each(std::execution::par, mBands.begin(), mBands.end(), [&](Band &band) noexcept {
		const uint8_t *planes[4] = {
			color->data[0] + static_cast<ptrdiff_t>(band.inputStart) * color->linesize[0],
			color->data[1] + static_cast<ptrdiff_t>(band.inputStart / 2) * color->linesize[1]
		};
		uint8_t *bandData[4] = {
			scratch->data[0] + static_cast<ptrdiff_t>(band.scratchRow) * scratch->linesize[0]
		};
		band.converted = sws_scale(band.scaler, planes, color->linesize, 0, band.rows,
								   bandData, scratch->linesize) == band.rows;
		if (!band.converted) return;
		for (int y = band.firstRow; y < band.endRow; ++y) {
			const uint8_t *top = bandData[0] + static_cast<ptrdiff_t>(2 * y - band.inputStart) * scratch->linesize[0];
			const uint8_t *alpha = mask->data[0] + static_cast<ptrdiff_t>(2 * y) * mask->linesize[0];
			uint8_t *out = dst + static_cast<ptrdiff_t>(dstHeight - 1 - y) * dstPitch;
			webmiere::alpha_resize::HalfRow(top, top + scratch->linesize[0], alpha, alpha + mask->linesize[0], out, width / 2);
		}
	});
	for (const Band &band : mBands) {
		if (!band.converted) {
			fallbackReason = "sws_scale";
			return false;
		}
	}
	return true;
}
