
// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.



#define __STDC_CONSTANT_MACROS

#include "VideoDecoder.h"
#include "AlphaResize.h"
#include "WebMiereLimits.h"
#include "WebMiereColorPolicy.h"

#include <windows.h>
#include <string>
#include <memory>
#include <cstring>
#include <limits>
#include <new>


#ifndef WEBMIERE_ENABLE_NPP
#define WEBMIERE_ENABLE_NPP 1
#endif


#ifndef WEBMIERE_ENABLE_CUDA_YUV420P_DIRECT
#define WEBMIERE_ENABLE_CUDA_YUV420P_DIRECT 1
#endif

#if WEBMIERE_ENABLE_NPP
#include <cuda.h>
#include "HalfAlphaPtx.h"
#include <cuda_runtime.h>
#include <npp.h>
#endif


extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/buffer.h>
#include <libavutil/mem.h>
#include <libavutil/mathematics.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#if WEBMIERE_ENABLE_NPP
#include <libavutil/hwcontext_cuda.h>
#endif
#include <libswscale/swscale.h>
}


namespace {
struct AVBufferRefDeleter
{
	void operator()(AVBufferRef *p) const noexcept
	{
		if (p)
		{
			av_buffer_unref(&p);
		}
	}
};
using AVBufferRefGuard = std::unique_ptr<AVBufferRef, AVBufferRefDeleter>;

#if WEBMIERE_ENABLE_NPP
class CuCtxPushGuard
{
public:
	explicit CuCtxPushGuard(CUcontext ctx) noexcept
		: mPushed(cuCtxPushCurrent(ctx) == CUDA_SUCCESS)
	{
	}

	~CuCtxPushGuard() noexcept
	{
		pop();
	}

	bool ok() const noexcept
	{
		return mPushed;
	}

	bool pop() noexcept
	{
		if (!mPushed)
		{
			return true;
		}
		CUcontext popped = nullptr;
		const CUresult res = cuCtxPopCurrent(&popped);
		mPushed = false;
		return res == CUDA_SUCCESS;
	}

private:
	bool mPushed;
};

enum WebMiereCudaSyncMode
{
	kWebMiereCudaSyncContext = 0,
	kWebMiereCudaSyncProducer = 1,
	kWebMiereCudaSyncSameStream = 2,
	kWebMiereCudaSyncEvent = 3
};

static WebMiereCudaSyncMode ReadCudaSyncModeFromEnv() noexcept
{
	wchar_t value[32] = {};
	const DWORD len = GetEnvironmentVariableW(
		L"WEBMIERE_CUDA_SYNC_MODE",
		value,
		static_cast<DWORD>(sizeof(value) / sizeof(value[0])));
	if (len == 0)
	{
		return kWebMiereCudaSyncEvent;
	}
	if (lstrcmpiW(value, L"context") == 0 ||
		lstrcmpiW(value, L"ctx") == 0)
	{
		return kWebMiereCudaSyncContext;
	}
	if (lstrcmpiW(value, L"producer") == 0 ||
		lstrcmpiW(value, L"producer_stream") == 0)
	{
		return kWebMiereCudaSyncProducer;
	}
	if (lstrcmpiW(value, L"same") == 0 ||
		lstrcmpiW(value, L"same_stream") == 0 ||
		lstrcmpiW(value, L"samestream") == 0)
	{
		return kWebMiereCudaSyncSameStream;
	}
	if (lstrcmpiW(value, L"event") == 0 ||
		lstrcmpiW(value, L"cuda_event") == 0)
	{
		return kWebMiereCudaSyncEvent;
	}
	return kWebMiereCudaSyncEvent;
}

#endif
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


extern "C" {
static enum AVPixelFormat WebMiereGetFormat(AVCodecContext *ctx, const enum AVPixelFormat *fmts)
{
	VideoDecoder *self = static_cast<VideoDecoder*>(ctx->opaque);

	for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p)
	{
		if (*p == AV_PIX_FMT_CUDA)
		{
			if (self)
			{
				self->OnHwFormatAccepted();
			}
			return AV_PIX_FMT_CUDA;
		}
	}

	if (self)
	{
		self->OnHwFormatRejected();
	}
	return fmts[0];
}
}


static bool IsHwPixFmt(int fmt)
{
	const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(fmt));
	return (d != nullptr) && ((d->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0);
}


static void CopyPlaneRespectingRowBytes(uint8_t *dst, int dstRowBytes,
										const uint8_t *src, int srcRowBytes,
										int widthBytes, int height)
{
	for (int y = 0; y < height; y++)
	{
		uint8_t       *d = dst + static_cast<ptrdiff_t>(y) * dstRowBytes;
		const uint8_t *s = src + static_cast<ptrdiff_t>(y) * srcRowBytes;
		std::memcpy(d, s, static_cast<size_t>(widthBytes));
	}
}


static bool RowBytesCanHold(int rowBytes, int widthBytes)
{
	return widthBytes > 0 && (rowBytes >= widthBytes || rowBytes <= -widthBytes);
}

static bool ValidateFramePlanesForDescriptor(const AVFrame *f, const AVPixFmtDescriptor *desc)
{
	if (f == nullptr || desc == nullptr)
	{
		return false;
	}

	bool usedPlanes[AV_NUM_DATA_POINTERS] = {};
	for (int i = 0; i < desc->nb_components; i++)
	{
		const int plane = desc->comp[i].plane;
		if (plane < 0 || plane >= AV_NUM_DATA_POINTERS)
		{
			return false;
		}
		usedPlanes[plane] = true;
	}

	for (int plane = 0; plane < AV_NUM_DATA_POINTERS; plane++)
	{
		if (usedPlanes[plane] && (f->data[plane] == nullptr || f->linesize[plane] == 0))
		{
			return false;
		}
	}
	return true;
}

static bool ValidateDecodedFrameForView(const AVFrame *f)
{
	if (f == nullptr ||
		!WebMiereIsValidVideoSize(f->width, f->height) ||
		f->format < 0)
	{
		return false;
	}

	if (f->format == AV_PIX_FMT_CUDA)
	{
#if WEBMIERE_ENABLE_NPP
		return f->hw_frames_ctx != nullptr && f->data[0] != nullptr;
#else
		return false;
#endif
	}

	const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
	if (desc == nullptr)
	{
		return false;
	}
	if ((desc->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0)
	{
		return f->data[0] != nullptr;
	}
	return ValidateFramePlanesForDescriptor(f, desc);
}

static bool ValidateYuv420PFrame(const AVFrame *f)
{
	return f != nullptr &&
		   f->format == AV_PIX_FMT_YUV420P &&
		   WebMiereIsValidYuv420Size(f->width, f->height) &&
		   f->data[0] != nullptr &&
		   f->data[1] != nullptr &&
		   f->data[2] != nullptr &&
		   RowBytesCanHold(f->linesize[0], f->width) &&
		   RowBytesCanHold(f->linesize[1], f->width / 2) &&
		   RowBytesCanHold(f->linesize[2], f->width / 2);
}

static bool ValidateNV12Frame(const AVFrame *f)
{
	return f != nullptr &&
		   f->format == AV_PIX_FMT_NV12 &&
		   WebMiereIsValidYuv420Size(f->width, f->height) &&
		   f->data[0] != nullptr &&
		   f->data[1] != nullptr &&
		   RowBytesCanHold(f->linesize[0], f->width) &&
		   RowBytesCanHold(f->linesize[1], f->width);
}

#if WEBMIERE_ENABLE_NPP
static bool ValidateCudaNV12Frame(const AVFrame *f,
								  AVHWFramesContext **outFramesCtx,
								  AVCUDADeviceContext **outCudaDev)
{
	if (outFramesCtx != nullptr)
	{
		*outFramesCtx = nullptr;
	}
	if (outCudaDev != nullptr)
	{
		*outCudaDev = nullptr;
	}

	if (f == nullptr ||
		f->format != AV_PIX_FMT_CUDA ||
		!WebMiereIsValidYuv420Size(f->width, f->height) ||
		f->data[0] == nullptr ||
		f->data[1] == nullptr ||
		f->hw_frames_ctx == nullptr ||
		f->linesize[0] <= 0 ||
		f->linesize[1] <= 0 ||
		f->linesize[0] != f->linesize[1])
	{
		return false;
	}

	AVHWFramesContext *fctx = reinterpret_cast<AVHWFramesContext*>(f->hw_frames_ctx->data);
	if (fctx == nullptr ||
		fctx->sw_format != AV_PIX_FMT_NV12 ||
		fctx->device_ctx == nullptr ||
		fctx->device_ctx->hwctx == nullptr)
	{
		return false;
	}

	AVCUDADeviceContext *cudev = static_cast<AVCUDADeviceContext*>(fctx->device_ctx->hwctx);
	if (cudev == nullptr || cudev->cuda_ctx == nullptr)
	{
		return false;
	}

	if (outFramesCtx != nullptr)
	{
		*outFramesCtx = fctx;
	}
	if (outCudaDev != nullptr)
	{
		*outCudaDev = cudev;
	}
	return true;
}
#endif


VideoDecoder::VideoDecoder()
	: mFmt(nullptr)
	, mCodecCtx(nullptr)
	, mFrame(nullptr)
	, mReceiveFrame(nullptr)
	, mSwFrame(nullptr)
	, mPacket(nullptr)
	, mPendingPacket(nullptr)
	, mHasPendingPacket(false)
	, mSws(nullptr)
	, mVideoStream(-1)
	, mWidth(0)
	, mHeight(0)
	, mFrameRateNum(60)
	, mFrameRateDen(1)
	, mTimeBaseNum(1)
	, mTimeBaseDen(1000)
	, mVideoStartTime(0)
	, mCurrentFrame(-1)
	, mHasFrame(false)
	, mEofDrained(false)
	, mLastReadError(0)
	, mFlushSent(false)
	, mShortSeekThreshold(120)
	, mUseHw(false)
	, mHwActive(false)
	, mDisableHw(false)
	, mFallbackReason(nullptr)
	, mSwsSrcFmt(-1)
	, mSwsSrcW(0)
	, mSwsSrcH(0)
	, mSwsDstW(0)
	, mSwsDstH(0)
	, mSwsFlags(SWS_BILINEAR)
	, mSwsYuv(nullptr)
	, mSwsYuvSrcFmt(-1)
	, mSwsYuvSrcW(0)
	, mSwsYuvSrcH(0)
	, mSwsYuvDstW(0)
	, mSwsYuvDstH(0)
	, mCudaBgr(nullptr)
	, mCudaBgra(nullptr)
	, mCudaFlip(nullptr)
	, mCudaBgrPitch(0)
	, mCudaBgraPitch(0)
	, mCudaFlipPitch(0)
	, mCudaBufW(0)
	, mCudaBufH(0)
	, mCudaYuvY(nullptr)
	, mCudaYuvU(nullptr)
	, mCudaYuvV(nullptr)
	, mCudaYuvYPitch(0)
	, mCudaYuvUPitch(0)
	, mCudaYuvVPitch(0)
	, mCudaYuvBufW(0)
	, mCudaYuvBufH(0)
	, mCudaCtx(nullptr)
	, mCudaStream(nullptr)
	, mCudaCleanupFailed(false)
	, mNppDevReady(false)
	, mNppDeviceId(0)
	, mNppMpCount(0)
	, mNppMaxThreadsPerMp(0)
	, mNppMaxThreadsPerBlock(0)
	, mNppSharedMemPerBlock(0)
	, mNppCcMajor(0)
	, mNppCcMinor(0)
	, mNppStreamFlags(0)
	, mNppStreamCtx(nullptr)
	, mNppStream(nullptr)
	, mCudaSyncMode(
#if WEBMIERE_ENABLE_NPP
		static_cast<int>(ReadCudaSyncModeFromEnv())
#else
		0
#endif
	  )
	, mCudaProducerEvent(nullptr)
	, mCudaProducerEventCtx(nullptr)
	, mPinnedStaging(nullptr)
	, mPinnedCapacity(0)
	, mOpened(false)
{
}

VideoDecoder::~VideoDecoder()
{
	Close();
}


void VideoDecoder::Close()
{
	ReleaseCudaBuffers();
	mAlphaResizeUnavailableCtx = nullptr;

#if WEBMIERE_ENABLE_NPP
	if (mNppStreamCtx != nullptr)
	{
		delete static_cast<NppStreamContext*>(mNppStreamCtx);
		mNppStreamCtx = nullptr;
	}
	mNppStream = nullptr;
#endif

	av_packet_free(&mAlphaPacket);
	av_frame_free(&mAlphaFrame);
	av_frame_free(&mAlphaReceive);
	av_frame_free(&mAlphaSwFrame);
	avcodec_free_context(&mAlphaCodec);
	av_frame_free(&mColorScratch);
	mHalfAlpha.reset();
	mColorDownload = {};
	mMaskDownload = {};
	if (mSws)
	{
		sws_freeContext(mSws);
		mSws = nullptr;
	}
	if (mSwsYuv)
	{
		sws_freeContext(mSwsYuv);
		mSwsYuv = nullptr;
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
	if (mSwFrame)
	{
		av_frame_free(&mSwFrame);
	}
	if (mReceiveFrame)
	{
		av_frame_free(&mReceiveFrame);
	}
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

	mVideoStream			= -1;
	mCurrentFrame			= -1;
	mHasFrame				= false;
	mEofDrained				= false;
	mLastReadError			= 0;
	mFlushSent				= false;
	mUseHw					= false;
	mHwActive				= false;
	mFallbackReason			= nullptr;
	mSwsSrcFmt				= -1;
	mSwsSrcW = mSwsSrcH = mSwsDstW = mSwsDstH = 0;
	mSwsYuvSrcFmt			= -1;
	mSwsYuvSrcW = mSwsYuvSrcH = mSwsYuvDstW = mSwsYuvDstH = 0;
	mOpened					= false;
}


bool VideoDecoder::Open(const prUTF16Char *path)
{
	mPathUtf8 = Utf16ToUtf8(path);
#if WEBMIERE_ENABLE_NPP
	mCudaSyncMode = static_cast<int>(ReadCudaSyncModeFromEnv());
#endif
	wchar_t forceCpu[8] = {};
	if (GetEnvironmentVariableW(L"WEBMIERE_FORCE_CPU_DECODE", forceCpu, static_cast<DWORD>(sizeof(forceCpu) / sizeof(forceCpu[0]))) > 0 &&
		forceCpu[0] == L'1')
	{
		mDisableHw = true;
	}
	return OpenInternal(!mDisableHw);
}


bool VideoDecoder::ReopenCpu()
{
	mDisableHw = true;
		return OpenInternal(false);
}


bool VideoDecoder::OpenInternal(bool allowHw)
{
	Close();

	if (mPathUtf8.empty())
	{
		return false;
	}

	if (avformat_open_input(&mFmt, mPathUtf8.c_str(), nullptr, nullptr) < 0)
	{
		Close();
		return false;
	}
	if (avformat_find_stream_info(mFmt, nullptr) < 0)
	{
		Close();
		return false;
	}

	const AVCodec *defaultDec = nullptr;
	mVideoStream = av_find_best_stream(mFmt, AVMEDIA_TYPE_VIDEO, -1, -1, &defaultDec, 0);
	if (mVideoStream < 0 || defaultDec == nullptr)
	{
		Close();
		return false;
	}

	AVStream *st = mFmt->streams[mVideoStream];
	const bool isVp9 = (st->codecpar->codec_id == AV_CODEC_ID_VP9);
	const bool isAv1 = (st->codecpar->codec_id == AV_CODEC_ID_AV1);
	const bool isSupportedVideo = isVp9 || isAv1;
	const bool hasAlpha = WebMiereHasAlphaMode(st);
	if (!isSupportedVideo || (hasAlpha && !isVp9))
	{
		Close();
		return false;
	}
	{
		const AVCodecParameters *par = st->codecpar;
		if (!WebMiereIsValidVideoSize(par->width, par->height))
		{
			Close();
			return false;
		}
		const AVPixFmtDescriptor *pd = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format));
		if (isSupportedVideo &&
			(par->format == AV_PIX_FMT_NONE || pd == nullptr ||
			 (pd->flags & AV_PIX_FMT_FLAG_HWACCEL) != 0 ||
			 (pd->flags & AV_PIX_FMT_FLAG_RGB) != 0 ||
			 ((pd->flags & AV_PIX_FMT_FLAG_ALPHA) != 0 && !(hasAlpha && par->format == AV_PIX_FMT_YUVA420P)) ||
			 pd->comp[0].depth != 8 ||
			 pd->log2_chroma_w != 1 || pd->log2_chroma_h != 1))
		{
			Close();
			return false;
		}
		if (isSupportedVideo && !WebMiereIsStrictBt709(par))
		{
			Close();
			return false;
		}
	}

	mWidth  = st->codecpar->width;
	mHeight = st->codecpar->height;

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
	mFrameRateNum = fr.num;
	mFrameRateDen = fr.den;
	mTimeBaseNum  = st->time_base.num;
	mTimeBaseDen  = st->time_base.den;
	const int64_t rawStartTime = (st->start_time == AV_NOPTS_VALUE) ? 0 : st->start_time;
	mVideoStartTime = rawStartTime;


	AVBufferRefGuard hwGuard;
	bool wantHw = allowHw && !mDisableHw && !mCudaCleanupFailed;
	if (wantHw)
	{
		AVBufferRef *hwRaw = nullptr;
		int hr = av_hwdevice_ctx_create(&hwRaw, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
		if (hr < 0)
		{
			wantHw = false;
		}
		else
		{
			hwGuard.reset(hwRaw);
					}
	}


	auto openCodec = [&](AVCodecContext *&ctx, bool useHw) -> bool
	{
		const AVCodec *attemptDec = hasAlpha ? avcodec_find_decoder_by_name("vp9") : defaultDec;
		if (!attemptDec) return false;
		if (isAv1)
		{
			attemptDec = avcodec_find_decoder_by_name(useHw ? "av1" : "libdav1d");
			if (attemptDec == nullptr)
			{
				return false;
			}
		}
		if (ctx)
		{
			avcodec_free_context(&ctx);
		}
		ctx = avcodec_alloc_context3(attemptDec);
		if (ctx == nullptr)
		{
			return false;
		}
		if (avcodec_parameters_to_context(ctx, st->codecpar) < 0)
		{
			return false;
		}
		// Each VP9 block supplies the color and mask for the same presentation time.
		// Disable frame-thread delay so both decoders expose that pair together.
		ctx->thread_count = hasAlpha ? 1 : 0;
		ctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;

		if (useHw)
		{
			AVBufferRef *hwRef = av_buffer_ref(hwGuard.get());
			if (hwRef == nullptr)
			{
				mUseHw = false;
				return false;
			}
			ctx->hw_device_ctx = hwRef;
			ctx->opaque        = this;
			ctx->get_format    = WebMiereGetFormat;
			mUseHw = true;
		}
		else
		{
			mUseHw = false;
		}

		return avcodec_open2(ctx, attemptDec, nullptr) == 0;
	};

	auto tryOpenCodec = [&](bool useHw) -> bool
	{
		return openCodec(mCodecCtx, useHw) && (!hasAlpha || openCodec(mAlphaCodec, useHw));
	};

	bool opened = false;
	if (wantHw && hwGuard)
	{
		opened = tryOpenCodec(true);
	}
	if (!opened)
	{
		opened = tryOpenCodec(false);
	}
	if (!opened)
	{
		Close();
		return false;
	}


	if (hasAlpha)
	{
		mAlphaFrame = av_frame_alloc();
		mAlphaReceive = av_frame_alloc();
		mAlphaSwFrame = av_frame_alloc();
		mAlphaPacket = av_packet_alloc();
		if (!mAlphaFrame || !mAlphaReceive || !mAlphaSwFrame || !mAlphaPacket)
		{
			Close();
			return false;
		}
	}
	mFrame         = av_frame_alloc();
	mReceiveFrame  = av_frame_alloc();
	mSwFrame       = av_frame_alloc();
	mPacket        = av_packet_alloc();
	mPendingPacket = av_packet_alloc();
	mHasPendingPacket = false;
	if (mFrame == nullptr || mReceiveFrame == nullptr || mSwFrame == nullptr ||
		mPacket == nullptr || mPendingPacket == nullptr)
	{
		Close();
		return false;
	}

	mCurrentFrame			= -1;
	mHasFrame				= false;
	mEofDrained				= false;
	mLastReadError			= 0;
	mFlushSent				= false;
	mOpened					= true;

	return true;
}


void VideoDecoder::OnHwFormatAccepted()
{
	mHwActive = true;
}

void VideoDecoder::OnHwFormatRejected()
{
	mHwActive = false;
}


int64_t VideoDecoder::FrameIndexOf(const AVFrame *f) const
{
	int64_t pts = f->best_effort_timestamp;
	if (pts == AV_NOPTS_VALUE)
	{
		pts = f->pts;
	}
	if (pts == AV_NOPTS_VALUE)
	{
		return mCurrentFrame + 1;
	}
	pts -= mVideoStartTime;
	if (pts < 0)
	{
		pts = 0;
	}
	const AVRational timeBase = { mTimeBaseNum, mTimeBaseDen };
	const AVRational frameBase = { mFrameRateDen, mFrameRateNum };

	return av_rescale_q_rnd(
		pts,
		timeBase,
		frameBase,
		AV_ROUND_NEAR_INF);
}


bool VideoDecoder::SeekBeforeFrame(int64_t targetFrame)
{
	if (targetFrame < 0)
	{
		targetFrame = 0;
	}

	AVRational tb     = { mTimeBaseNum, mTimeBaseDen };
	AVRational invFps = { mFrameRateDen, mFrameRateNum };

	int64_t		backoff		= 0;
	const int	kMaxAttempts = 6;

	for (int attempt = 0; attempt < kMaxAttempts; attempt++)
	{
		int64_t seekFrame = targetFrame - backoff;
		if (seekFrame < 0)
		{
			seekFrame = 0;
		}

		int64_t ts = av_rescale_q(seekFrame, invFps, tb) + mVideoStartTime;

		int ret = av_seek_frame(mFmt, mVideoStream, ts, AVSEEK_FLAG_BACKWARD);
		if (ret < 0)
		{
						return false;
		}

		avcodec_flush_buffers(mCodecCtx);
		if (mAlphaCodec)
		{
			avcodec_flush_buffers(mAlphaCodec);
			av_frame_unref(mAlphaFrame);
			av_frame_unref(mAlphaReceive);
			av_packet_unref(mAlphaPacket);
		}
		if (mHasPendingPacket)
		{
			av_packet_unref(mPendingPacket);
			mHasPendingPacket = false;
		}
		mHasFrame		= false;
		mCurrentFrame	= -1;
		mEofDrained		= false;
		mLastReadError = 0;
		mAlphaDecodeError = false;
		mFlushSent		= false;

		if (!DecodeNext())
		{
			if (seekFrame == 0)
			{
								return false;
			}
		}
		else
		{
			if (mCurrentFrame <= targetFrame)
			{
								return true;
			}
					}

		if (seekFrame == 0)
		{
			return mHasFrame;
		}
		backoff = (backoff == 0) ? 30 : (backoff * 2);
	}

		return mHasFrame;
}



bool VideoDecoder::DecodeNextWithAlpha()
{
	const int result = ReadAlphaFramePair();
	if (result == AVERROR_EOF)
	{
		mEofDrained = true;
	}
	else if (result < 0)
	{
		// The two decoders may have consumed different inputs. Seek both on retry.
		mAlphaDecodeError = !HadReadError();
		mHasFrame = false;
	}
	return result == 0;
}

int VideoDecoder::ReadAlphaFramePair()
{
	for (;;)
	{
		const int color = avcodec_receive_frame(mCodecCtx, mReceiveFrame);
		const int alpha = avcodec_receive_frame(mAlphaCodec, mAlphaReceive);
		if (color != alpha) return AVERROR_INVALIDDATA;
		if (color == 0)
		{
			if (mReceiveFrame->pts == AV_NOPTS_VALUE || mReceiveFrame->pts != mAlphaReceive->pts ||
				mReceiveFrame->width != mAlphaReceive->width || mReceiveFrame->height != mAlphaReceive->height)
			{
				return AVERROR_INVALIDDATA;
			}
			mCurrentFrame = FrameIndexOf(mReceiveFrame);
			av_frame_unref(mFrame);
			av_frame_move_ref(mFrame, mReceiveFrame);
			av_frame_unref(mAlphaFrame);
			av_frame_move_ref(mAlphaFrame, mAlphaReceive);
			mHasFrame = true;
			return 0;
		}
		if (color != AVERROR(EAGAIN)) return color;
		if (mFlushSent) return AVERROR_INVALIDDATA;

		int read;
		do
		{
			av_packet_unref(mPacket);
			read = av_read_frame(mFmt, mPacket);
		} while (read >= 0 && mPacket->stream_index != mVideoStream);
		if (read == AVERROR_EOF && mFmt->pb && mFmt->pb->error < 0 && mFmt->pb->error != AVERROR_EOF)
			read = mFmt->pb->error;
		if (read == AVERROR_EOF)
		{
			int ret = avcodec_send_packet(mCodecCtx, nullptr);
			if (ret < 0) return ret;
			ret = avcodec_send_packet(mAlphaCodec, nullptr);
			if (ret < 0) return ret;
			mFlushSent = true;
			continue;
		}
		if (read < 0)
		{
			mLastReadError = read;
			return read;
		}

		// The demuxer prefixes Matroska BlockAdditional with its 64-bit big-endian ID.
		const uint8_t alphaId[8] = {0, 0, 0, 0, 0, 0, 0, 1};
		av_packet_unref(mAlphaPacket);
		for (int i = 0; i < mPacket->side_data_elems; ++i)
		{
			const AVPacketSideData &side = mPacket->side_data[i];
			if (side.type != AV_PKT_DATA_MATROSKA_BLOCKADDITIONAL || side.size < sizeof(alphaId) ||
				std::memcmp(side.data, alphaId, sizeof(alphaId)) != 0) continue;

			const size_t size = side.size - sizeof(alphaId);
			if (size == 0 || size > std::numeric_limits<int>::max()) return AVERROR_INVALIDDATA;
			int ret = av_new_packet(mAlphaPacket, static_cast<int>(size));
			if (ret < 0) return ret;
			std::memcpy(mAlphaPacket->data, side.data + sizeof(alphaId), size);
			mAlphaPacket->pts = mPacket->pts;
			mAlphaPacket->dts = mPacket->dts;
			mAlphaPacket->duration = mPacket->duration;
			ret = avcodec_send_packet(mAlphaCodec, mAlphaPacket);
			if (ret < 0) return ret;
			break;
		}
		const int ret = avcodec_send_packet(mCodecCtx, mPacket);
		if (ret < 0) return ret;
	}
}

bool VideoDecoder::DecodeNext()
{
	if (mAlphaCodec) return DecodeNextWithAlpha();
	for (;;)
	{
		int ret = avcodec_receive_frame(mCodecCtx, mReceiveFrame);
		if (ret == 0)
		{
			const int64_t decodedFrame = FrameIndexOf(mReceiveFrame);

			av_frame_unref(mFrame);
			av_frame_move_ref(mFrame, mReceiveFrame);

			mCurrentFrame = decodedFrame;
			mHasFrame = true;
			return true;
		}
		if (ret == AVERROR_EOF)
		{
			mEofDrained = true;
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
			mEofDrained = true;
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
		if (mPacket->stream_index == mVideoStream)
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


bool VideoDecoder::DecodeFrameToBGRA(int64_t targetFrame,
									 uint8_t *dstBGRA, int dstRowBytes,
									 int dstWidth, int dstHeight)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mLastReadError = 0;
	mAlphaDecodeError = false;

	if (!mOpened || dstBGRA == nullptr)
	{
		return false;
	}
	if (targetFrame < 0)
	{
		targetFrame = 0;
	}

	if (DecodeToTargetAndConvert(targetFrame, dstBGRA, dstRowBytes, dstWidth, dstHeight))
	{
		return true;
	}


	if (mUseHw && mFallbackReason != nullptr)
	{
				if (ReopenCpu())
		{
			return DecodeToTargetAndConvert(targetFrame, dstBGRA, dstRowBytes, dstWidth, dstHeight);
		}
			}
	return false;
}


bool VideoDecoder::DecodeToTargetAndConvert(int64_t targetFrame, uint8_t *dst, int rowBytes, int w, int h)
{
	WebMiereDecodedFrameView view;
	if (!DecodeFrameToSurface(targetFrame, &view))
	{
		return false;
	}
	return ConvertSurfaceToBGRA(view, dst, rowBytes, w, h);
}


bool VideoDecoder::DecodeFrameToSurface(int64_t targetFrame, WebMiereDecodedFrameView *outView)
{
	mFallbackReason = nullptr;

	if (outView == nullptr)
	{
		return false;
	}

	if (!(mHasFrame && mCurrentFrame == targetFrame))
	{


		const bool needSeek =
			(!mHasFrame) ||
			(targetFrame < mCurrentFrame) ||
			(targetFrame - mCurrentFrame > mShortSeekThreshold);

		if (needSeek)
		{
						if (!SeekBeforeFrame(targetFrame))
			{
								return false;
			}
		}

		while (!mHasFrame || mCurrentFrame < targetFrame)
		{
			if (!DecodeNext())
			{
								break;
			}
		}


		if (!mHasFrame)
		{
			return false;
		}

		if (mCurrentFrame < targetFrame)
		{
			const bool holdLastFrame =
				mEofDrained &&
				targetFrame == mCurrentFrame + 1;

			if (!holdLastFrame)
			{
				return false;
			}
		}
	}


	if (!ValidateDecodedFrameForView(mFrame))
	{
				return false;
	}


	outView->frame         = mFrame;
	outView->width         = mFrame->width;
	outView->height        = mFrame->height;
	outView->avPixelFormat = mFrame->format;
	outView->isCuda        = (mFrame->format == AV_PIX_FMT_CUDA);
	for (int i = 0; i < 4; i++)
	{
		outView->data[i]     = mFrame->data[i];
		outView->linesize[i] = mFrame->linesize[i];
	}
	return true;
}


bool VideoDecoder::ConvertSurfaceToBGRA(const WebMiereDecodedFrameView &view, uint8_t *dst, int dstRowBytes, int dstW, int dstH)
{

	const AVFrame *src = view.frame;
	if (src == nullptr)
	{
		return false;
	}

	bool useHalfBands = false;
	if (view.isCuda)
	{
		if (mCudaCleanupFailed || mDisableHw)
		{
			mFallbackReason = "cuda-cleanup";
			return false;
		}


		if (src->data[0] == nullptr || src->hw_frames_ctx == nullptr)
		{
						return false;
		}


		if (ConvertToBGRA_CUDA(src, dst, dstRowBytes, dstW, dstH))
		{
			return true;
		}

#if WEBMIERE_ENABLE_NPP
		if (mAlphaCodec && !ReleaseAlphaGpuOutput())
		{
			MarkCudaCleanupFailed();
			return false;
		}
		// The existing half-size software converter can work in bands.
		useHalfBands = mAlphaCodec && WebMiereIsValidVideoSize(dstW, dstH) &&
			src->width == 2 * dstW && src->height == 2 * dstH &&
			static_cast<int64_t>(src->width) * src->height >= 1024 * 1024 && dstH >= 16 &&
			dst && RowBytesCanHold(dstRowBytes, dstW * 4) &&
			ValidateCudaNV12Frame(src, nullptr, nullptr) &&
			ValidateCudaNV12Frame(mAlphaFrame, nullptr, nullptr) &&
			mAlphaFrame->width == src->width && mAlphaFrame->height == src->height;
#endif

		int tr;
		if (mAlphaCodec) {
			tr = mColorDownload.Transfer(mSwFrame, src);
		} else {
			av_frame_unref(mSwFrame);
			tr = av_hwframe_transfer_data(mSwFrame, src, 0);
		}
		if (tr < 0)
		{
						mFallbackReason = "hwdownload";
			return false;
		}
				src = mSwFrame;
		if (!ValidateDecodedFrameForView(src) || IsHwPixFmt(src->format))
		{
			mFallbackReason = "hwdownload-frame";
			return false;
		}
	}
	else if (IsHwPixFmt(src->format))
	{

				mFallbackReason = "unexpected-hwfmt";
		return false;
	}

	return ConvertToBGRA(src, dst, dstRowBytes, dstW, dstH, useHalfBands);
}


bool VideoDecoder::DecodeFrameToYUV420P(int64_t targetFrame,
									   uint8_t *dstY, int dstYRowBytes,
									   uint8_t *dstU, int dstURowBytes,
									   uint8_t *dstV, int dstVRowBytes,
									   int dstWidth, int dstHeight)
{
	std::lock_guard<std::mutex> lock(mMutex);
	mLastReadError = 0;

	if (mAlphaCodec || !mOpened || dstY == nullptr || dstU == nullptr || dstV == nullptr)
	{
		return false;
	}
	if (targetFrame < 0)
	{
		targetFrame = 0;
	}

	WebMiereDecodedFrameView view;
	const bool decoded = DecodeFrameToSurface(targetFrame, &view);
	if (decoded &&
		ConvertSurfaceToYUV420P(view, dstY, dstYRowBytes, dstU, dstURowBytes,
								dstV, dstVRowBytes, dstWidth, dstHeight))
	{
		return true;
	}

	if (mUseHw && mFallbackReason != nullptr && ReopenCpu())
	{
		if (DecodeFrameToSurface(targetFrame, &view))
		{
			return ConvertSurfaceToYUV420P(view, dstY, dstYRowBytes, dstU, dstURowBytes,
										   dstV, dstVRowBytes, dstWidth, dstHeight);
		}
	}
	return false;
}


bool VideoDecoder::ConvertSurfaceToYUV420P(const WebMiereDecodedFrameView &view,
										   uint8_t *dstY, int dstYRowBytes,
										   uint8_t *dstU, int dstURowBytes,
										   uint8_t *dstV, int dstVRowBytes,
										   int dstW, int dstH)
{
	const AVFrame *src = view.frame;
	if (src == nullptr || dstY == nullptr || dstU == nullptr || dstV == nullptr)
	{
		return false;
	}

	if (!WebMiereIsValidYuv420Size(dstW, dstH))
	{
				return false;
	}

	if (!RowBytesCanHold(dstYRowBytes, dstW) ||
		!RowBytesCanHold(dstURowBytes, dstW / 2) ||
		!RowBytesCanHold(dstVRowBytes, dstW / 2))
	{
		return false;
	}


#if WEBMIERE_ENABLE_CUDA_YUV420P_DIRECT
	if (view.isCuda)
	{
				if (ConvertCudaNV12ToYUV420P(view, dstY, dstYRowBytes, dstU, dstURowBytes,
									 dstV, dstVRowBytes, dstW, dstH))
		{
						return true;
		}
			}
#endif


	if (view.isCuda)
	{
		if (mCudaCleanupFailed || mDisableHw)
		{
			mFallbackReason = "cuda-cleanup";
			return false;
		}
		av_frame_unref(mSwFrame);
		int tr = av_hwframe_transfer_data(mSwFrame, src, 0);
		if (tr < 0)
		{
			mFallbackReason = "hwdownload";
						return false;
		}
				src = mSwFrame;
		if (!ValidateDecodedFrameForView(src) || IsHwPixFmt(src->format))
		{
			mFallbackReason = "hwdownload-frame";
			return false;
		}
	}
	else if (IsHwPixFmt(src->format))
	{
		mFallbackReason = "unexpected-hwfmt";
				return false;
	}


	if (src->color_range == AVCOL_RANGE_JPEG)
	{
				return false;
	}

	const int srcFmt = src->format;
	const int srcW   = src->width;
	const int srcH   = src->height;
	if (!WebMiereIsValidVideoSize(srcW, srcH))
	{
		return false;
	}


	const bool isYuv420p = (srcFmt == AV_PIX_FMT_YUV420P);
	const bool isNv12    = (srcFmt == AV_PIX_FMT_NV12);
	if (!isYuv420p && !isNv12)
	{
				return false;
	}
	if (isYuv420p && !ValidateYuv420PFrame(src))
	{
		return false;
	}
	if (isNv12 && !ValidateNV12Frame(src))
	{
		return false;
	}

	const int cW = dstW / 2;
	const int cH = dstH / 2;


	if (isYuv420p && srcW == dstW && srcH == dstH)
	{
		CopyPlaneRespectingRowBytes(dstY, dstYRowBytes, src->data[0], src->linesize[0], dstW, dstH);
		CopyPlaneRespectingRowBytes(dstU, dstURowBytes, src->data[1], src->linesize[1], cW, cH);
		CopyPlaneRespectingRowBytes(dstV, dstVRowBytes, src->data[2], src->linesize[2], cW, cH);
				return true;
	}


	if (mSwsYuv == nullptr ||
		srcFmt != mSwsYuvSrcFmt || srcW != mSwsYuvSrcW || srcH != mSwsYuvSrcH ||
		dstW != mSwsYuvDstW || dstH != mSwsYuvDstH)
	{
		mSwsYuv = sws_getCachedContext(mSwsYuv, srcW, srcH, static_cast<AVPixelFormat>(srcFmt),
									   dstW, dstH, AV_PIX_FMT_YUV420P,
									   mSwsFlags, nullptr, nullptr, nullptr);
		if (mSwsYuv == nullptr)
		{
			return false;
		}
		mSwsYuvSrcFmt = srcFmt;
		mSwsYuvSrcW   = srcW;
		mSwsYuvSrcH   = srcH;
		mSwsYuvDstW   = dstW;
		mSwsYuvDstH   = dstH;
			}


	uint8_t *dstData[4]     = { dstY, dstU, dstV, nullptr };
	int      dstLinesize[4] = { dstYRowBytes, dstURowBytes, dstVRowBytes, 0 };

	int scaledH = sws_scale(mSwsYuv, src->data, src->linesize, 0, srcH, dstData, dstLinesize);
	if (!(scaledH == dstH))
	{
		return false;
	}
		return true;
}


bool VideoDecoder::ConvertToBGRA(const AVFrame *f, uint8_t *dst, int dstRowBytes, int dstW, int dstH, bool useHalfBands)
{
	if (f == nullptr || dst == nullptr || !WebMiereIsValidVideoSize(dstW, dstH))
	{
		return false;
	}
	if (!ValidateDecodedFrameForView(f) || IsHwPixFmt(f->format))
	{
		return false;
	}
	if (dstW > std::numeric_limits<int>::max() / 4)
	{
		return false;
	}
	const int dstWidthBytes = dstW * 4;

	if (!RowBytesCanHold(dstRowBytes, dstWidthBytes))
	{
		return false;
	}

	const bool resizeAlpha = mAlphaCodec && (dstW != f->width || dstH != f->height);
	uint8_t *firstRow = dst + static_cast<ptrdiff_t>(dstH - 1) * dstRowBytes;
	if (!resizeAlpha && !ConvertColorToBGRA(f, firstRow, -dstRowBytes, dstW, dstH)) return false;
	if (!mAlphaCodec) return true;

	const AVFrame *mask = SoftwareAlphaFrame();
	if (!mask) return false;
	if (resizeAlpha)
	{
		if (useHalfBands) {
			if (!mHalfAlpha) mHalfAlpha = std::make_unique<HalfAlphaConverter>();
			return mHalfAlpha->Convert(f, mask, mColorScratch, dst, dstRowBytes, mSwsFlags, mFallbackReason);
		}
		if (!EnsureAlphaBgraBuffer(mColorScratch, f->width, f->height)) return false;
		if (!ConvertColorToBGRA(f, mColorScratch->data[0], mColorScratch->linesize[0], f->width, f->height))
			return false;
		webmiere::alpha_resize::Resize(mColorScratch->data[0], mColorScratch->linesize[0],
			mask->data[0], mask->linesize[0], f->width, f->height, dst, dstRowBytes, dstW, dstH);
		return true;
	}

	for (int y = 0; y < dstH; ++y)
	{
		uint8_t *row = firstRow - static_cast<ptrdiff_t>(y) * dstRowBytes;
		const uint8_t *alpha = mask->data[0] + static_cast<ptrdiff_t>(y) * mask->linesize[0];
		for (int x = 0; x < dstW; ++x) row[4 * x + 3] = alpha[x];
	}
	return true;
}

bool VideoDecoder::ConvertColorToBGRA(const AVFrame *f, uint8_t *firstRow, int stride, int dstW, int dstH)
{
	const int srcFmt = f->format;
	const int srcW   = f->width;
	const int srcH   = f->height;

	if (mSws == nullptr ||
		srcFmt != mSwsSrcFmt || srcW != mSwsSrcW || srcH != mSwsSrcH ||
		dstW != mSwsDstW || dstH != mSwsDstH)
	{

		mSws = sws_getCachedContext(mSws, srcW, srcH, static_cast<AVPixelFormat>(srcFmt),
									dstW, dstH, AV_PIX_FMT_BGRA,
									mSwsFlags, nullptr, nullptr, nullptr);
		if (mSws == nullptr)
		{
			mFallbackReason = "sws_context";
			return false;
		}

		const int   srcRange = (f->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
		const int  *coef709  = sws_getCoefficients(SWS_CS_ITU709);
		sws_setColorspaceDetails(mSws, coef709, srcRange, coef709, 1, 0, 1 << 16, 1 << 16);

		mSwsSrcFmt = srcFmt;
		mSwsSrcW   = srcW;
		mSwsSrcH   = srcH;
		mSwsDstW   = dstW;
		mSwsDstH   = dstH;

			}


	uint8_t *dstData[4]     = { firstRow, nullptr, nullptr, nullptr };
	int      dstLinesize[4] = { stride, 0, 0, 0 };

	int scaledH = sws_scale(mSws, f->data, f->linesize, 0, srcH, dstData, dstLinesize);
	if (!(scaledH == dstH))
	{
		mFallbackReason = "sws_scale";
		return false;
	}

	return true;
}



const AVFrame *VideoDecoder::SoftwareAlphaFrame()
{
	const AVFrame *mask = mAlphaFrame;
	if (mask->format == AV_PIX_FMT_CUDA)
	{
		if (mMaskDownload.Transfer(mAlphaSwFrame, mask) < 0)
		{
			mFallbackReason = "alpha-hwdownload";
			return nullptr;
		}
		mask = mAlphaSwFrame;
	}
	if ((mask->format != AV_PIX_FMT_YUV420P && mask->format != AV_PIX_FMT_NV12) ||
		!mask->data[0] || !RowBytesCanHold(mask->linesize[0], mask->width)) return nullptr;

	return mask;
}

bool VideoDecoder::BuildNppStreamContext(void *streamVoid) noexcept
{
#if WEBMIERE_ENABLE_NPP
	if (mNppStreamCtx == nullptr)
	{
		mNppStreamCtx = new (std::nothrow) NppStreamContext();
		if (mNppStreamCtx == nullptr)
		{
			return false;
		}
	}
	NppStreamContext *c = static_cast<NppStreamContext*>(mNppStreamCtx);
	std::memset(c, 0, sizeof(*c));
	c->hStream                            = reinterpret_cast<cudaStream_t>(streamVoid);
	c->nCudaDeviceId                      = mNppDeviceId;
	c->nMultiProcessorCount               = mNppMpCount;
	c->nMaxThreadsPerMultiProcessor       = mNppMaxThreadsPerMp;
	c->nMaxThreadsPerBlock                = mNppMaxThreadsPerBlock;
	c->nSharedMemPerBlock                 = mNppSharedMemPerBlock;
	c->nCudaDevAttrComputeCapabilityMajor = mNppCcMajor;
	c->nCudaDevAttrComputeCapabilityMinor = mNppCcMinor;
	c->nStreamFlags                       = mNppStreamFlags;
	return true;
#else
	(void)streamVoid;
	return false;
#endif
}


bool VideoDecoder::EnsureCudaConsumerStream(void **outStream) noexcept
{
#if WEBMIERE_ENABLE_NPP
	if (outStream == nullptr)
	{
		return false;
	}
	if (mCudaStream == nullptr)
	{
		cudaStream_t s = nullptr;
		if (cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking) != cudaSuccess)
		{
			return false;
		}
		mCudaStream = reinterpret_cast<void*>(s);
	}
	*outStream = mCudaStream;
	return true;
#else
	(void)outStream;
	return false;
#endif
}


bool VideoDecoder::EnsureNppStreamContext(void *streamVoid) noexcept
{
#if WEBMIERE_ENABLE_NPP
	if (mNppDevReady && mNppStream == streamVoid && mNppStreamCtx != nullptr)
	{
		return true;
	}

	int dev = 0;
	if (cudaGetDevice(&dev) != cudaSuccess)
	{
		return false;
	}
	cudaDeviceProp prop;
	if (cudaGetDeviceProperties(&prop, dev) != cudaSuccess)
	{
		return false;
	}
	unsigned int sflags = 0;
	cudaStream_t stream = reinterpret_cast<cudaStream_t>(streamVoid);
	if (stream != nullptr && cudaStreamGetFlags(stream, &sflags) != cudaSuccess)
	{
		return false;
	}

	mNppDeviceId           = dev;
	mNppMpCount            = prop.multiProcessorCount;
	mNppMaxThreadsPerMp    = prop.maxThreadsPerMultiProcessor;
	mNppMaxThreadsPerBlock = prop.maxThreadsPerBlock;
	mNppSharedMemPerBlock  = prop.sharedMemPerBlock;
	mNppCcMajor            = prop.major;
	mNppCcMinor            = prop.minor;
	mNppStreamFlags        = sflags;
	if (!BuildNppStreamContext(streamVoid))
	{
		return false;
	}
	mNppDevReady = true;
	mNppStream   = streamVoid;
	return true;
#else
	(void)streamVoid;
	return false;
#endif
}


bool VideoDecoder::DestroyCudaEventStrict() noexcept
{
#if WEBMIERE_ENABLE_NPP
	bool eventOk = true;
	if (mCudaProducerEvent != nullptr)
	{
		CUevent ev = reinterpret_cast<CUevent>(mCudaProducerEvent);
		if (cuEventDestroy(ev) == CUDA_SUCCESS)
		{
			mCudaProducerEvent = nullptr;
			mCudaProducerEventCtx = nullptr;
		}
		else
		{
			eventOk = false;
		}
	}
	return eventOk;
#else
	mCudaProducerEvent = nullptr;
	mCudaProducerEventCtx = nullptr;
	return true;
#endif
}


bool VideoDecoder::PrepareCudaSurfaceRead(AVCUDADeviceContext *cudaDev,
										  void **outStream,
										  bool *outQueuedWait) noexcept
{
#if WEBMIERE_ENABLE_NPP
	if (outStream == nullptr || cudaDev == nullptr || cudaDev->cuda_ctx == nullptr)
	{
		return false;
	}
	if (outQueuedWait != nullptr)
	{
		*outQueuedWait = false;
	}

	CUstream producerStream = cudaDev->stream;
	WebMiereCudaSyncMode mode = static_cast<WebMiereCudaSyncMode>(mCudaSyncMode);

	void *consumerStream = nullptr;
	if (mode == kWebMiereCudaSyncSameStream)
	{
		consumerStream = reinterpret_cast<void*>(producerStream);
		if (!EnsureNppStreamContext(consumerStream))
		{
			mode = kWebMiereCudaSyncProducer;
		}
		else
		{
			*outStream = consumerStream;
			return true;
		}
	}

	if (!EnsureCudaConsumerStream(&consumerStream))
	{
		return false;
	}
	if (!EnsureNppStreamContext(consumerStream))
	{
		return false;
	}

	CUstream consumerCuStream = reinterpret_cast<CUstream>(consumerStream);

	if (mode == kWebMiereCudaSyncEvent)
	{
		if (mCudaProducerEventCtx != reinterpret_cast<void*>(cudaDev->cuda_ctx))
		{
			if (!DestroyCudaEventStrict())
			{
				return false;
			}
			mCudaProducerEventCtx = reinterpret_cast<void*>(cudaDev->cuda_ctx);
		}
		if (mCudaProducerEvent == nullptr)
		{
			CUevent ev = nullptr;
			if (cuEventCreate(&ev, CU_EVENT_DISABLE_TIMING) == CUDA_SUCCESS)
			{
				mCudaProducerEvent = reinterpret_cast<void*>(ev);
			}
		}
		if (mCudaProducerEvent != nullptr)
		{
			CUevent ev = reinterpret_cast<CUevent>(mCudaProducerEvent);
			if (cuEventRecord(ev, producerStream) == CUDA_SUCCESS &&
				cuStreamWaitEvent(consumerCuStream, ev, 0) == CUDA_SUCCESS)
			{
				if (outQueuedWait != nullptr)
				{
					*outQueuedWait = true;
				}
				*outStream = consumerStream;
				return true;
			}
		}

		mode = kWebMiereCudaSyncProducer;
	}

	if (mode == kWebMiereCudaSyncProducer)
	{
		if (cuStreamSynchronize(producerStream) == CUDA_SUCCESS)
		{
			*outStream = consumerStream;
			return true;
		}
		mode = kWebMiereCudaSyncContext;
	}

	if (mode == kWebMiereCudaSyncContext)
	{
		if (cuCtxSynchronize() == CUDA_SUCCESS)
		{
			*outStream = consumerStream;
			return true;
		}
	}

	return false;
#else
	(void)cudaDev;
	(void)outStream;
	(void)outQueuedWait;
	return false;
#endif
}


#if WEBMIERE_ENABLE_NPP
bool VideoDecoder::EnsureAlphaResizeKernel()
{
	if (mAlphaResizeKernel) return true;
	CUresult result = CUDA_SUCCESS;
	if (!mAlphaResizeModule)
		result = cuModuleLoadData(&mAlphaResizeModule, kHalfAlphaPtx);
	if (result == CUDA_SUCCESS)
		result = cuModuleGetFunction(&mAlphaResizeKernel, mAlphaResizeModule, "ResizeAlpha709");
	if (result == CUDA_SUCCESS) return true;

	// Retry temporary failures (e.g. out of memory) on later frames.
	switch (result)
	{
	case CUDA_ERROR_INVALID_IMAGE:
	case CUDA_ERROR_NO_BINARY_FOR_GPU:
	case CUDA_ERROR_INVALID_PTX:
	case CUDA_ERROR_UNSUPPORTED_PTX_VERSION:
	case CUDA_ERROR_JIT_COMPILER_NOT_FOUND:
	case CUDA_ERROR_JIT_COMPILATION_DISABLED:
	case CUDA_ERROR_NOT_FOUND:
		mAlphaResizeUnavailableCtx = static_cast<CUcontext>(mCudaCtx);
		break;
	default:
		break;
	}
	av_log(nullptr, AV_LOG_DEBUG, "WebMiere CUDA alpha-resize kernel unavailable (%d)\n", static_cast<int>(result));
	return false;
}

// The caller has made the allocating FFmpeg CUDA context current.
bool VideoDecoder::FreePinnedStaging()
{
	if (!mPinnedStaging) return true;
	if (cudaFreeHost(mPinnedStaging) != cudaSuccess) return false;
	mPinnedStaging = nullptr;
	mPinnedCapacity = 0;
	return true;
}

// Called on the transition to host-side alpha conversion, before downloading
// the new frame. Keep the decoder, CUDA module, event and stream warm.
bool VideoDecoder::ReleaseAlphaGpuOutput()
{
	if (!mCudaBgr && !mCudaBgra && !mCudaFlip && !mPinnedStaging) return true;
	CuCtxPushGuard owner(static_cast<CUcontext>(mCudaCtx));
	if (!owner.ok()) return false;
	const bool deviceFreed = FreeCudaBuffersStrict();
	const bool hostFreed = FreePinnedStaging();
	return owner.pop() && deviceFreed && hostFreed;
}
#endif


bool VideoDecoder::EnsurePinned(size_t bytes)
{
#if WEBMIERE_ENABLE_NPP
	if (mCudaCleanupFailed)
	{
		return false;
	}
	if (mPinnedStaging != nullptr && mPinnedCapacity >= bytes)
	{
		return true;
	}
	if (!FreePinnedStaging())
	{
		MarkCudaCleanupFailed();
		return false;
	}
	void *p = nullptr;
	if (cudaHostAlloc(&p, bytes, cudaHostAllocDefault) != cudaSuccess)
	{
		return false;
	}
	mPinnedStaging  = static_cast<uint8_t*>(p);
	mPinnedCapacity = bytes;
	return true;
#else
	(void)bytes;
	return false;
#endif
}


#if WEBMIERE_ENABLE_NPP
bool VideoDecoder::DownloadBgraOutput(const AVFrame *color, uint8_t *dst, int dstPitch,
                                      int width, int height, void *streamHandle, bool resizeAlpha)
{
	const int rowBytes = width * 4;
	const bool direct = dstPitch > 0;
	const auto stream = static_cast<cudaStream_t>(streamHandle);
	const size_t stagingBytes = size_t(rowBytes) * mCudaBufH;
	// A smaller preview no longer needs the native frame's pinned allocation.
	if (resizeAlpha && mPinnedCapacity > stagingBytes && !FreePinnedStaging())
	{
		MarkCudaCleanupFailed();
		return false;
	}
	if (!direct && !EnsurePinned(stagingBytes)) return false;

	for (int first = 0; first < height; first += mCudaBufH)
	{
		int rows = std::min(mCudaBufH, height - first);
		if (resizeAlpha)
		{
			auto luma = color->data[0], chroma = color->data[1];
			int pitch = color->linesize[0], srcWidth = color->width, srcHeight = color->height;
			void *args[] = { &luma, &chroma, &mAlphaFrame->data[0], &pitch, &mAlphaFrame->linesize[0],
				&width, &height, &mCudaFlip, &mCudaFlipPitch, &srcWidth, &srcHeight, &first, &rows };
			if (cuLaunchKernel(mAlphaResizeKernel, (width + 31) / 32, (rows + 7) / 8, 1,
			                   32, 8, 1, 0, stream, args, nullptr) != CUDA_SUCCESS) return false;
		}

		uint8_t *hostRows = dst + static_cast<ptrdiff_t>(height - first - rows) * dstPitch;
		if (cudaMemcpy2DAsync(direct ? hostRows : mPinnedStaging, direct ? dstPitch : rowBytes,
		                      mCudaFlip, mCudaFlipPitch, rowBytes, rows,
		                      cudaMemcpyDeviceToHost, stream) != cudaSuccess) return false;
		if (!direct)
		{
			if (cudaStreamSynchronize(stream) != cudaSuccess) return false;
			CopyPlaneRespectingRowBytes(hostRows, dstPitch, mPinnedStaging, rowBytes, rowBytes, rows);
		}
	}
	// Direct copies and subsequent launches reuse the same buffer in stream order.
	return !direct || cudaStreamSynchronize(stream) == cudaSuccess;
}
#endif


bool VideoDecoder::ConvertToBGRA_CUDA(const AVFrame *f, uint8_t *dst, int dstRowBytes, int dstW, int dstH)
{
#if WEBMIERE_ENABLE_NPP
	if (mCudaCleanupFailed || mDisableHw)
	{
		mFallbackReason = "cuda-cleanup";
		return false;
	}
	if (f == nullptr || dst == nullptr)
	{
		return false;
	}

	if (!WebMiereIsValidVideoSize(dstW, dstH)) return false;
	const bool resizeAlpha = f->width != dstW || f->height != dstH;
	constexpr int maxReduction = 32;
	if (resizeAlpha && (!mAlphaCodec || mSwsFlags != SWS_BILINEAR || f->color_range == AVCOL_RANGE_JPEG ||
		dstW > f->width || dstH > f->height ||
		f->width > maxReduction * dstW || f->height > maxReduction * dstH))
	{
		return false;
	}
	if (dstW > std::numeric_limits<int>::max() / 4)
	{
		return false;
	}
	const int bgraStride = dstW * 4;
	// A full 1080p preview fits in one pass. Larger resizes reuse the same
	// bounded output buffer across rows, including near-native requests.
	constexpr int resizeBufferBytes = 8 * 1024 * 1024;
	const int outputRows = resizeAlpha ? std::min(dstH, resizeBufferBytes / bgraStride) : dstH;
	if (!RowBytesCanHold(dstRowBytes, bgraStride))
	{
		return false;
	}

	AVHWFramesContext    *fctx  = nullptr;
	AVCUDADeviceContext  *cudev = nullptr;
	if (!ValidateCudaNV12Frame(f, &fctx, &cudev))
	{
		return false;
	}
	AVCUDADeviceContext *alphaDevice = nullptr;
	if (mAlphaCodec && (!ValidateCudaNV12Frame(mAlphaFrame, nullptr, &alphaDevice) ||
		alphaDevice != cudev || mAlphaFrame->width != f->width || mAlphaFrame->height != f->height ||
        mAlphaFrame->linesize[0] < f->width))
	{
		return false;
	}
	(void)fctx;
	CUcontext cuCtx = cudev->cuda_ctx;
	if (resizeAlpha && mAlphaResizeUnavailableCtx == cuCtx) return false;

	if (mCudaCtx != nullptr && mCudaCtx != reinterpret_cast<void*>(cuCtx))
	{
				if (!ReleaseCudaBuffersInCtx(mCudaCtx))
		{
			MarkCudaCleanupFailed();
						return false;
		}
	}

	CuCtxPushGuard ctxGuard(cuCtx);
	if (!ctxGuard.ok())
	{
				return false;
	}
	mCudaCtx = reinterpret_cast<void*>(cuCtx);

	bool			ok    = false;
	do
	{
		if (resizeAlpha && !EnsureAlphaResizeKernel()) break;
		if ((!resizeAlpha && (!mCudaBgr || !mCudaBgra)) || !mCudaFlip ||
			mCudaBufW != dstW || mCudaBufH != outputRows)
		{


			if (!FreeCudaBuffersStrict())
			{
				MarkCudaCleanupFailed();
								break;
			}

			void *p0 = nullptr, *p1 = nullptr, *p2 = nullptr;
			if (!resizeAlpha) {
				if (cudaMallocPitch(&p0, &mCudaBgrPitch,  static_cast<size_t>(dstW) * 3, static_cast<size_t>(dstH)) != cudaSuccess) { break; }
				mCudaBgr = static_cast<uint8_t*>(p0);
				if (cudaMallocPitch(&p1, &mCudaBgraPitch, static_cast<size_t>(dstW) * 4, static_cast<size_t>(dstH)) != cudaSuccess) { break; }
				mCudaBgra = static_cast<uint8_t*>(p1);
				if (cudaMallocPitch(&p2, &mCudaFlipPitch, bgraStride, dstH) != cudaSuccess) { break; }
			} else {
				if (cudaMalloc(&p2, size_t(bgraStride) * outputRows) != cudaSuccess) { break; }
				mCudaFlipPitch = bgraStride;
			}
			mCudaFlip = static_cast<uint8_t*>(p2);

			mCudaBufW = dstW;
			mCudaBufH = outputRows;
					}

		void *streamVoid = nullptr;
		bool queuedWait = false;
		if (!PrepareCudaSurfaceRead(cudev, &streamVoid, &queuedWait))
		{
			break;
		}
		(void)queuedWait;
		cudaStream_t stream = reinterpret_cast<cudaStream_t>(streamVoid);

		if (!resizeAlpha)
		{
			if (mNppStreamCtx == nullptr)
			{
				break;
			}
			NppStreamContext &nppCtx = *static_cast<NppStreamContext*>(mNppStreamCtx);

			const NppiSize roi    = { dstW, dstH };
			const Npp8u   *pSrc[2] = {
				reinterpret_cast<const Npp8u*>(f->data[0]),
				reinterpret_cast<const Npp8u*>(f->data[1])
			};
			const int srcStep = f->linesize[0];

			if (nppiNV12ToBGR_709CSC_8u_P2C3R_Ctx(pSrc, srcStep, mCudaBgr, static_cast<int>(mCudaBgrPitch), roi, nppCtx) != NPP_SUCCESS)
			{
				break;
			}

			const int dstOrder[4] = { 0, 1, 2, 3 };
			if (nppiSwapChannels_8u_C3C4R_Ctx(mCudaBgr, static_cast<int>(mCudaBgrPitch), mCudaBgra, static_cast<int>(mCudaBgraPitch), roi, dstOrder, 255, nppCtx) != NPP_SUCCESS)
			{
				break;
			}

			if (mAlphaCodec && nppiCopy_8u_C1C4R_Ctx(mAlphaFrame->data[0], mAlphaFrame->linesize[0],
					mCudaBgra + 3, static_cast<int>(mCudaBgraPitch), roi, nppCtx) != NPP_SUCCESS)
			{
				break;
			}

			if (nppiMirror_8u_C4R_Ctx(mCudaBgra, static_cast<int>(mCudaBgraPitch), mCudaFlip, static_cast<int>(mCudaFlipPitch), roi, NPP_HORIZONTAL_AXIS, nppCtx) != NPP_SUCCESS)
			{
				break;
			}


		}

		if (!DownloadBgraOutput(f, dst, dstRowBytes, dstW, dstH, stream, resizeAlpha)) break;

		// Both native and reduced GPU output replace the software workspace.
		// Drop its backing buffers only after the GPU and host copy have finished.
		if (mAlphaCodec && (mColorScratch || mSwFrame->buf[0] || mAlphaSwFrame->buf[0]))
		{
			av_frame_free(&mColorScratch);
			av_frame_unref(mSwFrame);
			av_frame_unref(mAlphaSwFrame);
			mColorDownload = {};
			mMaskDownload = {};
		}
        if (resizeAlpha) {
            av_log(nullptr, AV_LOG_DEBUG, f->width == 2 * dstW && f->height == 2 * dstH
                ? "WebMiere CUDA half-alpha frame complete\n" : "WebMiere CUDA alpha-resize frame complete\n");
        }
		ok = true;
	}
	while (false);


	const bool		popOk  = ctxGuard.pop();
	if (!popOk)
	{
		MarkCudaCleanupFailed();
				ok = false;
	}


	if (!ok && popOk)
	{
		if (!ReleaseCudaBuffersInCtx(mCudaCtx))
		{
			MarkCudaCleanupFailed();
		}
	}

	return ok;
#else
	(void)f; (void)dst; (void)dstRowBytes; (void)dstW; (void)dstH;
	return false;
#endif
}


bool VideoDecoder::ConvertCudaNV12ToYUV420P(const WebMiereDecodedFrameView &view,
											uint8_t *dstY, int dstYRowBytes,
											uint8_t *dstU, int dstURowBytes,
											uint8_t *dstV, int dstVRowBytes,
											int dstW, int dstH)
{
#if WEBMIERE_ENABLE_NPP
	const AVFrame *f = view.frame;

	if (mCudaCleanupFailed || mDisableHw)
	{
		mFallbackReason = "cuda-cleanup";
		return false;
	}
	if (f == nullptr || dstY == nullptr || dstU == nullptr || dstV == nullptr)
	{
		return false;
	}
	if (!WebMiereIsValidYuv420Size(dstW, dstH))
	{
		return false;
	}
	if (f->width != dstW || f->height != dstH)
	{
		return false;
	}
	if (f->color_range == AVCOL_RANGE_JPEG)
	{
		return false;
	}


	AVHWFramesContext    *fctx  = nullptr;
	AVCUDADeviceContext  *cudev = nullptr;
	if (!ValidateCudaNV12Frame(f, &fctx, &cudev))
	{
		return false;
	}
	(void)fctx;
	CUcontext cuCtx = cudev->cuda_ctx;


	const int srcStep = f->linesize[0];

	if (mCudaCtx != nullptr && mCudaCtx != reinterpret_cast<void*>(cuCtx))
	{
				if (!ReleaseCudaBuffersInCtx(mCudaCtx))
		{
			MarkCudaCleanupFailed();
						return false;
		}
	}

	CuCtxPushGuard ctxGuard(cuCtx);
	if (!ctxGuard.ok())
	{
				return false;
	}
	mCudaCtx = reinterpret_cast<void*>(cuCtx);

	const int cW = dstW / 2;
	const int cH = dstH / 2;

	bool			ok    = false;
	do
	{
		if (mCudaYuvY == nullptr || mCudaYuvU == nullptr || mCudaYuvV == nullptr ||
			mCudaYuvBufW != dstW || mCudaYuvBufH != dstH)
		{
			if (!FreeCudaYuvBuffersStrict())
			{
				MarkCudaCleanupFailed();
								break;
			}
			void *p0 = nullptr, *p1 = nullptr, *p2 = nullptr;
			if (cudaMallocPitch(&p0, &mCudaYuvYPitch, static_cast<size_t>(dstW), static_cast<size_t>(dstH)) != cudaSuccess) { break; }
			mCudaYuvY = static_cast<uint8_t*>(p0);
			if (cudaMallocPitch(&p1, &mCudaYuvUPitch, static_cast<size_t>(cW), static_cast<size_t>(cH)) != cudaSuccess) { break; }
			mCudaYuvU = static_cast<uint8_t*>(p1);
			if (cudaMallocPitch(&p2, &mCudaYuvVPitch, static_cast<size_t>(cW), static_cast<size_t>(cH)) != cudaSuccess) { break; }
			mCudaYuvV = static_cast<uint8_t*>(p2);

			mCudaYuvBufW = dstW;
			mCudaYuvBufH = dstH;
					}

		void *streamVoid = nullptr;
		bool queuedWait = false;
		if (!PrepareCudaSurfaceRead(cudev, &streamVoid, &queuedWait))
		{
			break;
		}
		(void)queuedWait;
		cudaStream_t stream = reinterpret_cast<cudaStream_t>(streamVoid);

		if (mNppStreamCtx == nullptr)
		{
			break;
		}
		NppStreamContext &nppCtx = *static_cast<NppStreamContext*>(mNppStreamCtx);

		const Npp8u *pSrc[2] = {
			reinterpret_cast<const Npp8u*>(f->data[0]),
			reinterpret_cast<const Npp8u*>(f->data[1])
		};
		Npp8u *pDst[3] = { mCudaYuvY, mCudaYuvU, mCudaYuvV };
		int    aDstStep[3] = {
			static_cast<int>(mCudaYuvYPitch),
			static_cast<int>(mCudaYuvUPitch),
			static_cast<int>(mCudaYuvVPitch)
		};
		const NppiSize roi = { dstW, dstH };

		if (nppiNV12ToYUV420_8u_P2P3R_Ctx(pSrc, srcStep, pDst, aDstStep, roi, nppCtx) != NPP_SUCCESS)
		{
			break;
		}


		const bool canCopyDirect =
			dstYRowBytes >= dstW &&
			dstURowBytes >= cW &&
			dstVRowBytes >= cW;

		if (canCopyDirect)
		{
			if (cudaMemcpy2DAsync(dstY, static_cast<size_t>(dstYRowBytes),
							  mCudaYuvY, mCudaYuvYPitch,
							  static_cast<size_t>(dstW), static_cast<size_t>(dstH),
							  cudaMemcpyDeviceToHost, stream) != cudaSuccess) { break; }
			if (cudaMemcpy2DAsync(dstU, static_cast<size_t>(dstURowBytes),
							  mCudaYuvU, mCudaYuvUPitch,
							  static_cast<size_t>(cW), static_cast<size_t>(cH),
							  cudaMemcpyDeviceToHost, stream) != cudaSuccess) { break; }
			if (cudaMemcpy2DAsync(dstV, static_cast<size_t>(dstVRowBytes),
							  mCudaYuvV, mCudaYuvVPitch,
							  static_cast<size_t>(cW), static_cast<size_t>(cH),
							  cudaMemcpyDeviceToHost, stream) != cudaSuccess) { break; }
			if (cudaStreamSynchronize(stream) != cudaSuccess) { break; }
		}
		else
		{
			const size_t ySize = static_cast<size_t>(dstW) * static_cast<size_t>(dstH);
			const size_t cSize = static_cast<size_t>(cW) * static_cast<size_t>(cH);
			const size_t need  = ySize + cSize * 2;
			if (!EnsurePinned(need))
			{
				break;
			}
			uint8_t *py = mPinnedStaging;
			uint8_t *pu = py + ySize;
			uint8_t *pv = pu + cSize;

			if (cudaMemcpy2DAsync(py, static_cast<size_t>(dstW), mCudaYuvY, mCudaYuvYPitch,
							  static_cast<size_t>(dstW), static_cast<size_t>(dstH),
							  cudaMemcpyDeviceToHost, stream) != cudaSuccess) { break; }
			if (cudaMemcpy2DAsync(pu, static_cast<size_t>(cW), mCudaYuvU, mCudaYuvUPitch,
							  static_cast<size_t>(cW), static_cast<size_t>(cH),
							  cudaMemcpyDeviceToHost, stream) != cudaSuccess) { break; }
			if (cudaMemcpy2DAsync(pv, static_cast<size_t>(cW), mCudaYuvV, mCudaYuvVPitch,
							  static_cast<size_t>(cW), static_cast<size_t>(cH),
							  cudaMemcpyDeviceToHost, stream) != cudaSuccess) { break; }
			if (cudaStreamSynchronize(stream) != cudaSuccess) { break; }

			CopyPlaneRespectingRowBytes(dstY, dstYRowBytes, py, dstW, dstW, dstH);
			CopyPlaneRespectingRowBytes(dstU, dstURowBytes, pu, cW,   cW,   cH);
			CopyPlaneRespectingRowBytes(dstV, dstVRowBytes, pv, cW,   cW,   cH);
		}

				ok = true;
	}
	while (false);


	const bool		popOk  = ctxGuard.pop();
	if (!popOk)
	{
		MarkCudaCleanupFailed();
				ok = false;
	}


	if (!ok && popOk)
	{
		if (!ReleaseCudaBuffersInCtx(mCudaCtx))
		{
			MarkCudaCleanupFailed();
		}
	}

	return ok;
#else
	(void)view; (void)dstY; (void)dstYRowBytes; (void)dstU; (void)dstURowBytes;
	(void)dstV; (void)dstVRowBytes; (void)dstW; (void)dstH;
	return false;
#endif
}


bool VideoDecoder::ReleaseCudaBuffers()
{
	const bool ok = ReleaseCudaBuffersInCtx(mCudaCtx);
	if (!ok)
	{
		MarkCudaCleanupFailed();
	}
	return ok;
}


void VideoDecoder::MarkCudaCleanupFailed() noexcept
{
	mCudaCleanupFailed = true;
	mDisableHw = true;
	mNppDevReady = false;
	mFallbackReason = "cuda-cleanup";
}


bool VideoDecoder::FreeCudaBuffersStrict()
{
#if WEBMIERE_ENABLE_NPP
	bool freeOk = true;
	if (mCudaBgr)
	{
		if (cudaFree(mCudaBgr) == cudaSuccess) { mCudaBgr = nullptr; mCudaBgrPitch = 0; }
		else { freeOk = false; }
	}
	if (mCudaBgra)
	{
		if (cudaFree(mCudaBgra) == cudaSuccess) { mCudaBgra = nullptr; mCudaBgraPitch = 0; }
		else { freeOk = false; }
	}
	if (mCudaFlip)
	{
		if (cudaFree(mCudaFlip) == cudaSuccess) { mCudaFlip = nullptr; mCudaFlipPitch = 0; }
		else { freeOk = false; }
	}

	if (freeOk)
	{
		mCudaBufW = 0;
		mCudaBufH = 0;
	}
	return freeOk;
#else
	mCudaBgr = nullptr; mCudaBgra = nullptr; mCudaFlip = nullptr;
	mCudaBgrPitch = 0; mCudaBgraPitch = 0; mCudaFlipPitch = 0;
	mCudaBufW = 0; mCudaBufH = 0;
	return true;
#endif
}


bool VideoDecoder::FreeCudaYuvBuffersStrict()
{
#if WEBMIERE_ENABLE_NPP
	bool freeOk = true;
	if (mCudaYuvY)
	{
		if (cudaFree(mCudaYuvY) == cudaSuccess) { mCudaYuvY = nullptr; mCudaYuvYPitch = 0; }
		else { freeOk = false; }
	}
	if (mCudaYuvU)
	{
		if (cudaFree(mCudaYuvU) == cudaSuccess) { mCudaYuvU = nullptr; mCudaYuvUPitch = 0; }
		else { freeOk = false; }
	}
	if (mCudaYuvV)
	{
		if (cudaFree(mCudaYuvV) == cudaSuccess) { mCudaYuvV = nullptr; mCudaYuvVPitch = 0; }
		else { freeOk = false; }
	}
	if (freeOk)
	{
		mCudaYuvBufW = 0;
		mCudaYuvBufH = 0;
	}
	return freeOk;
#else
	mCudaYuvY = nullptr; mCudaYuvU = nullptr; mCudaYuvV = nullptr;
	mCudaYuvYPitch = 0; mCudaYuvUPitch = 0; mCudaYuvVPitch = 0;
	mCudaYuvBufW = 0; mCudaYuvBufH = 0;
	return true;
#endif
}


bool VideoDecoder::DestroyStreamStrict()
{
#if WEBMIERE_ENABLE_NPP
	bool streamOk = true;
	if (mCudaStream)
	{
		cudaStream_t s = reinterpret_cast<cudaStream_t>(mCudaStream);
		if (cudaStreamDestroy(s) == cudaSuccess) { mCudaStream = nullptr; }
		else { streamOk = false; }
	}
	return streamOk;
#else
	mCudaStream = nullptr;
	return true;
#endif
}


bool VideoDecoder::ReleaseCudaBuffersInCtx(void *ctxVoid)
{
#if WEBMIERE_ENABLE_NPP
	if (mCudaBgr == nullptr && mCudaBgra == nullptr && mCudaFlip == nullptr &&
		mCudaYuvY == nullptr && mCudaYuvU == nullptr && mCudaYuvV == nullptr &&
		mCudaStream == nullptr && mCudaProducerEvent == nullptr && !mAlphaResizeModule && !mPinnedStaging)
	{
		mCudaBufW = 0; mCudaBufH = 0; mCudaYuvBufW = 0; mCudaYuvBufH = 0;
		mCudaCtx = nullptr; mNppDevReady = false; mNppStream = nullptr;
		return true;
	}
	if (ctxVoid == nullptr)
	{
		return false;
	}

	CUcontext ctx = reinterpret_cast<CUcontext>(ctxVoid);
	if (cuCtxPushCurrent(ctx) != CUDA_SUCCESS)
	{
		return false;
	}

	const bool moduleOk = !mAlphaResizeModule || cuModuleUnload(mAlphaResizeModule) == CUDA_SUCCESS;
	if (moduleOk) { mAlphaResizeModule = nullptr; mAlphaResizeKernel = nullptr; }
	const bool freeOk    = FreeCudaBuffersStrict();
	const bool freeYuvOk = FreeCudaYuvBuffersStrict();
	const bool pinnedOk  = FreePinnedStaging();
	const bool eventOk   = DestroyCudaEventStrict();
	const bool streamOk  = DestroyStreamStrict();


	CUcontext		popped = nullptr;
	const CUresult	popRes = cuCtxPopCurrent(&popped);


	if (!freeOk || !moduleOk || !pinnedOk)
	{

		mNppDevReady = false;
		return false;
	}
	if (!freeYuvOk)
	{

		mNppDevReady = false;
		return false;
	}
	if (!streamOk)
	{

		mNppDevReady = false;
		return false;
	}
	if (!eventOk)
	{

		mNppDevReady = false;
		return false;
	}
	if (popRes != CUDA_SUCCESS)
	{

		mNppDevReady = false;
		return false;
	}

	mCudaCtx = nullptr; mNppDevReady = false; mNppStream = nullptr;
	return true;
#else
	(void)ctxVoid;
	FreeCudaBuffersStrict();
	FreeCudaYuvBuffersStrict();
	DestroyCudaEventStrict();
	DestroyStreamStrict();
	mCudaCtx = nullptr; mNppDevReady = false; mNppStream = nullptr;
	return true;
#endif
}
