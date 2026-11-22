// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
#pragma once

#include <array>
#include <cstdint>

struct AVFrame;
struct SwsContext;

// Allocation geometry for one decoder-owned software AVFrame. The AVFrame's
// width and height describe the visible picture, which can be smaller.
struct AlphaDownloadLayout
{
	int allocatedWidth = 0;
	int allocatedHeight = 0;
	int Transfer(AVFrame *host, const AVFrame *device);
};

// The same BGRA workspace serves both full-frame conversion and four disjoint bands.
bool EnsureAlphaBgraBuffer(AVFrame *&buffer, int width, int rows);

class HalfAlphaConverter
{
public:
	HalfAlphaConverter() = default;
	~HalfAlphaConverter();
	HalfAlphaConverter(const HalfAlphaConverter&) = delete;
	HalfAlphaConverter& operator=(const HalfAlphaConverter&) = delete;

	bool Convert(const AVFrame *color, const AVFrame *mask, AVFrame *&scratch, uint8_t *dst, int dstPitch,
				 int flags, const char *&fallbackReason);

private:
	struct Band
	{
		SwsContext *scaler = nullptr;
		int width = 0, rows = 0, scratchRow = 0;
		int firstRow = 0, endRow = 0, inputStart = 0;
		bool converted = false;
	};
	std::array<Band, 4> mBands{};
};
