// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.

#ifndef WEBMIERE_COLOR_POLICY_H
#define WEBMIERE_COLOR_POLICY_H

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/pixfmt.h>
}

inline bool WebMiereHasAlphaMode(const AVStream *stream) noexcept
{
	const AVDictionaryEntry *tag = av_dict_get(stream->metadata, "alpha_mode", nullptr, 0);
	return tag && tag->value[0] == '1' && tag->value[1] == '\0';
}

inline bool WebMiereIsStrictBt709(const AVCodecParameters *par) noexcept
{
	return par != nullptr &&
		par->color_space     == AVCOL_SPC_BT709 &&
		par->color_range     == AVCOL_RANGE_MPEG;
}

#endif
