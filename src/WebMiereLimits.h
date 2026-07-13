// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef WEBMIERE_LIMITS_H
#define WEBMIERE_LIMITS_H

#include <cstdint>

constexpr int kVp9oMaxVideoWidth  = 8192;
constexpr int kVp9oMaxVideoHeight = 4320;
constexpr int kVp9oMaxAudioStreams = 6;
constexpr int64_t kVp9oMaxVideoPixels =
	static_cast<int64_t>(kVp9oMaxVideoWidth) * kVp9oMaxVideoHeight;

inline bool Vp9oIsValidVideoSize(int width, int height) noexcept
{
	return width > 0 &&
		   height > 0 &&
		   width <= kVp9oMaxVideoWidth &&
		   height <= kVp9oMaxVideoHeight &&
		   static_cast<int64_t>(width) * static_cast<int64_t>(height) <= kVp9oMaxVideoPixels;
}

inline bool Vp9oIsValidYuv420Size(int width, int height) noexcept
{
	return Vp9oIsValidVideoSize(width, height) &&
		   (width & 1) == 0 &&
		   (height & 1) == 0;
}

#endif
