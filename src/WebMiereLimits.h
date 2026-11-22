// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef WEBMIERE_LIMITS_H
#define WEBMIERE_LIMITS_H

#include <cstdint>

constexpr int kWebMiereMaxVideoWidth  = 8192;
constexpr int kWebMiereMaxVideoHeight = 4320;
constexpr int kWebMiereMaxAudioStreams = 6;
constexpr int64_t kWebMiereMaxVideoPixels =
	static_cast<int64_t>(kWebMiereMaxVideoWidth) * kWebMiereMaxVideoHeight;

inline bool WebMiereIsValidVideoSize(int width, int height) noexcept
{
	return width > 0 &&
		   height > 0 &&
		   width <= kWebMiereMaxVideoWidth &&
		   height <= kWebMiereMaxVideoHeight &&
		   static_cast<int64_t>(width) * static_cast<int64_t>(height) <= kWebMiereMaxVideoPixels;
}

inline bool WebMiereIsValidYuv420Size(int width, int height) noexcept
{
	return WebMiereIsValidVideoSize(width, height) &&
		   (width & 1) == 0 &&
		   (height & 1) == 0;
}

#endif
