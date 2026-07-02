
// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.



#ifndef VP9OPUS_DEMUXER_H
#define VP9OPUS_DEMUXER_H

#include <cstdint>
#include <string>


#include "PrSDKTypes.h"


enum Vp9oCodec
{
	VP9O_CODEC_UNKNOWN = 0,
	VP9O_CODEC_VP9,
	VP9O_CODEC_OPUS,
	VP9O_CODEC_OTHER
};


struct MediaProbeInfo
{

	bool		hasVideo;
	Vp9oCodec	videoCodec;
	int			width;
	int			height;
	int			frameRateNum;
	int			frameRateDen;
	int64_t		numFrames;
	int			avPixelFormat;
	bool		isYuv420p8;


	bool		hasAudio;
	Vp9oCodec	audioCodec;
	int			sampleRate;
	int			channels;
	int64_t		numAudioSampleFrames;
};


bool ProbeMedia(const prUTF16Char *path, MediaProbeInfo *out, std::string *errMsg);

#endif
