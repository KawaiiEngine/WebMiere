
// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.



#ifndef VP9OPUS_VIDEO_DECODER_H
#define VP9OPUS_VIDEO_DECODER_H

#include <cstdint>
#include <string>
#include <mutex>

#include "PrSDKTypes.h"

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct AVCUDADeviceContext;

struct Vp9oDecodedFrameView
{
	const AVFrame	*frame;
	int				width;
	int				height;
	int				avPixelFormat;
	bool			isCuda;
	const uint8_t	*data[4];
	int				linesize[4];
};

class VideoDecoder
{
public:
	VideoDecoder();
	~VideoDecoder();

	VideoDecoder(const VideoDecoder&) = delete;
	VideoDecoder& operator=(const VideoDecoder&) = delete;

	bool Open(const prUTF16Char *path);
	void Close();

	bool IsOpen() const { return mOpened; }
	int  Width()  const { return mWidth; }
	int  Height() const { return mHeight; }
	bool HadReadError() const noexcept { return mLastReadError != 0; }
	int  LastReadError() const noexcept { return mLastReadError; }

	bool DecodeFrameToBGRA(int64_t targetFrame,
						   uint8_t *dstBGRA, int dstRowBytes,
						   int dstWidth, int dstHeight);


	bool DecodeFrameToYUV420P(int64_t targetFrame,
							  uint8_t *dstY, int dstYRowBytes,
							  uint8_t *dstU, int dstURowBytes,
							  uint8_t *dstV, int dstVRowBytes,
							  int dstWidth, int dstHeight);


	void OnHwFormatAccepted();
	void OnHwFormatRejected();

private:
	bool	OpenInternal(bool allowHw);
	bool	ReopenCpu();
	bool	DecodeToTargetAndConvert(int64_t targetFrame, uint8_t *dst, int rowBytes, int w, int h);


	bool	DecodeFrameToSurface(int64_t targetFrame, Vp9oDecodedFrameView *outView);
	bool	ConvertSurfaceToBGRA(const Vp9oDecodedFrameView &view, uint8_t *dst, int dstRowBytes, int dstW, int dstH);


	bool	ConvertSurfaceToYUV420P(const Vp9oDecodedFrameView &view,
									uint8_t *dstY, int dstYRowBytes,
									uint8_t *dstU, int dstURowBytes,
									uint8_t *dstV, int dstVRowBytes,
									int dstW, int dstH);


	bool	SeekBeforeFrame(int64_t targetFrame);
	bool	DecodeNext();
	int64_t	FrameIndexOf(const AVFrame *f) const;
	bool	ConvertToBGRA(const AVFrame *f, uint8_t *dst, int dstRowBytes, int dstW, int dstH);


	bool	ConvertToBGRA_CUDA(const AVFrame *f, uint8_t *dst, int dstRowBytes, int dstW, int dstH);


	bool	ConvertCudaNV12ToYUV420P(const Vp9oDecodedFrameView &view,
									 uint8_t *dstY, int dstYRowBytes,
									 uint8_t *dstU, int dstURowBytes,
									 uint8_t *dstV, int dstVRowBytes,
									 int dstW, int dstH);
	bool	FreeCudaYuvBuffersStrict();

	bool	EnsureCudaConsumerStream(void **outStream) noexcept;
	bool	EnsureNppStreamContext(void *stream) noexcept;
	bool	PrepareCudaSurfaceRead(AVCUDADeviceContext *cudaDev, void **outStream, bool *outQueuedWait) noexcept;
	bool	DestroyCudaEventStrict() noexcept;
	bool	ReleaseCudaBuffers();
	bool	ReleaseCudaBuffersInCtx(void *ctx);
	bool	FreeCudaBuffersStrict();
	bool	DestroyStreamStrict();
	bool	BuildNppStreamContext(void *stream) noexcept;
	bool	EnsurePinned(size_t bytes);
	void	MarkCudaCleanupFailed() noexcept;


	AVFormatContext	*mFmt;
	AVCodecContext	*mCodecCtx;
	AVFrame			*mFrame;
	AVFrame			*mReceiveFrame;
	AVFrame			*mSwFrame;
	AVPacket		*mPacket;
	AVPacket		*mPendingPacket;
	bool			mHasPendingPacket;
	SwsContext		*mSws;


	int		mVideoStream;
	int		mWidth;
	int		mHeight;
	int		mFrameRateNum;
	int		mFrameRateDen;
	int		mTimeBaseNum;
	int		mTimeBaseDen;
	int64_t	mVideoStartTime;


	int64_t	mCurrentFrame;
	bool	mHasFrame;
	bool	mEofDrained;
	int		mLastReadError;
	bool	mFlushSent;


	int		mShortSeekThreshold;


	bool		mUseHw;
	bool		mHwActive;
	bool		mDisableHw;
	const char	*mFallbackReason;


	int		mSwsSrcFmt;
	int		mSwsSrcW;
	int		mSwsSrcH;
	int		mSwsDstW;
	int		mSwsDstH;


	int		mSwsFlags;


	SwsContext				*mSwsYuv;
	int						mSwsYuvSrcFmt;
	int						mSwsYuvSrcW;
	int						mSwsYuvSrcH;
	int						mSwsYuvDstW;
	int						mSwsYuvDstH;


	uint8_t			*mCudaBgr;
	uint8_t			*mCudaBgra;
	uint8_t			*mCudaFlip;
	size_t			mCudaBgrPitch;
	size_t			mCudaBgraPitch;
	size_t			mCudaFlipPitch;
	int				mCudaBufW;
	int				mCudaBufH;


	uint8_t			*mCudaYuvY;
	uint8_t			*mCudaYuvU;
	uint8_t			*mCudaYuvV;
	size_t			mCudaYuvYPitch;
	size_t			mCudaYuvUPitch;
	size_t			mCudaYuvVPitch;
	int				mCudaYuvBufW;
	int				mCudaYuvBufH;
	void			*mCudaCtx;
	void			*mCudaStream;
	bool			mCudaCleanupFailed;

	bool			mNppDevReady;
	int				mNppDeviceId;
	int				mNppMpCount;
	int				mNppMaxThreadsPerMp;
	int				mNppMaxThreadsPerBlock;
	size_t			mNppSharedMemPerBlock;
	int				mNppCcMajor;
	int				mNppCcMinor;
	unsigned int	mNppStreamFlags;
	void			*mNppStreamCtx;
	void			*mNppStream;
	int				mCudaSyncMode;
	void			*mCudaProducerEvent;
	void			*mCudaProducerEventCtx;

	uint8_t			*mPinnedStaging;
	size_t			mPinnedCapacity;

	std::string	mPathUtf8;
	bool		mOpened;
	std::mutex	mMutex;
};

#endif
