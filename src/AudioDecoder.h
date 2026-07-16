
// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.



#ifndef VP9OPUS_AUDIO_DECODER_H
#define VP9OPUS_AUDIO_DECODER_H

#include <cstdint>
#include <vector>
#include <mutex>

#include "PrSDKTypes.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

class AudioDecoder
{
public:
	AudioDecoder();
	~AudioDecoder();

	AudioDecoder(const AudioDecoder&) = delete;
	AudioDecoder& operator=(const AudioDecoder&) = delete;


	bool Open(const prUTF16Char *path, int ffmpegAudioStreamIndex);
	void Close();

	bool IsOpen()     const { return mOpened; }
	int  Channels()   const { return mOutChannels; }
	int  SampleRate() const { return mSampleRate; }
	bool HadReadError() const noexcept { return mLastReadError != 0; }
	int  LastReadError() const noexcept { return mLastReadError; }


	int64_t ReadSamples(int64_t startSample, int64_t numSamples, float **out, int outChannels);

private:
	bool	SeekToSample(int64_t targetSample);
	bool	TrySeekViaVideoCue(int64_t targetSample);
	bool	SeekViaAudioIndex(int64_t targetSample);
	bool	DecodeAppendOneFrame();
	bool	EnsureSwr(const AVFrame *f);
	int64_t	SampleIndexOf(const AVFrame *f) const;
	int64_t	LeftoverCount() const;
	void	CompactLeftover();


	AVFormatContext	*mFmt;
	AVCodecContext	*mCodecCtx;
	AVFrame			*mFrame;
	AVPacket		*mPacket;
	AVPacket		*mPendingPacket;
	bool			mHasPendingPacket;
	SwrContext		*mSwr;


	int	mAudioStream;
	int	mVideoStream;
	int	mSampleRate;
	int	mOutChannels;
	int	mTimeBaseNum;
	int	mTimeBaseDen;
	int64_t	mAudioStartTime;
	int64_t	mInitialPaddingSamples;
	int64_t	mSeekPrerollSamples;


	int	mSwrInFmt;
	int	mSwrInRate;
	int	mSwrInCh;


	std::vector<std::vector<float>>	mLeftover;
	std::vector<std::vector<float>>	mScratch;
	size_t	mFront;
	int64_t	mHeadSample;
	bool	mResync;
	bool	mEof;
	int		mLastReadError;
	bool	mFlushSent;

	bool		mOpened;
	std::mutex	mMutex;
};

#endif
