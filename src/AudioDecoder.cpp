
// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.



#define __STDC_CONSTANT_MACROS

#include "AudioDecoder.h"

#include <windows.h>
#include <string>
#include <cstring>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/mathematics.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <libswresample/swresample.h>
}


static std::string Utf16ToUtf8(const prUTF16Char *src)
{
	if (src == nullptr)
	{
		return std::string();
	}
	const wchar_t *w = reinterpret_cast<const wchar_t*>(src);
	int needed = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
	if (needed <= 1)
	{
		return std::string();
	}
	std::string out(static_cast<size_t>(needed), '\0');
	int written = WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], needed, nullptr, nullptr);
	if (written <= 1)
	{
		return std::string();
	}
	out.resize(static_cast<size_t>(written - 1));
	return out;
}


AudioDecoder::AudioDecoder()
	: mFmt(nullptr)
	, mCodecCtx(nullptr)
	, mFrame(nullptr)
	, mPacket(nullptr)
	, mPendingPacket(nullptr)
	, mHasPendingPacket(false)
	, mSwr(nullptr)
	, mAudioStream(-1)
	, mSampleRate(48000)
	, mOutChannels(2)
	, mTimeBaseNum(1)
	, mTimeBaseDen(48000)
	, mAudioStartTime(0)
	, mInitialPaddingSamples(0)
	, mSeekPrerollSamples(0)
	, mSwrInFmt(-1)
	, mSwrInRate(0)
	, mSwrInCh(0)
	, mFront(0)
	, mHeadSample(-1)
	, mResync(true)
	, mEof(false)
	, mLastReadError(0)
	, mFlushSent(false)
	, mOpened(false)
{
}

AudioDecoder::~AudioDecoder()
{
	Close();
}


void AudioDecoder::Close()
{
	if (mSwr)
	{
		swr_free(&mSwr);
	}
	if (mPacket)
	{
		av_packet_free(&mPacket);
	}
	if (mPendingPacket)
	{
		av_packet_free(&mPendingPacket);
	}
	mHasPendingPacket = false;
	if (mFrame)
	{
		av_frame_free(&mFrame);
	}
	if (mCodecCtx)
	{
		avcodec_free_context(&mCodecCtx);
	}
	if (mFmt)
	{
		avformat_close_input(&mFmt);
	}

	mLeftover.clear();
	mScratch.clear();
	mAudioStream	= -1;
	mFront			= 0;
	mHeadSample		= -1;
	mInitialPaddingSamples = 0;
	mSeekPrerollSamples = 0;
	mResync			= true;
	mEof			= false;
	mLastReadError	= 0;
	mFlushSent		= false;
	mSwrInFmt		= -1;
	mSwrInRate		= 0;
	mSwrInCh		= 0;
	mOpened			= false;
}


bool AudioDecoder::Open(const prUTF16Char *path)
{
	Close();

	std::string utf8 = Utf16ToUtf8(path);
	if (utf8.empty())
	{
		return false;
	}

	if (avformat_open_input(&mFmt, utf8.c_str(), nullptr, nullptr) < 0)
	{
		Close();
		return false;
	}
	if (avformat_find_stream_info(mFmt, nullptr) < 0)
	{
		Close();
		return false;
	}

	const AVCodec *dec = nullptr;
	mAudioStream = av_find_best_stream(mFmt, AVMEDIA_TYPE_AUDIO, -1, -1, &dec, 0);
	if (mAudioStream < 0 || dec == nullptr)
	{
		Close();
		return false;
	}

	AVStream *st = mFmt->streams[mAudioStream];


	if (st->codecpar->codec_id != AV_CODEC_ID_OPUS)
	{
		Close();
		return false;
	}

	if (st->codecpar->ch_layout.nb_channels != 2)
	{
		Close();
		return false;
	}

	if (st->codecpar->sample_rate != 48000)
	{
		Close();
		return false;
	}

	mCodecCtx = avcodec_alloc_context3(dec);
	if (mCodecCtx == nullptr)
	{
		Close();
		return false;
	}
	if (avcodec_parameters_to_context(mCodecCtx, st->codecpar) < 0)
	{
		Close();
		return false;
	}
	if (avcodec_open2(mCodecCtx, dec, nullptr) < 0)
	{
		Close();
		return false;
	}

	mSampleRate  = st->codecpar->sample_rate;
	if (mSampleRate <= 0)
	{
		mSampleRate = 48000;
	}
	mOutChannels = 2;
	mTimeBaseNum = st->time_base.num;
	mTimeBaseDen = st->time_base.den;
	const int64_t rawStartTime = (st->start_time == AV_NOPTS_VALUE) ? 0 : st->start_time;
	mAudioStartTime = rawStartTime;
	mInitialPaddingSamples = (st->codecpar->initial_padding > 0)
		? static_cast<int64_t>(st->codecpar->initial_padding)
		: 0;
	const int64_t fallbackPreroll = static_cast<int64_t>(mSampleRate) * 80 / 1000;
	mSeekPrerollSamples = (st->codecpar->seek_preroll > 0)
		? static_cast<int64_t>(st->codecpar->seek_preroll)
		: fallbackPreroll;
	const int64_t maxPreroll = static_cast<int64_t>(mSampleRate);
	if (mSeekPrerollSamples > maxPreroll)
	{
		mSeekPrerollSamples = maxPreroll;
	}

	mFrame         = av_frame_alloc();
	mPacket        = av_packet_alloc();
	mPendingPacket = av_packet_alloc();
	mHasPendingPacket = false;
	if (mFrame == nullptr || mPacket == nullptr || mPendingPacket == nullptr)
	{
		Close();
		return false;
	}

	mLeftover.assign(static_cast<size_t>(mOutChannels), std::vector<float>());
	mScratch.assign(static_cast<size_t>(mOutChannels), std::vector<float>());
	mFront		= 0;
	mHeadSample	= -1;
	mResync		= true;
	mEof		= false;
	mLastReadError = 0;
	mFlushSent	= false;
	mOpened		= true;

	return true;
}


int64_t AudioDecoder::SampleIndexOf(const AVFrame *f) const
{
	int64_t pts = f->best_effort_timestamp;
	if (pts == AV_NOPTS_VALUE)
	{
		pts = f->pts;
	}
	if (pts == AV_NOPTS_VALUE)
	{

		return (mHeadSample >= 0) ? (mHeadSample + LeftoverCount()) : 0;
	}
	AVRational tb     = { mTimeBaseNum, mTimeBaseDen };
	AVRational srBase = { 1, mSampleRate };
	pts -= mAudioStartTime;
	if (pts < 0)
	{
		pts = 0;
	}
	else if (pts > 0 && mHeadSample < 0 && mInitialPaddingSamples > 0)
	{
		const int64_t paddingTime = av_rescale_q_rnd(
			mInitialPaddingSamples,
			srBase,
			tb,
			AV_ROUND_UP);
		if (pts <= paddingTime + 1)
		{
			pts = 0;
		}
	}
	const int64_t sample = av_rescale_q(pts, tb, srBase);
	return (sample > 0) ? sample : 0;
}

int64_t AudioDecoder::LeftoverCount() const
{
	if (mLeftover.empty())
	{
		return 0;
	}
	const int64_t total = static_cast<int64_t>(mLeftover[0].size());
	const int64_t front = static_cast<int64_t>(mFront);
	return (total > front) ? (total - front) : 0;
}

void AudioDecoder::CompactLeftover()
{

	if (mFront >= static_cast<size_t>(mSampleRate))
	{
		const int64_t remaining = LeftoverCount();
		for (size_t ch = 0; ch < mLeftover.size(); ch++)
		{
			std::vector<float> &buf = mLeftover[ch];
			if (remaining > 0 && mFront < buf.size())
			{
				size_t keep = static_cast<size_t>(remaining);
				const size_t available = buf.size() - mFront;
				if (keep > available)
				{
					keep = available;
				}
				std::memmove(buf.data(), buf.data() + mFront,
							 keep * sizeof(float));
				buf.resize(keep);
			}
			else
			{
				buf.clear();
			}
		}
		mFront = 0;
	}
}


bool AudioDecoder::EnsureSwr(const AVFrame *f)
{
	const int inFmt  = f->format;
	const int inRate = f->sample_rate;
	const int inCh   = f->ch_layout.nb_channels;

	if (mSwr != nullptr && inFmt == mSwrInFmt && inRate == mSwrInRate && inCh == mSwrInCh)
	{
		return true;
	}

	if (mSwr)
	{
		swr_free(&mSwr);
	}

	AVChannelLayout outLayout;
	av_channel_layout_default(&outLayout, mOutChannels);

	int ret = swr_alloc_set_opts2(
		&mSwr,
		&outLayout, AV_SAMPLE_FMT_FLTP, mSampleRate,
		const_cast<AVChannelLayout*>(&f->ch_layout),
		static_cast<AVSampleFormat>(inFmt), inRate,
		0, nullptr);

	av_channel_layout_uninit(&outLayout);

	if (ret < 0 || mSwr == nullptr)
	{
		return false;
	}
	if (swr_init(mSwr) < 0)
	{
		swr_free(&mSwr);
		return false;
	}

	mSwrInFmt  = inFmt;
	mSwrInRate = inRate;
	mSwrInCh   = inCh;
	return true;
}


bool AudioDecoder::DecodeAppendOneFrame()
{
	for (;;)
	{
		int ret = avcodec_receive_frame(mCodecCtx, mFrame);
		if (ret == 0)
		{

			if (mResync && LeftoverCount() == 0)
			{
				mHeadSample = SampleIndexOf(mFrame);
				mResync = false;
			}

			if (!EnsureSwr(mFrame))
			{
				return false;
			}

			const int inSamples = mFrame->nb_samples;
			const int outMax    = static_cast<int>(swr_get_out_samples(mSwr, inSamples));
			if (outMax <= 0)
			{
				return true;
			}

			const size_t oldSize = mLeftover[0].size();
			const size_t outCount = static_cast<size_t>(outMax);
			if (mScratch.size() != mLeftover.size())
			{
				mScratch.assign(mLeftover.size(), std::vector<float>());
			}
			for (size_t ch = 0; ch < mScratch.size(); ch++)
			{
				mScratch[ch].resize(outCount);
			}

			uint8_t *outPlanes[8] = { nullptr };
			for (int ch = 0; ch < mOutChannels && ch < 8; ch++)
			{
				outPlanes[ch] = reinterpret_cast<uint8_t*>(mScratch[static_cast<size_t>(ch)].data());
			}

			int got = swr_convert(mSwr, outPlanes, outMax,
								  mFrame->extended_data, inSamples);
			if (got < 0)
			{
				return false;
			}


			const size_t gotCount = static_cast<size_t>(got);
			for (size_t ch = 0; ch < mLeftover.size(); ch++)
			{
				mLeftover[ch].reserve(oldSize + gotCount);
			}
			for (size_t ch = 0; ch < mLeftover.size(); ch++)
			{
				mLeftover[ch].resize(oldSize + gotCount);
				if (gotCount > 0)
				{
					std::memcpy(mLeftover[ch].data() + oldSize,
								mScratch[ch].data(),
								gotCount * sizeof(float));
				}
			}
			return true;
		}
		if (ret == AVERROR_EOF)
		{
			mEof = true;
			return false;
		}
		if (ret != AVERROR(EAGAIN))
		{
			return false;
		}


		if (mHasPendingPacket)
		{
			int sret = avcodec_send_packet(mCodecCtx, mPendingPacket);
			if (sret != AVERROR(EAGAIN))
			{
				av_packet_unref(mPendingPacket);
				mHasPendingPacket = false;
			}
			continue;
		}

		if (mFlushSent)
		{
			mEof = true;
			return false;
		}

		int rd = av_read_frame(mFmt, mPacket);
		if (rd == AVERROR_EOF)
		{
			const int sret = avcodec_send_packet(mCodecCtx, nullptr);
			if (sret == 0 || sret == AVERROR_EOF)
			{
				mFlushSent = true;
			}
			else if (sret != AVERROR(EAGAIN))
			{
				return false;
			}
			continue;
		}
		if (rd < 0)
		{
			mLastReadError = rd;
			return false;
		}
		if (mPacket->stream_index == mAudioStream)
		{
			int sret = avcodec_send_packet(mCodecCtx, mPacket);
			if (sret == AVERROR(EAGAIN))
			{
				av_packet_move_ref(mPendingPacket, mPacket);
				mHasPendingPacket = true;
				continue;
			}
		}
		av_packet_unref(mPacket);
	}
}


bool AudioDecoder::SeekToSample(int64_t targetSample)
{
	if (targetSample < 0)
	{
		targetSample = 0;
	}


	AVRational srBase = { 1, mSampleRate };
	AVRational tb     = { mTimeBaseNum, mTimeBaseDen };
	int64_t preroll = mSeekPrerollSamples;
	bool haveSeekStart = false;

	for (int attempt = 0; attempt < 2; attempt++)
	{
		const int64_t seekSample =
			(targetSample > preroll) ? (targetSample - preroll) : 0;
		int64_t ts = av_rescale_q(seekSample, srBase, tb) + mAudioStartTime;

		if (av_seek_frame(mFmt, mAudioStream, ts, AVSEEK_FLAG_BACKWARD) < 0)
		{
					return false;
		}

		avcodec_flush_buffers(mCodecCtx);
		if (mHasPendingPacket)
		{
			av_packet_unref(mPendingPacket);
			mHasPendingPacket = false;
		}
		for (size_t ch = 0; ch < mLeftover.size(); ch++)
		{
			mLeftover[ch].clear();
		}
		mFront		= 0;
		mHeadSample	= -1;
		mResync		= true;
		mEof		= false;
		mLastReadError = 0;
		mFlushSent	= false;


		while (LeftoverCount() == 0 && !mEof)
		{
			if (!DecodeAppendOneFrame())
			{
				break;
			}
		}
		if (mHeadSample < 0)
		{
					return false;
		}
		if (mHeadSample <= targetSample || seekSample == 0)
		{
			haveSeekStart = true;
			break;
		}

		preroll = (preroll > 0) ? (preroll * 2) : (static_cast<int64_t>(mSampleRate) / 5);
		if (preroll > static_cast<int64_t>(mSampleRate))
		{
			preroll = static_cast<int64_t>(mSampleRate);
		}
	}

	if (!haveSeekStart || mHeadSample > targetSample)
	{
			return false;
	}

	int64_t toDrop = targetSample - mHeadSample;
	if (toDrop < 0)
	{
		toDrop = 0;
	}
	while (toDrop > 0)
	{
		if (LeftoverCount() == 0)
		{
			if (!DecodeAppendOneFrame())
			{
				break;
			}
			if (LeftoverCount() == 0)
			{
				continue;
			}
		}
		const int64_t avail = LeftoverCount();
		const int64_t d = (avail < toDrop) ? avail : toDrop;
		if (d <= 0)
		{
			break;
		}
		mFront      += static_cast<size_t>(d);
		mHeadSample += d;
		toDrop      -= d;
	}
	if (toDrop > 0)
	{
		return false;
	}

	return true;
}


int64_t AudioDecoder::ReadSamples(int64_t startSample, int64_t numSamples, float **out, int outChannels)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mLastReadError = 0;

	if (out == nullptr || numSamples <= 0)
	{
		return 0;
	}
	if (startSample < 0)
	{
		startSample = 0;
	}

	int fillCh = (outChannels < mOutChannels) ? outChannels : mOutChannels;


	for (int ch = 0; ch < outChannels; ch++)
	{
		if (out[ch] != nullptr)
		{
			std::memset(out[ch], 0, static_cast<size_t>(numSamples) * sizeof(float));
		}
	}

	if (!mOpened)
	{
		return 0;
	}


	if (mHeadSample != startSample)
	{
		if (!SeekToSample(startSample))
		{
			return 0;
		}
	}

	int64_t delivered = 0;
	while (delivered < numSamples)
	{
		if (LeftoverCount() == 0)
		{
			if (!DecodeAppendOneFrame())
			{
				break;
			}
			continue;
		}

		const int64_t avail = LeftoverCount();
		int64_t n = numSamples - delivered;
		if (n > avail)
		{
			n = avail;
		}

		for (int ch = 0; ch < fillCh; ch++)
		{
			if (out[ch] != nullptr)
			{
				const float *src = mLeftover[ch].data() + mFront;
				std::memcpy(out[ch] + delivered, src, static_cast<size_t>(n) * sizeof(float));
			}
		}

		mFront      += static_cast<size_t>(n);
		mHeadSample += n;
		delivered   += n;

		CompactLeftover();
	}

	return delivered;
}
