// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef WEBMIERE_COLOR_POLICY_H
#define WEBMIERE_COLOR_POLICY_H

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

inline bool Vp9oIsStrictBt709(const AVCodecParameters *par) noexcept
{
	return par != nullptr &&
		par->color_space     == AVCOL_SPC_BT709 &&
		par->color_range     == AVCOL_RANGE_MPEG;
}

#endif
