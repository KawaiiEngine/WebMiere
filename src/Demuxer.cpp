
// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.



#define __STDC_CONSTANT_MACROS

#include "Demuxer.h"
#include "WebMiereLimits.h"
#include "WebMiereColorPolicy.h"

#include <windows.h>
#include <cmath>
#include <cstring>
#include <memory>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
}


namespace {
struct AVFormatInputDeleter
{
	void operator()(AVFormatContext *p) const noexcept
	{
		if (p)
		{
			avformat_close_input(&p);
		}
	}
};
using AVFormatInputGuard = std::unique_ptr<AVFormatContext, AVFormatInputDeleter>;
}


static void Utf16ToUtf8(const prUTF16Char *src, std::string &out)
{
	out.clear();
	if (src == nullptr)
	{
		return;
	}
	const wchar_t *w = reinterpret_cast<const wchar_t*>(src);
	int needed = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (needed <= 1)
	{
		return;
	}
	out.resize(static_cast<size_t>(needed));
	int written = WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], needed, nullptr, nullptr);
	if (written <= 1)
	{
		out.clear();
		return;
	}
	out.resize(static_cast<size_t>(written - 1));
}


static WebMiereCodec MapCodec(int avCodecId)
{
	switch (avCodecId)
	{
		case AV_CODEC_ID_VP9:	return WEBMIERE_CODEC_VP9;
		case AV_CODEC_ID_AV1:	return WEBMIERE_CODEC_AV1;
		case AV_CODEC_ID_OPUS:	return WEBMIERE_CODEC_OPUS;
		default:				return WEBMIERE_CODEC_OTHER;
	}
}


static bool IsSupportedVideoCodec(WebMiereCodec codec)
{
	return codec == WEBMIERE_CODEC_VP9 ||
		   codec == WEBMIERE_CODEC_AV1;
}


static double StreamDurationSeconds(AVFormatContext *fmt, AVStream *st)
{
	if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
	{
		return static_cast<double>(st->duration) * av_q2d(st->time_base);
	}
	if (fmt->duration != AV_NOPTS_VALUE && fmt->duration > 0)
	{
		return static_cast<double>(fmt->duration) / static_cast<double>(AV_TIME_BASE);
	}
	return 0.0;
}


bool ProbeMedia(const prUTF16Char *path, MediaProbeInfo *out, std::string *errMsg)
{
	if (out == nullptr)
	{
		return false;
	}
	std::memset(out, 0, sizeof(*out));


	std::string utf8Path;
	Utf16ToUtf8(path, utf8Path);
	if (utf8Path.empty())
	{
		if (errMsg) *errMsg = "empty/invalid path";
		return false;
	}

	AVFormatContext *rawFmt = nullptr;
	int ret = avformat_open_input(&rawFmt, utf8Path.c_str(), nullptr, nullptr);
	if (ret < 0)
	{
		if (errMsg)
		{
			char buf[256] = {0};
			av_strerror(ret, buf, sizeof(buf));
			*errMsg = std::string("avformat_open_input failed: ") + buf;
		}
		return false;
	}
	AVFormatInputGuard fmt(rawFmt);

	ret = avformat_find_stream_info(fmt.get(), nullptr);
	if (ret < 0)
	{
		if (errMsg)
		{
			char buf[256] = {0};
			av_strerror(ret, buf, sizeof(buf));
			*errMsg = std::string("avformat_find_stream_info failed: ") + buf;
		}
		return false;
	}

	int vIdx = av_find_best_stream(fmt.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);


	if (vIdx < 0)
	{
		if (errMsg) *errMsg = "no video stream";
		return false;
	}

	{
		AVStream			*st  = fmt->streams[vIdx];
		AVCodecParameters	*par = st->codecpar;

		out->hasVideo		= true;
		out->videoCodec		= MapCodec(par->codec_id);
		out->width			= par->width;
		out->height			= par->height;
		out->avPixelFormat	= par->format;

		if (!IsSupportedVideoCodec(out->videoCodec))
		{
			if (errMsg) *errMsg = "video codec is not VP9 or AV1";
			return false;
		}
		if (!Vp9oIsValidVideoSize(par->width, par->height))
		{
			if (errMsg) *errMsg = "invalid video dimensions";
			return false;
		}

		const bool isSupportedVideo = IsSupportedVideoCodec(out->videoCodec);
		const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format));
		if (isSupportedVideo && (par->format == AV_PIX_FMT_NONE || desc == nullptr))
		{
			if (errMsg) *errMsg = "unknown pixel format";
			return false;
		}
		if (isSupportedVideo && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0)
		{
			if (errMsg) *errMsg = "hardware-only pixel format not supported";
			return false;
		}
		if (isSupportedVideo && (desc->flags & AV_PIX_FMT_FLAG_RGB) != 0)
		{
			if (errMsg) *errMsg = "RGB pixel format not supported";
			return false;
		}
		if (isSupportedVideo && (desc->flags & AV_PIX_FMT_FLAG_ALPHA) != 0)
		{
			if (errMsg) *errMsg = "alpha not supported";
			return false;
		}

		out->isYuv420p8 = (desc != nullptr) &&
						  ((par->format == AV_PIX_FMT_YUV420P) ||
						   (desc->comp[0].depth == 8 &&
						    desc->log2_chroma_w == 1 && desc->log2_chroma_h == 1 &&
						    (desc->flags & AV_PIX_FMT_FLAG_RGB) == 0));
		if (isSupportedVideo && !out->isYuv420p8)
		{
			if (errMsg) *errMsg = "pixel format is not 8-bit 4:2:0";
			return false;
		}

		if (isSupportedVideo && !Vp9oIsStrictBt709(par))
		{
			if (errMsg) *errMsg = "only explicitly tagged BT.709 limited-range video is supported";
			return false;
		}


		AVRational fr = st->avg_frame_rate;
		if (fr.num <= 0 || fr.den <= 0)
		{
			fr = st->r_frame_rate;
		}
		if (fr.num <= 0 || fr.den <= 0)
		{
			fr.num = 60;
			fr.den = 1;
		}
		out->frameRateNum = fr.num;
		out->frameRateDen = fr.den;


		int64_t nb = st->nb_frames;
		if (nb <= 0)
		{
			double durSec = StreamDurationSeconds(fmt.get(), st);
			nb = static_cast<int64_t>(std::llround(durSec * av_q2d(fr)));
		}
		if (nb <= 0)
		{
			nb = 1;
		}
		out->numFrames = nb;
	}


	for (unsigned int streamIndex = 0; streamIndex < fmt->nb_streams; streamIndex++)
	{
		AVStream			*st  = fmt->streams[streamIndex];
		if (st == nullptr || st->codecpar == nullptr ||
			st->codecpar->codec_type != AVMEDIA_TYPE_AUDIO)
		{
			continue;
		}
		if (out->audioStreamCount >= kVp9oMaxAudioStreams)
		{
			if (errMsg) *errMsg = "more than 6 audio streams";
			return false;
		}

		AVCodecParameters	*par = st->codecpar;
		MediaProbeInfo::AudioStreamProbeInfo &audio =
			out->audioStreams[out->audioStreamCount];

		audio.ffmpegStreamIndex		= static_cast<int>(streamIndex);
		audio.codec					= MapCodec(par->codec_id);
		audio.sampleRate				= par->sample_rate;
		audio.channels				= par->ch_layout.nb_channels;
		audio.startTime				= st->start_time;
		audio.timeBaseNum			= st->time_base.num;
		audio.timeBaseDen			= st->time_base.den;
		audio.initialPaddingSamples	= par->initial_padding;
		audio.seekPrerollSamples		= par->seek_preroll;

		if (audio.codec != WEBMIERE_CODEC_OPUS ||
			par->ch_layout.nb_channels != 2 ||
			par->sample_rate != 48000)
		{
			if (errMsg) *errMsg = "audio stream is not Opus stereo 48kHz";
			return false;
		}

		double durSec = StreamDurationSeconds(fmt.get(), st);
		audio.sourceSampleFrames =
			static_cast<int64_t>(std::llround(durSec * static_cast<double>(par->sample_rate)));
		if (audio.sourceSampleFrames < 0)
		{
			audio.sourceSampleFrames = 0;
		}
		out->audioStreamCount++;
	}

	if (out->audioStreamCount > 1)
	{
		const MediaProbeInfo::AudioStreamProbeInfo &primary = out->audioStreams[0];
		if (primary.startTime == AV_NOPTS_VALUE ||
			primary.timeBaseNum <= 0 || primary.timeBaseDen <= 0)
		{
			if (errMsg) *errMsg = "missing audio start timestamp";
			return false;
		}
		const AVRational sampleBase = { 1, 48000 };
		const AVRational primaryTimeBase = { primary.timeBaseNum, primary.timeBaseDen };
		const int64_t primaryStartSample =
			av_rescale_q(primary.startTime, primaryTimeBase, sampleBase);

		for (int i = 1; i < out->audioStreamCount; i++)
		{
			const MediaProbeInfo::AudioStreamProbeInfo &audio = out->audioStreams[i];
			if (audio.startTime == AV_NOPTS_VALUE ||
				audio.timeBaseNum <= 0 || audio.timeBaseDen <= 0)
			{
				if (errMsg) *errMsg = "missing audio start timestamp";
				return false;
			}
			const AVRational audioTimeBase = { audio.timeBaseNum, audio.timeBaseDen };
			const int64_t audioStartSample =
				av_rescale_q(audio.startTime, audioTimeBase, sampleBase);
			if (audioStartSample != primaryStartSample)
			{
				if (errMsg) *errMsg = "audio streams do not share the same start sample";
				return false;
			}
		}
	}

	return true;
}
