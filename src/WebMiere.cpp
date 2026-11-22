
// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.



#include "WebMiere.h"
#include "Demuxer.h"
#include "VideoDecoder.h"
#include "AudioDecoder.h"
#include "WebMiereLimits.h"
#include "PrSDKAppInfoSuite.h"
#include "PrSDKImporterFileManagerSuite.h"

#include <bcrypt.h>
#include <string>
#include <cstring>
#include <mutex>
#include <new>
#include <memory>
#include <cstdint>
#include <limits>
#include <cwchar>
#include <cstdio>


struct WebMiereInstanceLocks
{
	std::recursive_mutex video;
	std::recursive_mutex audio;
};


namespace {
class WebMiereScopedLock
{
public:
	explicit WebMiereScopedLock(std::recursive_mutex *m) : mMutex(m) { if (mMutex) mMutex->lock(); }
	~WebMiereScopedLock() { if (mMutex) mMutex->unlock(); }
	WebMiereScopedLock(const WebMiereScopedLock&) = delete;
	WebMiereScopedLock& operator=(const WebMiereScopedLock&) = delete;
private:
	std::recursive_mutex *mMutex;
};


class WebMiereHandleLock
{
public:
	WebMiereHandleLock(imStdParms *stdParms, ImporterLocalRecH h)
		: mMem((stdParms != NULL && stdParms->piSuites != NULL) ? stdParms->piSuites->memFuncs : NULL)
		, mHandle(h)
		, mData(NULL)
		, mLocked(false)
	{
		if (mMem != NULL && mHandle != NULL)
		{
			mMem->lockHandle(reinterpret_cast<char**>(mHandle));
			mLocked = true;
			mData = *mHandle;
		}
	}

	~WebMiereHandleLock()
	{
		Unlock();
	}

	WebMiereHandleLock(const WebMiereHandleLock&) = delete;
	WebMiereHandleLock& operator=(const WebMiereHandleLock&) = delete;

	ImporterLocalRecP Get() const
	{
		return mData;
	}

	bool IsValid() const
	{
		return mLocked && mData != NULL;
	}

	void Refresh()
	{
		if (mLocked && mHandle != NULL)
		{
			mData = *mHandle;
		}
	}

	void Unlock()
	{
		if (mLocked && mMem != NULL && mHandle != NULL)
		{
			mMem->unlockHandle(reinterpret_cast<char**>(mHandle));
		}
		mLocked = false;
		mData = NULL;
	}

private:
	PlugMemoryFuncsPtr	mMem;
	ImporterLocalRecH	mHandle;
	ImporterLocalRecP	mData;
	bool				mLocked;
};
}


static std::recursive_mutex *
WebMiereVideoLock(ImporterLocalRecH h)
{
	if (h != NULL && *h != NULL && (*h)->locks != NULL)
	{
		return &reinterpret_cast<WebMiereInstanceLocks*>((*h)->locks)->video;
	}
	return NULL;
}


static std::recursive_mutex *
WebMiereVideoLock(ImporterLocalRecP p)
{
	if (p != NULL && p->locks != NULL)
	{
		return &reinterpret_cast<WebMiereInstanceLocks*>(p->locks)->video;
	}
	return NULL;
}


static std::recursive_mutex *
WebMiereAudioLock(ImporterLocalRecH h)
{
	if (h != NULL && *h != NULL && (*h)->locks != NULL)
	{
		return &reinterpret_cast<WebMiereInstanceLocks*>((*h)->locks)->audio;
	}
	return NULL;
}


static std::recursive_mutex *
WebMiereAudioLock(ImporterLocalRecP p)
{
	if (p != NULL && p->locks != NULL)
	{
		return &reinterpret_cast<WebMiereInstanceLocks*>(p->locks)->audio;
	}
	return NULL;
}


static void
WebMiereFreeLocks(ImporterLocalRecH h)
{
	if (h != NULL && *h != NULL && (*h)->locks != NULL)
	{
		delete reinterpret_cast<WebMiereInstanceLocks*>((*h)->locks);
		(*h)->locks = NULL;
	}
}


static bool
WebMiereEnsureLocks(ImporterLocalRecP p)
{
	if (p == NULL)
	{
		return false;
	}
	if (p->locks == NULL)
	{
		p->locks = new (std::nothrow) WebMiereInstanceLocks();
	}
	return p->locks != NULL;
}


static bool
WebMiereHasMemoryFuncs(imStdParms *stdParms)
{
	return stdParms != NULL &&
		   stdParms->piSuites != NULL &&
		   stdParms->piSuites->memFuncs != NULL;
}


static bool
WebMiereHasRequiredSuites(imStdParms *stdParms)
{
	return WebMiereHasMemoryFuncs(stdParms) &&
		   stdParms->piSuites->utilFuncs != NULL;
}


static bool
WebMiereSetImporterStreamFileCount(
	imStdParms		*stdParms,
	csSDK_uint32	importerID,
	csSDK_int32		streamIndex,
	csSDK_int32		fileCount)
{
	if (!WebMiereHasRequiredSuites(stdParms) || fileCount <= 0)
	{
		return false;
	}

	SPBasicSuite *basicSuite = stdParms->piSuites->utilFuncs->getSPBasicSuite();
	if (basicSuite == NULL)
	{
		return false;
	}

	const void *fileManagerSuiteRaw = NULL;
	const SPErr acquireResult = basicSuite->AcquireSuite(
		kPrSDKImporterFileManagerSuite,
		kPrSDKImporterFileManagerSuiteVersion,
		&fileManagerSuiteRaw);
	const PrSDKImporterFileManagerSuite *fileManagerSuite =
		static_cast<const PrSDKImporterFileManagerSuite*>(fileManagerSuiteRaw);
	if (acquireResult != kSPNoError || fileManagerSuite == NULL)
	{
		if (acquireResult == kSPNoError)
		{
			basicSuite->ReleaseSuite(
				kPrSDKImporterFileManagerSuite,
				kPrSDKImporterFileManagerSuiteVersion);
		}
		return false;
	}

	const prSuiteError setResult = fileManagerSuite->SetImporterStreamFileCount(
		importerID, streamIndex, fileCount);
	basicSuite->ReleaseSuite(
		kPrSDKImporterFileManagerSuite,
		kPrSDKImporterFileManagerSuiteVersion);
	return setResult == suiteError_NoError;
}


static void
WebMiereDisposePrivateDataHandle(imStdParms *stdParms, ImporterLocalRecH ldataH, void **privateData)
{
	if (privateData != NULL)
	{
		*privateData = NULL;
	}
	if (WebMiereHasMemoryFuncs(stdParms) && ldataH != NULL)
	{
		stdParms->piSuites->memFuncs->disposeHandle(reinterpret_cast<char**>(ldataH));
	}
}


static void
WebMiereReleaseSuites(ImporterLocalRecP ldata)
{
	if (ldata == NULL || ldata->BasicSuite == NULL)
	{
		return;
	}

	SPBasicSuite *bs = ldata->BasicSuite;
	if (ldata->PPixCreatorSuite != NULL)
	{
		bs->ReleaseSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion);
		ldata->PPixCreatorSuite = NULL;
	}
	if (ldata->PPixSuite != NULL)
	{
		bs->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
		ldata->PPixSuite = NULL;
	}
	if (ldata->PPix2Suite != NULL)
	{
		bs->ReleaseSuite(kPrSDKPPix2Suite, kPrSDKPPix2SuiteVersion);
		ldata->PPix2Suite = NULL;
	}
	if (ldata->TimeSuite != NULL)
	{
		bs->ReleaseSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion);
		ldata->TimeSuite = NULL;
	}
	ldata->BasicSuite = NULL;
}


static bool
WebMiereComputeFrameRatePrTime(PrTime ticksPerSecond, int frameRateNum, int frameRateDen, PrTime *outPrTime)
{
	if (outPrTime == NULL)
	{
		return false;
	}
	*outPrTime = 0;
	if (ticksPerSecond <= 0 || frameRateNum <= 0 || frameRateDen <= 0)
	{
		return false;
	}
	if (ticksPerSecond > (std::numeric_limits<PrTime>::max)() / static_cast<PrTime>(frameRateDen))
	{
		return false;
	}

	const PrTime ticksTimesDen = ticksPerSecond * static_cast<PrTime>(frameRateDen);
	const PrTime prTime = ticksTimesDen / static_cast<PrTime>(frameRateNum);
	if (prTime <= 0)
	{
		return false;
	}
	*outPrTime = prTime;
	return true;
}

static PrAudioSample
WebMiereSaturatingAddAudioSamples(PrAudioSample a, int64_t b)
{
	if (b <= 0)
	{
		return a;
	}
	const PrAudioSample maxValue = (std::numeric_limits<PrAudioSample>::max)();
	if (a > maxValue - static_cast<PrAudioSample>(b))
	{
		return maxValue;
	}
	return a + static_cast<PrAudioSample>(b);
}


static void
WebMiereResetFileInfoStreamFields(imFileInfoRec8 *fileInfo8)
{
	fileInfo8->hasVideo = kPrFalse;
	fileInfo8->hasAudio = kPrFalse;
	fileInfo8->streamsAsComp = kPrFalse;
	fileInfo8->streamName[0] = 0;

	fileInfo8->vidScale = 0;
	fileInfo8->vidSampleSize = 0;
	fileInfo8->vidDuration = 0;
	fileInfo8->vidDurationInFrames = 0;
	fileInfo8->vidInfo.subType = 0;
	fileInfo8->vidInfo.imageWidth = 0;
	fileInfo8->vidInfo.imageHeight = 0;
	fileInfo8->vidInfo.depth = 0;
	fileInfo8->vidInfo.fieldType = prFieldsNone;
	fileInfo8->vidInfo.alphaType = alphaNone;
	fileInfo8->vidInfo.pixelAspectNum = 0;
	fileInfo8->vidInfo.pixelAspectDen = 0;
	fileInfo8->vidInfo.supportsAsyncIO = kPrFalse;
	fileInfo8->vidInfo.supportsGetSourceVideo = kPrFalse;
	fileInfo8->vidInfo.hasPulldown = kPrFalse;

	fileInfo8->audInfo.numChannels = 0;
	fileInfo8->audInfo.sampleRate = 0.0f;
	fileInfo8->audInfo.sampleType = kPrAudioSampleType_Compressed;
	fileInfo8->audDuration = 0;
}


static bool
WebMiereApplyProbeStreamMapping(
	ImporterLocalRecP		ldata,
	const MediaProbeInfo	&probe,
	csSDK_int32				premiereStreamIdx)
{
	if (ldata == NULL)
	{
		return false;
	}

	const int streamCount = (probe.audioStreamCount > 0) ? probe.audioStreamCount : 1;
	if (premiereStreamIdx < 0 || premiereStreamIdx >= streamCount)
	{
		return false;
	}

	ldata->premiereStreamIdx = premiereStreamIdx;
	ldata->hasVideo = (premiereStreamIdx == 0) ? kPrTrue : kPrFalse;
	ldata->hasAlpha = ldata->hasVideo && probe.hasAlpha;
	ldata->hasAudio = (probe.audioStreamCount > 0) ? kPrTrue : kPrFalse;
	ldata->width = probe.width;
	ldata->height = probe.height;
	ldata->frameRateScale = probe.frameRateNum;
	ldata->frameRateSampleSize = probe.frameRateDen;
	ldata->numFrames = static_cast<csSDK_int64>(probe.numFrames);

	ldata->audioSampleRate = 0.0f;
	ldata->numChannels = 0;
	ldata->numSampleFrames = 0;
	ldata->ffmpegAudioStreamIndex = -1;
	if (ldata->hasAudio)
	{
		const MediaProbeInfo::AudioStreamProbeInfo &audio =
			probe.audioStreams[premiereStreamIdx];
		ldata->audioSampleRate = static_cast<float>(audio.sampleRate);
		ldata->numChannels = audio.channels;
		ldata->numSampleFrames = static_cast<PrAudioSample>(audio.sourceSampleFrames);
		ldata->ffmpegAudioStreamIndex = audio.ffmpegStreamIndex;
	}

	ldata->savedAudioPosition = 0;
	ldata->savedSequentialAudioPosition = 0;
	return true;
}


static prMALError SDKInit(imStdParms *stdParms, imImportInfoRec *importInfo);
static prMALError SDKShutdown(imStdParms *stdParms);
static prMALError SDKGetIndFormat(imStdParms *stdParms, csSDK_size_t index, imIndFormatRec *formatRec);
static prMALError SDKGetIndPixelFormat(imStdParms *stdParms, csSDK_size_t index, imIndPixelFormatRec *pixelFormatRec);
static prMALError SDKGetInfo8(imStdParms *stdParms, imFileAccessRec8 *fileAccessInfo8, imFileInfoRec8 *fileInfo8);
static prMALError SDKOpenFile8(imStdParms *stdParms, imFileRef *fileRef, imFileOpenRec8 *fileOpenRec8);
static prMALError SDKQuietFile(imStdParms *stdParms, imFileRef *fileRef, void *privateData);
static prMALError SDKCloseFile(imStdParms *stdParms, imFileRef *fileRef, void *privateData);
static prMALError SDKPreferredFrameSize(imStdParms *stdParms, imPreferredFrameSizeRec *preferredFrameSizeRec);
static prMALError SDKGetSourceVideo(imStdParms *stdParms, imFileRef fileRef, imSourceVideoRec *sourceVideoRec);
static prMALError SDKImportAudio7(imStdParms *stdParms, imFileRef fileRef, imImportAudioRec7 *audioRec7);
static prMALError SDKResetSequentialAudio(imStdParms *stdParms, imFileRef fileRef, imImportAudioRec7 *audioRec7);
static prMALError SDKGetSequentialAudio(imStdParms *stdParms, imFileRef fileRef, imImportAudioRec7 *audioRec7);


static VideoDecoder *EnsureDecoder(ImporterLocalRecH ldataH);
static AudioDecoder *EnsureAudioDecoder(ImporterLocalRecH ldataH);
static AudioDecoder *EnsureSequentialAudioDecoder(ImporterLocalRecH ldataH);


static bool       IsValidImporterFileRef(imFileRef fileRef);
static void       ReleaseOpenResources(ImporterLocalRecP ldata, imFileRef *fileRef);
static bool       IsBgraPixelFormat(PrPixelFormat fmt);
static bool       SelectBgraFallbackFormat(const imFrameFormat *formats, csSDK_int32 numFormats,
										   bool ppixAvailable,
										   csSDK_int32 nativeWidth, csSDK_int32 nativeHeight,
										   WebMiereOutputFormat *out);
static bool       SelectOutputFormat(const imFrameFormat *formats, csSDK_int32 numFormats,
									 bool ppixAvailable, bool ppix2Available,
									 csSDK_int32 nativeWidth, csSDK_int32 nativeHeight,
									 WebMiereOutputFormat *out);
static prMALError CreateOutputPPix(ImporterLocalRecP ldata, PPixHand *outFrame,
								   const WebMiereOutputFormat &fmt,
								   char **outBuffer, csSDK_int32 *outRowBytes);
static void       DisposeOutputPPix(ImporterLocalRecP ldata, PPixHand *outFrame);
#if WEBMIERE_ENABLE_YUV420P_RETURN

static bool       TryGetSourceVideoYUV420P(ImporterLocalRecP ldata, imSourceVideoRec *sourceVideoRec,
										   const WebMiereOutputFormat &outFmt, VideoDecoder *decoder,
										   int64_t theFrame);
#endif


static bool
IsValidImporterFileRef(imFileRef fileRef)
{
	return fileRef != NULL && fileRef != imInvalidHandleValue;
}


static void
ReleaseOpenResources(
	ImporterLocalRecP	ldata,
	imFileRef			*fileRef)
{
	if (ldata)
	{
		{
			WebMiereScopedLock lock(WebMiereVideoLock(ldata));
			if (ldata->decoder)
			{
				delete ldata->decoder;
				ldata->decoder = NULL;
			}
		}
		{
			WebMiereScopedLock lock(WebMiereAudioLock(ldata));
			if (ldata->audioDecoder)
			{
				delete ldata->audioDecoder;
				ldata->audioDecoder = NULL;
			}
			if (ldata->sequentialAudioDecoder)
			{
				delete ldata->sequentialAudioDecoder;
				ldata->sequentialAudioDecoder = NULL;
			}
		}
	}

	if (fileRef && IsValidImporterFileRef(*fileRef))
	{
		if (ldata && ldata->fileRef == *fileRef)
		{
			ldata->fileRef = imInvalidHandleValue;
		}
		CloseHandle(*fileRef);
		*fileRef = imInvalidHandleValue;
	}

	if (ldata && IsValidImporterFileRef(ldata->fileRef))
	{
		CloseHandle(ldata->fileRef);
		ldata->fileRef = imInvalidHandleValue;
	}
}


static csSDK_int32
WebMiereFiletypeForPath(const prUTF16Char *path)
{
	if (path == NULL)
	{
		return WEBMIERE_FILETYPE_WEBM;
	}

	size_t len = 0;
	while (path[len] != 0)
	{
		len++;
	}

	if (len >= 4)
	{
		const prUTF16Char *ext = path + (len - 4);
		const int c1 = static_cast<int>(ext[1]) | 0x20;
		const int c2 = static_cast<int>(ext[2]) | 0x20;
		const int c3 = static_cast<int>(ext[3]) | 0x20;
		if (ext[0] == '.' && c1 == 'm' && c2 == 'k' && c3 == 'v')
		{
			return WEBMIERE_FILETYPE_MKV;
		}
	}

	return WEBMIERE_FILETYPE_WEBM;
}


static bool
WebMiereCopyFilePath(
	prUTF16Char			*dst,
	size_t				dstCount,
	const prUTF16Char	*src)
{
	if (dst == NULL || dstCount == 0 || src == NULL)
	{
		return false;
	}

	size_t i = 0;
	for (; i + 1 < dstCount && src[i] != 0; i++)
	{
		dst[i] = src[i];
	}
	dst[i] = 0;

	return src[i] == 0;
}


namespace {
constexpr DWORD kWebMiereRuntimePathMax = 4096;

struct WebMiereRuntimeLoaderState
{
	bool					ok;
	bool					logged;
	DWORD					error;
	int						moduleCount;
	int						dirCount;
	HMODULE					modules[10];
	DLL_DIRECTORY_COOKIE	dirCookies[2];
	wchar_t					errorPath[kWebMiereRuntimePathMax];
	wchar_t					errorMessage[1024];
	wchar_t					cudartPath[kWebMiereRuntimePathMax];
};

static std::once_flag gRuntimeLoaderOnce;
static WebMiereRuntimeLoaderState gRuntimeLoaderState = {};

static bool
WebMiereCopyWide(wchar_t *dst, size_t dstCount, const wchar_t *src)
{
	if (dst == nullptr || dstCount == 0 || src == nullptr)
	{
		return false;
	}
	size_t i = 0;
	for (; i + 1 < dstCount && src[i] != 0; i++)
	{
		dst[i] = src[i];
	}
	dst[i] = 0;
	return src[i] == 0;
}


static bool
WebMiereAppendLeaf(const wchar_t *dir, const wchar_t *leaf, wchar_t *out, size_t outCount)
{
	if (!WebMiereCopyWide(out, outCount, dir))
	{
		return false;
	}
	size_t len = std::wcslen(out);
	if (len > 0 && out[len - 1] != L'\\' && out[len - 1] != L'/')
	{
		if (len + 1 >= outCount)
		{
			return false;
		}
		out[len++] = L'\\';
		out[len] = 0;
	}
	for (size_t i = 0; leaf[i] != 0; i++)
	{
		if (len + 1 >= outCount)
		{
			return false;
		}
		out[len++] = leaf[i];
		out[len] = 0;
	}
	return true;
}


static void
WebMiereFormatWin32Error(DWORD error, wchar_t *out, size_t outCount)
{
	if (out == nullptr || outCount == 0)
	{
		return;
	}
	out[0] = 0;
	const DWORD written = FormatMessageW(
		FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
		nullptr,
		error,
		0,
		out,
		static_cast<DWORD>(outCount),
		nullptr);
	if (written == 0)
	{
		std::swprintf(out, outCount, L"Win32 error %lu", static_cast<unsigned long>(error));
	}
}


static bool
WebMiereSetRuntimeFailure(WebMiereRuntimeLoaderState *state, const wchar_t *path, DWORD error)
{
	if (state == nullptr)
	{
		return false;
	}
	state->ok = false;
	state->error = error;
	WebMiereCopyWide(state->errorPath, kWebMiereRuntimePathMax, path ? path : L"");
	WebMiereFormatWin32Error(error, state->errorMessage, sizeof(state->errorMessage) / sizeof(state->errorMessage[0]));
	return false;
}


static void
WebMiereLogRuntimeFailureOnce(WebMiereRuntimeLoaderState *state)
{
	if (state == nullptr || state->logged)
	{
		return;
	}
	state->logged = true;

	wchar_t msg[8192] = {};
	std::swprintf(
		msg,
		sizeof(msg) / sizeof(msg[0]),
		L"WebMiere RuntimeLoader failed. path=\"%ls\" GetLastError=%lu message=\"%ls\"\r\n",
		state->errorPath,
		static_cast<unsigned long>(state->error),
		state->errorMessage);
	OutputDebugStringW(msg);
}


static bool
WebMiereGetPluginDirectory(wchar_t *pluginDir, size_t pluginDirCount, WebMiereRuntimeLoaderState *state)
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&WebMiereGetPluginDirectory),
			&module))
	{
		return WebMiereSetRuntimeFailure(state, L"WebMiere.prm", GetLastError());
	}

	const DWORD len = GetModuleFileNameW(module, pluginDir, static_cast<DWORD>(pluginDirCount));
	if (len == 0)
	{
		return WebMiereSetRuntimeFailure(state, L"WebMiere.prm", GetLastError());
	}
	if (len >= pluginDirCount)
	{
		return WebMiereSetRuntimeFailure(state, L"WebMiere.prm", ERROR_INSUFFICIENT_BUFFER);
	}

	for (DWORD i = len; i > 0; i--)
	{
		if (pluginDir[i - 1] == L'\\' || pluginDir[i - 1] == L'/')
		{
			pluginDir[i - 1] = 0;
			return true;
		}
	}
	return WebMiereSetRuntimeFailure(state, pluginDir, ERROR_INVALID_NAME);
}


static bool
MinaReady()
{
	static constexpr wchar_t kMinaLeaf[] =
		L"assets\\licenses\\ARTWORK_POLICY.md";
	static constexpr unsigned char kMinaMark[] = {
		0xE4, 0x80, 0x86, 0xE7, 0x50, 0x25, 0xC3, 0x8C,
		0xA6, 0x7E, 0x1C, 0x62, 0x34, 0x18, 0x5A, 0x8A,
		0x10, 0x98, 0x5B, 0xD2, 0xE6, 0x6A, 0x39, 0x59,
		0x0A, 0x5E, 0xE6, 0x0B, 0xD0, 0xB0, 0xB0, 0xD7
	};
	static_assert(sizeof(kMinaMark) == 32);

	WebMiereRuntimeLoaderState scratchState = {};
	wchar_t pluginDir[kWebMiereRuntimePathMax] = {};
	wchar_t candidatePath[kWebMiereRuntimePathMax] = {};

	if (!WebMiereGetPluginDirectory(pluginDir, kWebMiereRuntimePathMax, &scratchState) ||
		!WebMiereAppendLeaf(
			pluginDir,
			kMinaLeaf,
			candidatePath,
			kWebMiereRuntimePathMax))
	{
		return false;
	}

	HANDLE file = CreateFileW(
		candidatePath,
		GENERIC_READ,
		FILE_SHARE_READ,
		nullptr,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL |
			FILE_FLAG_OPEN_REPARSE_POINT |
			FILE_FLAG_SEQUENTIAL_SCAN,
		nullptr);
	if (file == INVALID_HANDLE_VALUE)
	{
		return false;
	}

	BCRYPT_ALG_HANDLE algorithm = nullptr;
	BCRYPT_HASH_HANDLE hash = nullptr;
	bool ready = false;

	do
	{
		BY_HANDLE_FILE_INFORMATION information = {};
		if (!GetFileInformationByHandle(file, &information) ||
			(information.dwFileAttributes &
				(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0)
		{
			break;
		}

		if (BCryptOpenAlgorithmProvider(
				&algorithm,
				BCRYPT_SHA256_ALGORITHM,
				nullptr,
				0) != 0 ||
			BCryptCreateHash(
				algorithm,
				&hash,
				nullptr,
				0,
				nullptr,
				0,
				0) != 0)
		{
			break;
		}

		// Keep a trailing CR for the next read so a split CRLF is one LF.
		unsigned char buffer[1024 + 1] = {};
		DWORD carry = 0;
		bool consumed = true;
		for (;;)
		{
			DWORD bytesRead = 0;
			if (!ReadFile(
					file,
					buffer + carry,
					static_cast<DWORD>(sizeof(buffer) - 1),
					&bytesRead,
					nullptr))
			{
				consumed = false;
				break;
			}
			const DWORD available = bytesRead + carry;
			carry = bytesRead > 0 && buffer[available - 1] == '\r' ? 1 : 0;
			const DWORD end = available - carry;
			DWORD written = 0;
			for (DWORD i = 0; i < end; ++i)
			{
				if (buffer[i] == '\r' && i + 1 < end && buffer[i + 1] == '\n')
					continue;
				buffer[written++] = buffer[i];
			}
			if (written && BCryptHashData(hash, buffer, written, 0) != 0)
			{
				consumed = false;
				break;
			}
			if (bytesRead == 0)
				break;
			if (carry)
				buffer[0] = '\r';
		}
		if (!consumed)
		{
			break;
		}

		unsigned char mark[sizeof(kMinaMark)] = {};
		if (BCryptFinishHash(
				hash,
				mark,
				static_cast<ULONG>(sizeof(mark)),
				0) != 0)
		{
			break;
		}

		ready = std::memcmp(mark, kMinaMark, sizeof(kMinaMark)) == 0;
	}
	while (false);

	if (hash != nullptr)
	{
		BCryptDestroyHash(hash);
	}
	if (algorithm != nullptr)
	{
		BCryptCloseAlgorithmProvider(algorithm, 0);
	}
	CloseHandle(file);
	return ready;
}


static bool
MinaReadyOnce()
{
	static std::once_flag once;
	static bool ready = false;

	std::call_once(once, []()
	{
		ready = MinaReady();
	});

	return ready;
}


static bool
WebMiereAddRuntimeDirectory(WebMiereRuntimeLoaderState *state, const wchar_t *path)
{
	DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(path);
	if (cookie == nullptr)
	{
		return WebMiereSetRuntimeFailure(state, path, GetLastError());
	}
	if (state->dirCount < static_cast<int>(sizeof(state->dirCookies) / sizeof(state->dirCookies[0])))
	{
		state->dirCookies[state->dirCount++] = cookie;
	}
	return true;
}


static bool
WebMiereLoadRuntimeDll(WebMiereRuntimeLoaderState *state, const wchar_t *path)
{
	const DWORD flags =
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
		LOAD_LIBRARY_SEARCH_USER_DIRS |
		LOAD_LIBRARY_SEARCH_SYSTEM32;
	HMODULE module = LoadLibraryExW(path, nullptr, flags);
	if (module == nullptr)
	{
		return WebMiereSetRuntimeFailure(state, path, GetLastError());
	}
	if (state->moduleCount < static_cast<int>(sizeof(state->modules) / sizeof(state->modules[0])))
	{
		state->modules[state->moduleCount++] = module;
	}
	return true;
}


static bool
WebMiereLoadCudart(WebMiereRuntimeLoaderState *state, const wchar_t *nvidiaDir)
{
	HMODULE existing = GetModuleHandleW(L"cudart64_12.dll");
	if (existing != nullptr)
	{
		const DWORD len = GetModuleFileNameW(
			existing,
			state->cudartPath,
			static_cast<DWORD>(sizeof(state->cudartPath) / sizeof(state->cudartPath[0])));
		if (len == 0)
		{
			return WebMiereSetRuntimeFailure(state, L"cudart64_12.dll", GetLastError());
		}
		if (len >= sizeof(state->cudartPath) / sizeof(state->cudartPath[0]))
		{
			return WebMiereSetRuntimeFailure(state, L"cudart64_12.dll", ERROR_INSUFFICIENT_BUFFER);
		}
		return WebMiereLoadRuntimeDll(state, state->cudartPath);
	}

	wchar_t path[kWebMiereRuntimePathMax] = {};
	if (!WebMiereAppendLeaf(nvidiaDir, L"cudart64_12.dll", path, kWebMiereRuntimePathMax))
	{
		return WebMiereSetRuntimeFailure(state, nvidiaDir, ERROR_INSUFFICIENT_BUFFER);
	}
	if (!WebMiereLoadRuntimeDll(state, path))
	{
		return false;
	}
	WebMiereCopyWide(state->cudartPath, sizeof(state->cudartPath) / sizeof(state->cudartPath[0]), path);
	return true;
}


static bool
WebMiereLoadRuntimeDllFromDir(WebMiereRuntimeLoaderState *state, const wchar_t *dir, const wchar_t *name)
{
	wchar_t path[kWebMiereRuntimePathMax] = {};
	if (!WebMiereAppendLeaf(dir, name, path, kWebMiereRuntimePathMax))
	{
		return WebMiereSetRuntimeFailure(state, dir, ERROR_INSUFFICIENT_BUFFER);
	}
	return WebMiereLoadRuntimeDll(state, path);
}


static void
WebMiereRuntimeLoaderOnce()
{
	WebMiereRuntimeLoaderState *state = &gRuntimeLoaderState;
	wchar_t pluginDir[kWebMiereRuntimePathMax] = {};
	wchar_t ffmpegDir[kWebMiereRuntimePathMax] = {};
	wchar_t nvidiaDir[kWebMiereRuntimePathMax] = {};

	if (!WebMiereGetPluginDirectory(pluginDir, kWebMiereRuntimePathMax, state) ||
		!WebMiereAppendLeaf(pluginDir, L"ffmpeg", ffmpegDir, kWebMiereRuntimePathMax) ||
		!WebMiereAppendLeaf(pluginDir, L"nvidia", nvidiaDir, kWebMiereRuntimePathMax))
	{
		if (state->error == 0)
		{
			WebMiereSetRuntimeFailure(state, pluginDir, ERROR_INSUFFICIENT_BUFFER);
		}
		return;
	}

	if (!WebMiereAddRuntimeDirectory(state, ffmpegDir) ||
		!WebMiereAddRuntimeDirectory(state, nvidiaDir) ||
		!WebMiereLoadCudart(state, nvidiaDir) ||
		!WebMiereLoadRuntimeDllFromDir(state, nvidiaDir, L"nppc64_12.dll") ||
		!WebMiereLoadRuntimeDllFromDir(state, nvidiaDir, L"nppicc64_12.dll") ||
		!WebMiereLoadRuntimeDllFromDir(state, nvidiaDir, L"nppidei64_12.dll") ||
		!WebMiereLoadRuntimeDllFromDir(state, nvidiaDir, L"nppig64_12.dll") ||
		!WebMiereLoadRuntimeDllFromDir(state, ffmpegDir, L"avutil-60.dll") ||
		!WebMiereLoadRuntimeDllFromDir(state, ffmpegDir, L"swresample-6.dll") ||
		!WebMiereLoadRuntimeDllFromDir(state, ffmpegDir, L"swscale-9.dll") ||
		!WebMiereLoadRuntimeDllFromDir(state, ffmpegDir, L"avcodec-62.dll") ||
		!WebMiereLoadRuntimeDllFromDir(state, ffmpegDir, L"avformat-62.dll"))
	{
		return;
	}

	state->ok = true;
}


static bool
WebMiereEnsureRuntimeLoaded()
{
	std::call_once(gRuntimeLoaderOnce, WebMiereRuntimeLoaderOnce);
	if (!gRuntimeLoaderState.ok)
	{
		WebMiereLogRuntimeFailureOnce(&gRuntimeLoaderState);
		return false;
	}
	return true;
}
}


PREMPLUGENTRY DllExport xImportEntry(
	csSDK_int32		selector,
	imStdParms		*stdParms,
	void			*param1,
	void			*param2)
{
	try
	{
		if (!MinaReadyOnce())
		{
			return imOtherErr;
		}

		if (!WebMiereEnsureRuntimeLoaded())
		{
			return imOtherErr;
		}

		prMALError result = imUnsupported;

		switch (selector)
		{
			case imInit:
				result = SDKInit(stdParms,
								  reinterpret_cast<imImportInfoRec*>(param1));
				break;

			case imShutdown:
				result = SDKShutdown(stdParms);
				break;

			case imGetIndFormat:
				result = SDKGetIndFormat(stdParms,
										 reinterpret_cast<csSDK_size_t>(param1),
										 reinterpret_cast<imIndFormatRec*>(param2));
				break;

			case imGetIndPixelFormat:
				result = SDKGetIndPixelFormat(stdParms,
											  reinterpret_cast<csSDK_size_t>(param1),
											  reinterpret_cast<imIndPixelFormatRec*>(param2));
				break;


			case imGetSupports8:
				result = malSupports8;
				break;

			case imGetInfo8:
				result = SDKGetInfo8(stdParms,
									 reinterpret_cast<imFileAccessRec8*>(param1),
									 reinterpret_cast<imFileInfoRec8*>(param2));
				break;

			case imOpenFile8:
				result = SDKOpenFile8(stdParms,
									  reinterpret_cast<imFileRef*>(param1),
									  reinterpret_cast<imFileOpenRec8*>(param2));
				break;

			case imQuietFile:
				result = SDKQuietFile(stdParms,
									  reinterpret_cast<imFileRef*>(param1),
									  param2);
				break;

			case imCloseFile:
				result = SDKCloseFile(stdParms,
									  reinterpret_cast<imFileRef*>(param1),
									  param2);
				break;

			case imGetPreferredFrameSize:
				result = SDKPreferredFrameSize(stdParms,
											   reinterpret_cast<imPreferredFrameSizeRec*>(param1));
				break;

			case imGetSourceVideo:
				result = SDKGetSourceVideo(stdParms,
										   reinterpret_cast<imFileRef>(param1),
										   reinterpret_cast<imSourceVideoRec*>(param2));
				break;

			case imImportAudio7:
				result = SDKImportAudio7(stdParms,
										 reinterpret_cast<imFileRef>(param1),
										 reinterpret_cast<imImportAudioRec7*>(param2));
				break;

			case imResetSequentialAudio:
				result = SDKResetSequentialAudio(stdParms,
												 reinterpret_cast<imFileRef>(param1),
												 reinterpret_cast<imImportAudioRec7*>(param2));
				break;

			case imGetSequentialAudio:
				result = SDKGetSequentialAudio(stdParms,
											   reinterpret_cast<imFileRef>(param1),
											   reinterpret_cast<imImportAudioRec7*>(param2));
				break;


			case imGetAudioChannelLayout:
				result = imUnsupported;
				break;

			default:
				result = imUnsupported;
				break;
		}

		return result;
	}
	catch (const std::bad_alloc&)
	{
		return imMemErr;
	}
	catch (...)
	{
		return imOtherErr;
	}
}


static bool
IsAfterEffectsHost(const imStdParms& stdParms)
{
	if (!stdParms.piSuites || !stdParms.piSuites->utilFuncs ||
		!stdParms.piSuites->utilFuncs->getSPBasicSuite)
	{
		return false;
	}

	SPBasicSuite *basic = stdParms.piSuites->utilFuncs->getSPBasicSuite();
	if (!basic || !basic->AcquireSuite || !basic->ReleaseSuite)
	{
		return false;
	}

	const void *suite = nullptr;
	if (basic->AcquireSuite(kPrSDKAppInfoSuite, kPrSDKAppInfoSuiteVersion, &suite) != kSPNoError)
	{
		return false;
	}

	const auto *appInfo = static_cast<const PrSDKAppInfoSuite*>(suite);
	csSDK_uint32 hostID = 0;
	const bool isAfterEffects = appInfo && appInfo->GetAppInfo &&
		appInfo->GetAppInfo(PrSDKAppInfoSuite::kAppInfo_AppFourCC, &hostID) == suiteError_NoError &&
		hostID == kAppAfterEffects;
	basic->ReleaseSuite(kPrSDKAppInfoSuite, kPrSDKAppInfoSuiteVersion);
	return isAfterEffects;
}


static prMALError
SDKInit(
	imStdParms		*stdParms,
	imImportInfoRec	*importInfo)
{
	if (stdParms == NULL || importInfo == NULL)
	{
		return imOtherErr;
	}

	importInfo->canSave				= kPrFalse;
	importInfo->canDelete			= kPrFalse;
	importInfo->canCalcSizes		= kPrFalse;
	importInfo->canTrim				= kPrFalse;
	importInfo->noFile				= kPrFalse;
	importInfo->hasSetup			= kPrFalse;
	importInfo->setupOnDblClk		= kPrFalse;
	importInfo->dontCache			= kPrFalse;
	importInfo->keepLoaded			= kPrFalse;

	// 224 = Feb. 24, the birthday of Mina, KawaiiEngine's official mascot.
	importInfo->priority			= IsAfterEffectsHost(*stdParms) ? 37 : 224;


	if (stdParms->imInterfaceVer >= IMPORTMOD_VERSION_6)
	{
		importInfo->avoidAudioConform = kPrTrue;
	}

	return imIsCacheable;
}


static prMALError
SDKShutdown(imStdParms *stdParms)
{
	(void)stdParms;
	return malNoError;
}


static prMALError
SDKGetIndFormat(
	imStdParms		*stdParms,
	csSDK_size_t	index,
	imIndFormatRec	*formatRec)
{
	(void)stdParms;
	if (formatRec == NULL)
	{
		return imOtherErr;
	}
	prMALError	result			= malNoError;
	char		formatName[256]	= WEBMIERE_FORMAT_NAME;
	char		shortName[32]	= WEBMIERE_FORMAT_SHORT;

	switch (index)
	{
		case 0:
		{
			char ext[256] = "webm";
			formatRec->filetype			= WEBMIERE_FILETYPE_WEBM;
			formatRec->canWriteTimecode	= kPrFalse;
			formatRec->flags			= xfCanImport | xfCanOpen | xfIsMovie;
			strcpy_s(formatRec->FormatName,        sizeof(formatRec->FormatName),        formatName);
			strcpy_s(formatRec->FormatShortName,   sizeof(formatRec->FormatShortName),   shortName);
			strcpy_s(formatRec->PlatformExtension, sizeof(formatRec->PlatformExtension), ext);
			break;
		}

		case 1:
		{
			char ext[256] = "mkv";
			formatRec->filetype			= WEBMIERE_FILETYPE_MKV;
			formatRec->canWriteTimecode	= kPrFalse;
			formatRec->flags			= xfCanImport | xfCanOpen | xfIsMovie;
			strcpy_s(formatRec->FormatName,        sizeof(formatRec->FormatName),        formatName);
			strcpy_s(formatRec->FormatShortName,   sizeof(formatRec->FormatShortName),   shortName);
			strcpy_s(formatRec->PlatformExtension, sizeof(formatRec->PlatformExtension), ext);
			break;
		}

		default:
			result = imBadFormatIndex;
			break;
	}


	return result;
}


static void
AppendPixelFormatUnique(PrPixelFormat *list, csSDK_size_t *count, PrPixelFormat fmt)
{
	for (csSDK_size_t i = 0; i < *count; i++)
	{
		if (list[i] == fmt)
		{
			return;
		}
	}
	list[(*count)++] = fmt;
}


static prMALError
SDKGetIndPixelFormat(
	imStdParms			*stdParms,
	csSDK_size_t		index,
	imIndPixelFormatRec	*pixelFormatRec)
{
	if (pixelFormatRec == NULL)
	{
		return imOtherErr;
	}
	prMALError result = malNoError;


	WebMiereHandleLock handleLock(stdParms, reinterpret_cast<ImporterLocalRecH>(pixelFormatRec->privatedata));
	if (handleLock.IsValid() && handleLock.Get()->hasAlpha)
	{
		if (index != 0) return imBadFormatIndex;
		pixelFormatRec->outPixelFormat = PrPixelFormat_BGRA_4444_8u;
		return malNoError;
	}

	PrPixelFormat formats[3];
	csSDK_size_t  count = 0;


#if WEBMIERE_ENABLE_YUV420P_RETURN && WEBMIERE_PREFER_YUV420P_RETURN
	AppendPixelFormatUnique(formats, &count, PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709);
#endif


	AppendPixelFormatUnique(formats, &count, PrPixelFormat_BGRA_4444_8u);
#if WEBMIERE_ENABLE_YUV420P_RETURN
	AppendPixelFormatUnique(formats, &count, PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709);
#endif

	if (index < count)
	{
		pixelFormatRec->outPixelFormat = formats[index];
	}
	else
	{
		result = imBadFormatIndex;
	}

	return result;
}


static prMALError
SDKGetInfo8(
	imStdParms			*stdParms,
	imFileAccessRec8	*fileAccessInfo8,
	imFileInfoRec8		*fileInfo8)
{
	prMALError			result		= malNoError;
	ImporterLocalRecH	ldataH		= NULL;
	bool				freshAlloc	= false;

	if (!WebMiereHasRequiredSuites(stdParms) ||
		fileAccessInfo8 == NULL || fileInfo8 == NULL ||
		fileAccessInfo8->filepath == NULL)
	{
		return imOtherErr;
	}
	WebMiereResetFileInfoStreamFields(fileInfo8);

	MediaProbeInfo	probe;
	std::string		probeErr;
	if (!ProbeMedia(fileAccessInfo8->filepath, &probe, &probeErr))
	{
		return imBadFile;
	}
	const int premiereStreamCount = (probe.audioStreamCount > 0) ? probe.audioStreamCount : 1;
	if (fileInfo8->streamIdx < 0 || fileInfo8->streamIdx >= premiereStreamCount)
	{
		return imBadStreamIndex;
	}

	if (probe.videoCodec != WEBMIERE_CODEC_VP9 &&
		probe.videoCodec != WEBMIERE_CODEC_AV1)
	{
		return imBadFile;
	}

	if (stdParms->imInterfaceVer >= IMPORTMOD_VERSION_6)
	{
		fileInfo8->accessModes = kSeparateSequentialAudio;
	}
	else
	{
		fileInfo8->accessModes = kRandomAccessImport;
	}
	fileInfo8->hasDataRate						= kPrFalse;


	if (fileInfo8->privatedata)
	{
		ldataH = reinterpret_cast<ImporterLocalRecH>(fileInfo8->privatedata);
	}
	else
	{
		ldataH = reinterpret_cast<ImporterLocalRecH>(
			stdParms->piSuites->memFuncs->newHandle(sizeof(ImporterLocalRec)));
		if (ldataH == NULL)
		{
			return imMemErr;
		}
		fileInfo8->privatedata = reinterpret_cast<void*>(ldataH);
		freshAlloc = true;
	}

	WebMiereHandleLock handleLock(stdParms, ldataH);
	if (!handleLock.IsValid())
	{
		if (freshAlloc)
		{
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileInfo8->privatedata);
		}
		return imMemErr;
	}
	ImporterLocalRecP ldata = handleLock.Get();


	if (freshAlloc)
	{
		std::memset(ldata, 0, sizeof(ImporterLocalRec));
		ldata->fileRef = imInvalidHandleValue;
	}
	else if (ldata->premiereStreamIdx != fileInfo8->streamIdx)
	{
		return imBadFile;
	}
	if (!WebMiereEnsureLocks(ldata))
	{
		if (freshAlloc)
		{
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileInfo8->privatedata);
		}
		return imMemErr;
	}

	if (!WebMiereCopyFilePath(ldata->filePath,
						  sizeof(ldata->filePath) / sizeof(ldata->filePath[0]),
						  fileAccessInfo8->filepath))
	{
		if (freshAlloc)
		{
			WebMiereFreeLocks(ldataH);
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileInfo8->privatedata);
		}
		return imBadFile;
	}


	ldata->BasicSuite = stdParms->piSuites->utilFuncs->getSPBasicSuite();
	if (ldata->BasicSuite)
	{
		if (ldata->PPixCreatorSuite == NULL)
		{
			ldata->BasicSuite->AcquireSuite(
				kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion,
				(const void**)&ldata->PPixCreatorSuite);
		}
		if (ldata->PPixSuite == NULL)
		{
			ldata->BasicSuite->AcquireSuite(
				kPrSDKPPixSuite, kPrSDKPPixSuiteVersion,
				(const void**)&ldata->PPixSuite);
		}
		if (ldata->PPix2Suite == NULL)
		{
			ldata->BasicSuite->AcquireSuite(
				kPrSDKPPix2Suite, kPrSDKPPix2SuiteVersion,
				(const void**)&ldata->PPix2Suite);
		}
		if (ldata->TimeSuite == NULL)
		{
			ldata->BasicSuite->AcquireSuite(
				kPrSDKTimeSuite, kPrSDKTimeSuiteVersion,
				(const void**)&ldata->TimeSuite);
		}
	}
	else
	{
		if (freshAlloc)
		{
			WebMiereFreeLocks(ldataH);
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileInfo8->privatedata);
		}
		return imOtherErr;
	}


	if (ldata->PPixCreatorSuite == NULL ||
		ldata->PPixSuite == NULL ||
		ldata->TimeSuite == NULL)
	{
		WebMiereReleaseSuites(ldata);
		if (freshAlloc)
		{
			WebMiereFreeLocks(ldataH);
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileInfo8->privatedata);
		}
		return imOtherErr;
	}


	PrTime ticksPerSecond = 0;
	ldata->TimeSuite->GetTicksPerSecond(&ticksPerSecond);

	const int frScale  = probe.frameRateNum;
	const int frSample = probe.frameRateDen;
	PrTime frameRatePrTime = 0;
	if (!WebMiereComputeFrameRatePrTime(ticksPerSecond, frScale, frSample, &frameRatePrTime))
	{
		if (freshAlloc)
		{
			WebMiereReleaseSuites(ldata);
			WebMiereFreeLocks(ldataH);
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileInfo8->privatedata);
		}
		return imBadFile;
	}

	if (!WebMiereApplyProbeStreamMapping(ldata, probe, fileInfo8->streamIdx))
	{
		return imBadStreamIndex;
	}
	ldata->importerID = fileInfo8->vidInfo.importerID;
	ldata->frameRatePrTime = frameRatePrTime;


	if (ldata->hasVideo)
	{
		fileInfo8->hasVideo					= kPrTrue;
		fileInfo8->vidInfo.subType			= WEBMIERE_VIDEO_SUBTYPE_VP9;
		fileInfo8->vidInfo.imageWidth		= ldata->width;
		fileInfo8->vidInfo.imageHeight		= ldata->height;
		fileInfo8->vidInfo.depth			= 32;
		fileInfo8->vidInfo.fieldType		= prFieldsNone;
		fileInfo8->vidInfo.alphaType		= ldata->hasAlpha ? alphaStraight : alphaNone;
		fileInfo8->vidInfo.pixelAspectNum	= 1;
		fileInfo8->vidInfo.pixelAspectDen	= 1;
		fileInfo8->vidInfo.supportsAsyncIO			= kPrFalse;
		fileInfo8->vidInfo.supportsGetSourceVideo	= kPrTrue;
		fileInfo8->vidInfo.hasPulldown				= kPrFalse;
		fileInfo8->vidScale					= ldata->frameRateScale;
		fileInfo8->vidSampleSize			= ldata->frameRateSampleSize;

		const csSDK_int64 numFrames = ldata->numFrames;
		const csSDK_int64 sampleSize = static_cast<csSDK_int64>(fileInfo8->vidSampleSize);
		csSDK_int64 legacyVidDuration = 0;
		if (numFrames > 0 && sampleSize > 0)
		{
			const csSDK_int64 maxDuration = (std::numeric_limits<csSDK_int64>::max)();
			legacyVidDuration = (numFrames > maxDuration / sampleSize)
				? maxDuration
				: numFrames * sampleSize;
		}
		fileInfo8->vidDuration =
			(legacyVidDuration > static_cast<csSDK_int64>((std::numeric_limits<csSDK_int32>::max)()))
				? (std::numeric_limits<csSDK_int32>::max)()
				: static_cast<csSDK_int32>(legacyVidDuration);
		fileInfo8->vidDurationInFrames = ldata->numFrames;
	}


	if (ldata->hasAudio)
	{
		fileInfo8->hasAudio				= kPrTrue;
		fileInfo8->audInfo.numChannels	= ldata->numChannels;
		fileInfo8->audInfo.sampleRate	= ldata->audioSampleRate;
		fileInfo8->audInfo.sampleType	= kPrAudioSampleType_Compressed;
		fileInfo8->audDuration			= ldata->numSampleFrames;
	}

	if (premiereStreamCount > 1)
	{
		if (fileInfo8->streamIdx == 0)
		{
			fileInfo8->streamsAsComp = kPrFalse;
		}
		result = (fileInfo8->streamIdx == premiereStreamCount - 1)
			? imBadStreamIndex
			: imIterateStreams;
	}

	return result;
}


static prMALError
SDKOpenFile8(
	imStdParms		*stdParms,
	imFileRef		*fileRef,
	imFileOpenRec8	*fileOpenRec8)
{
	prMALError			result		= malNoError;
	ImporterLocalRecH	ldataH		= NULL;
	bool				freshAlloc	= false;

	if (!WebMiereHasMemoryFuncs(stdParms) ||
		fileRef == NULL || fileOpenRec8 == NULL ||
		fileOpenRec8->fileinfo.filepath == NULL)
	{
		return imOtherErr;
	}

	*fileRef = imInvalidHandleValue;
	fileOpenRec8->fileinfo.fileref = imInvalidHandleValue;
	fileOpenRec8->outExtraMemoryUsage = 0;

	if (fileOpenRec8->privatedata)
	{
		ldataH = reinterpret_cast<ImporterLocalRecH>(fileOpenRec8->privatedata);
	}
	else
	{
		ldataH = reinterpret_cast<ImporterLocalRecH>(
			stdParms->piSuites->memFuncs->newHandle(sizeof(ImporterLocalRec)));
		if (ldataH == NULL)
		{
			return imMemErr;
		}
		fileOpenRec8->privatedata = reinterpret_cast<void*>(ldataH);
		freshAlloc = true;
	}

	WebMiereHandleLock handleLock(stdParms, ldataH);
	if (!handleLock.IsValid())
	{
		if (freshAlloc)
		{
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
		}
		return imMemErr;
	}
	ImporterLocalRecP ldata = handleLock.Get();

	if (freshAlloc)
	{
		std::memset(ldata, 0, sizeof(ImporterLocalRec));
		ldata->fileRef = imInvalidHandleValue;
		ldata->premiereStreamIdx = fileOpenRec8->inStreamIdx;
		ldata->hasVideo = (fileOpenRec8->inStreamIdx == 0) ? kPrTrue : kPrFalse;
		ldata->ffmpegAudioStreamIndex = -1;
	}
	if (!WebMiereEnsureLocks(ldata))
	{
		if (freshAlloc)
		{
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
		}
		return imMemErr;
	}
	if (ldata->premiereStreamIdx != fileOpenRec8->inStreamIdx)
	{
		if (freshAlloc)
		{
			WebMiereFreeLocks(ldataH);
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
		}
		return imBadFile;
	}

	if (!freshAlloc)
	{
		ReleaseOpenResources(ldata, NULL);
	}


	if (!WebMiereCopyFilePath(ldata->filePath,
						  sizeof(ldata->filePath) / sizeof(ldata->filePath[0]),
						  fileOpenRec8->fileinfo.filepath))
	{
		if (freshAlloc)
		{
			WebMiereFreeLocks(ldataH);
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
		}
		*fileRef = imInvalidHandleValue;
		fileOpenRec8->fileinfo.fileref = imInvalidHandleValue;
		return imBadFile;
	}
	if (freshAlloc)
	{
		MediaProbeInfo probe;
		std::string probeErr;
		if (!ProbeMedia(fileOpenRec8->fileinfo.filepath, &probe, &probeErr) ||
			!WebMiereApplyProbeStreamMapping(ldata, probe, fileOpenRec8->inStreamIdx))
		{
			WebMiereFreeLocks(ldataH);
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
			return imBadFile;
		}
	}


	HANDLE h = CreateFileW(
		fileOpenRec8->fileinfo.filepath,
		GENERIC_READ,
		FILE_SHARE_READ,
		NULL,
		OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL,
		NULL);

	if (h == imInvalidHandleValue)
	{
		if (freshAlloc)
		{
			WebMiereFreeLocks(ldataH);
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
		}
		*fileRef = imInvalidHandleValue;
		return imBadFile;
	}

	ldata->fileRef					= h;
	*fileRef						= h;
	fileOpenRec8->fileinfo.fileref	= h;
	fileOpenRec8->fileinfo.filetype	= WebMiereFiletypeForPath(fileOpenRec8->fileinfo.filepath);
	fileOpenRec8->outExtraMemoryUsage = 0;


	VideoDecoder *openedDecoder = NULL;
	if (ldata->hasVideo)
	{
		openedDecoder = EnsureDecoder(ldataH);
	}
	if (ldata->hasVideo && openedDecoder == NULL)
	{
		ReleaseOpenResources(ldata, fileRef);
		ldata->fileRef					= imInvalidHandleValue;
		*fileRef						= imInvalidHandleValue;
		fileOpenRec8->fileinfo.fileref	= imInvalidHandleValue;
		if (freshAlloc)
		{
			WebMiereFreeLocks(ldataH);
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
		}
		return imBadFile;
	}
	if (openedDecoder != NULL) ldata->hasAlpha = openedDecoder->HasAlpha();
	if (ldata->hasVideo && !WebMiereIsValidVideoSize(ldata->width, ldata->height))
	{
		ldata->width  = openedDecoder->Width();
		ldata->height = openedDecoder->Height();
	}
	fileOpenRec8->outExtraMemoryUsage = 0;


	if (ldata->hasAudio && EnsureAudioDecoder(ldataH) == NULL)
	{
		ReleaseOpenResources(ldata, fileRef);
		ldata->fileRef					= imInvalidHandleValue;
		*fileRef						= imInvalidHandleValue;
		fileOpenRec8->fileinfo.fileref	= imInvalidHandleValue;
		if (freshAlloc)
		{
			WebMiereFreeLocks(ldataH);
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
		}
		return imBadFile;
	}

	const csSDK_int32 maxOpenFileCount =
		1 +
		(ldata->hasVideo ? 1 : 0) +
		(ldata->hasAudio ? 2 : 0);
	if (!WebMiereSetImporterStreamFileCount(
			stdParms,
			static_cast<csSDK_uint32>(fileOpenRec8->inImporterID),
			fileOpenRec8->inStreamIdx,
			maxOpenFileCount))
	{
		ReleaseOpenResources(ldata, fileRef);
		ldata->fileRef					= imInvalidHandleValue;
		*fileRef						= imInvalidHandleValue;
		fileOpenRec8->fileinfo.fileref	= imInvalidHandleValue;
		if (freshAlloc)
		{
			WebMiereFreeLocks(ldataH);
			handleLock.Unlock();
			WebMiereDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
		}
		return imOtherErr;
	}

	return result;
}


static prMALError
SDKQuietFile(
	imStdParms	*stdParms,
	imFileRef	*fileRef,
	void		*privateData)
{
	if (!WebMiereHasMemoryFuncs(stdParms))
	{
		return imOtherErr;
	}
	ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(privateData);

	WebMiereHandleLock handleLock(stdParms, ldataH);
	ReleaseOpenResources(handleLock.Get(), fileRef);

	return malNoError;
}


static prMALError
SDKCloseFile(
	imStdParms	*stdParms,
	imFileRef	*fileRef,
	void		*privateData)
{
	if (!WebMiereHasMemoryFuncs(stdParms))
	{
		return imOtherErr;
	}
	ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(privateData);

	WebMiereHandleLock handleLock(stdParms, ldataH);
	ImporterLocalRecP ldata = handleLock.Get();
	ReleaseOpenResources(ldata, fileRef);

	if (ldata)
	{
		WebMiereReleaseSuites(ldata);
		WebMiereFreeLocks(ldataH);

		handleLock.Unlock();
		stdParms->piSuites->memFuncs->disposeHandle(reinterpret_cast<char**>(ldataH));
	}

	return malNoError;
}


static prMALError
SDKPreferredFrameSize(
	imStdParms				*stdParms,
	imPreferredFrameSizeRec	*preferredFrameSizeRec)
{
	prMALError			result	= malNoError;

	if (!WebMiereHasMemoryFuncs(stdParms) || preferredFrameSizeRec == NULL)
	{
		return imOtherErr;
	}
	ImporterLocalRecH	ldataH	= reinterpret_cast<ImporterLocalRecH>(preferredFrameSizeRec->inPrivateData);

	WebMiereHandleLock handleLock(stdParms, ldataH);
	ImporterLocalRecP ldata = handleLock.Get();
	if (ldata == NULL)
	{
		return imOtherErr;
	}

	switch (preferredFrameSizeRec->inIndex)
	{
		case 0:
			preferredFrameSizeRec->outWidth  = ldata->width;
			preferredFrameSizeRec->outHeight = ldata->height;
			result = malNoError;
			break;

		default:
			result = imOtherErr;
			break;
	}

	return result;
}


static prMALError
SDKGetSourceVideo(
	imStdParms			*stdParms,
	imFileRef			fileRef,
	imSourceVideoRec	*sourceVideoRec)
{
	prMALError			result	= malNoError;
	if (!WebMiereHasMemoryFuncs(stdParms) || sourceVideoRec == NULL)
	{
		return imOtherErr;
	}
	ImporterLocalRecH	ldataH	= reinterpret_cast<ImporterLocalRecH>(sourceVideoRec->inPrivateData);

	WebMiereHandleLock handleLock(stdParms, ldataH);
	ImporterLocalRecP ldata = handleLock.Get();
	if (ldata == NULL)
	{
		return imOtherErr;
	}
	if (!ldata->hasVideo)
	{
		return imOtherErr;
	}
	if (!WebMiereEnsureLocks(ldata))
	{
		return imMemErr;
	}

	WebMiereScopedLock lock(WebMiereVideoLock(ldata));

	csSDK_int64 theFrame = 0;
	if (ldata->frameRatePrTime > 0)
	{
		theFrame = static_cast<csSDK_int64>(sourceVideoRec->inFrameTime / ldata->frameRatePrTime);
	}

	if (theFrame < 0)
	{
		theFrame = 0;
	}
	if (ldata->numFrames > 0 && theFrame > ldata->numFrames - 1)
	{
		theFrame = ldata->numFrames - 1;
	}
	const bool			ppixAvailable	= (ldata->PPixSuite != NULL);
	const bool			ppix2Available	= (ldata->PPix2Suite != NULL);
	VideoDecoder		*decoder		= EnsureDecoder(ldataH);


	WebMiereOutputFormat	outFmt;
	if (!SelectOutputFormat(sourceVideoRec->inFrameFormats, sourceVideoRec->inNumFrameFormats,
							ppixAvailable, ppix2Available && !ldata->hasAlpha, ldata->width, ldata->height, &outFmt))
	{
		return imOtherErr;
	}


#if WEBMIERE_ENABLE_YUV420P_RETURN
	if (outFmt.kind == WEBMIERE_OUTPUT_YUV420P8)
	{
		bool attemptedYuv = false;
		if (decoder != NULL && ppix2Available)
		{
			attemptedYuv = true;
			if (TryGetSourceVideoYUV420P(ldata, sourceVideoRec, outFmt, decoder, theFrame))
			{
				return malNoError;
			}
			if (decoder->HadReadError())
			{
				return imFileReadFailed;
			}
		}
		if (attemptedYuv)
		{
			DisposeOutputPPix(ldata, sourceVideoRec->outFrame);
		}
		if (!SelectBgraFallbackFormat(sourceVideoRec->inFrameFormats, sourceVideoRec->inNumFrameFormats,
									  ppixAvailable, ldata->width, ldata->height, &outFmt))
		{
			return imOtherErr;
		}
			}
#endif


	char		*frameBuffer = NULL;
	csSDK_int32	rowBytes	 = 0;
	result = CreateOutputPPix(ldata, sourceVideoRec->outFrame, outFmt, &frameBuffer, &rowBytes);
	if (result != malNoError)
	{
		return result;
	}

	try
	{
		bool decoded = false;
		if (decoder != NULL && IsBgraPixelFormat(outFmt.prFormat) && frameBuffer != NULL)
		{
			decoded = decoder->DecodeFrameToBGRA(
				theFrame,
				reinterpret_cast<uint8_t*>(frameBuffer),
				rowBytes,
				outFmt.width,
				outFmt.height);
		}

		if (!decoded)
		{
			const bool readFailed = (decoder != NULL && decoder->HadReadError());
			DisposeOutputPPix(ldata, sourceVideoRec->outFrame);
			if (readFailed) return imFileReadFailed;
			return decoder != nullptr && decoder->HadDecodeError() ? imDecompressionError : imOtherErr;
		}
	}
	catch (const std::bad_alloc&)
	{
		DisposeOutputPPix(ldata, sourceVideoRec->outFrame);
		return imMemErr;
	}
	catch (...)
	{
		DisposeOutputPPix(ldata, sourceVideoRec->outFrame);
		return imOtherErr;
	}

	return malNoError;
}


static prMALError
SDKReadAudioSamples(
	ImporterLocalRecH	ldataH,
	imImportAudioRec7	*audioRec7,
	AudioDecoder		*ad,
	PrAudioSample		*savedPosition)
{
	if (ldataH == NULL || *ldataH == NULL || audioRec7 == NULL || savedPosition == NULL)
	{
		return imOtherErr;
	}

	const csSDK_int32	numChannels	= (*ldataH)->numChannels;
	const int64_t		reqSize		= static_cast<int64_t>(audioRec7->size);
	const PrAudioSample	total		= (*ldataH)->numSampleFrames;

	if (numChannels < 0 || numChannels > 2)
	{
		return imOtherErr;
	}
	if (reqSize > static_cast<int64_t>((std::numeric_limits<size_t>::max)() / sizeof(float)))
	{
		return imMemErr;
	}

	if (audioRec7->buffer != NULL && reqSize > 0)
	{
		for (csSDK_int32 ch = 0; ch < numChannels; ch++)
		{
			if (audioRec7->buffer[ch] != NULL)
			{
				std::memset(audioRec7->buffer[ch], 0, static_cast<size_t>(reqSize) * sizeof(float));
			}
		}
	}


	if (reqSize <= 0 || numChannels <= 0 || (*ldataH)->audioSampleRate <= 0.0f)
	{
		return malNoError;
	}
	if (audioRec7->buffer == NULL)
	{
		return malNoError;
	}


	PrAudioSample startSample;
	if (audioRec7->position < 0)
	{
		startSample = *savedPosition;
	}
	else
	{
		startSample = audioRec7->position;
	}
	if (startSample < 0)
	{
		startSample = 0;
	}


	if (ad == NULL || ad->Channels() != numChannels)
	{
		return imFileReadFailed;
	}

	int64_t decodeSize = reqSize;
	if (total > 0)
	{
		if (startSample >= total)
		{
			decodeSize = 0;
		}
		else if (decodeSize > static_cast<int64_t>(total - startSample))
		{
			decodeSize = static_cast<int64_t>(total - startSample);
		}
	}
	if (decodeSize > 0)
	{
		ad->ReadSamples(static_cast<int64_t>(startSample), decodeSize, audioRec7->buffer, numChannels);
		if (ad->HadReadError())
		{
			return imFileReadFailed;
		}
	}


	PrAudioSample next = WebMiereSaturatingAddAudioSamples(startSample, reqSize);
	if (total > 0 && next > total)
	{
		next = total;
	}
	*savedPosition = next;


	return malNoError;
}


static prMALError
SDKImportAudio7(
	imStdParms			*stdParms,
	imFileRef			fileRef,
	imImportAudioRec7	*audioRec7)
{
	(void)fileRef;

	if (!WebMiereHasMemoryFuncs(stdParms) || audioRec7 == NULL)
	{
		return imOtherErr;
	}

	ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(audioRec7->privateData);

	WebMiereHandleLock handleLock(stdParms, ldataH);
	ImporterLocalRecP ldata = handleLock.Get();
	if (ldata == NULL)
	{
		return imOtherErr;
	}
	if (!ldata->hasAudio)
	{
		return imOtherErr;
	}
	if (!WebMiereEnsureLocks(ldata))
	{
		return imMemErr;
	}

	WebMiereScopedLock lock(WebMiereAudioLock(ldata));
	AudioDecoder *ad = EnsureAudioDecoder(ldataH);
	return SDKReadAudioSamples(ldataH, audioRec7, ad, &ldata->savedAudioPosition);
}


static prMALError
SDKResetSequentialAudio(
	imStdParms			*stdParms,
	imFileRef			fileRef,
	imImportAudioRec7	*audioRec7)
{
	(void)fileRef;

	if (!WebMiereHasMemoryFuncs(stdParms) || audioRec7 == NULL)
	{
		return imOtherErr;
	}

	ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(audioRec7->privateData);

	WebMiereHandleLock handleLock(stdParms, ldataH);
	ImporterLocalRecP ldata = handleLock.Get();
	if (ldata == NULL)
	{
		return imOtherErr;
	}
	if (!ldata->hasAudio)
	{
		return imOtherErr;
	}
	if (!WebMiereEnsureLocks(ldata))
	{
		return imMemErr;
	}

	WebMiereScopedLock lock(WebMiereAudioLock(ldata));
	if (ldata->sequentialAudioDecoder != NULL)
	{
		delete ldata->sequentialAudioDecoder;
		ldata->sequentialAudioDecoder = NULL;
	}
	ldata->savedSequentialAudioPosition = (audioRec7->position >= 0) ? audioRec7->position : 0;

	return malNoError;
}


static prMALError
SDKGetSequentialAudio(
	imStdParms			*stdParms,
	imFileRef			fileRef,
	imImportAudioRec7	*audioRec7)
{
	(void)fileRef;

	if (!WebMiereHasMemoryFuncs(stdParms) || audioRec7 == NULL)
	{
		return imOtherErr;
	}

	ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(audioRec7->privateData);

	WebMiereHandleLock handleLock(stdParms, ldataH);
	ImporterLocalRecP ldata = handleLock.Get();
	if (ldata == NULL)
	{
		return imOtherErr;
	}
	if (!ldata->hasAudio)
	{
		return imOtherErr;
	}
	if (!WebMiereEnsureLocks(ldata))
	{
		return imMemErr;
	}

	WebMiereScopedLock lock(WebMiereAudioLock(ldata));
	AudioDecoder *ad = EnsureSequentialAudioDecoder(ldataH);
	return SDKReadAudioSamples(ldataH, audioRec7, ad, &ldata->savedSequentialAudioPosition);
}


static VideoDecoder *
EnsureDecoder(ImporterLocalRecH ldataH)
{
	if (ldataH == NULL || *ldataH == NULL)
	{
		return NULL;
	}
	if (!(*ldataH)->hasVideo)
	{
		return NULL;
	}

	WebMiereScopedLock lock(WebMiereVideoLock(ldataH));

	if ((*ldataH)->decoder == NULL)
	{
		try
		{
			std::unique_ptr<VideoDecoder> dec(new VideoDecoder());
			if (!dec->Open((*ldataH)->filePath))
			{
				return NULL;
			}
			(*ldataH)->decoder = dec.release();
		}
		catch (...)
		{
			return NULL;
		}
	}

	return (*ldataH)->decoder;
}


static AudioDecoder *
EnsureAudioDecoder(ImporterLocalRecH ldataH)
{
	if (ldataH == NULL || *ldataH == NULL)
	{
		return NULL;
	}
	if (!(*ldataH)->hasAudio || (*ldataH)->ffmpegAudioStreamIndex < 0)
	{
		return NULL;
	}

	WebMiereScopedLock lock(WebMiereAudioLock(ldataH));

	if ((*ldataH)->audioDecoder == NULL)
	{
		try
		{
			std::unique_ptr<AudioDecoder> ad(new AudioDecoder());
			if (!ad->Open((*ldataH)->filePath, (*ldataH)->ffmpegAudioStreamIndex))
			{
				return NULL;
			}
			(*ldataH)->audioDecoder = ad.release();
		}
		catch (...)
		{
			return NULL;
		}
	}

	return (*ldataH)->audioDecoder;
}


static AudioDecoder *
EnsureSequentialAudioDecoder(ImporterLocalRecH ldataH)
{
	if (ldataH == NULL || *ldataH == NULL)
	{
		return NULL;
	}
	if (!(*ldataH)->hasAudio || (*ldataH)->ffmpegAudioStreamIndex < 0)
	{
		return NULL;
	}

	WebMiereScopedLock lock(WebMiereAudioLock(ldataH));

	if ((*ldataH)->sequentialAudioDecoder == NULL)
	{
		try
		{
			std::unique_ptr<AudioDecoder> ad(new AudioDecoder());
			if (!ad->Open((*ldataH)->filePath, (*ldataH)->ffmpegAudioStreamIndex))
			{
				return NULL;
			}
			(*ldataH)->sequentialAudioDecoder = ad.release();
		}
		catch (...)
		{
			return NULL;
		}
	}

	return (*ldataH)->sequentialAudioDecoder;
}


static bool
IsBgraPixelFormat(PrPixelFormat fmt)
{
	return fmt == PrPixelFormat_BGRA_4444_8u;
}


static void
SetOutputFormat(
	WebMiereOutputFormat	*out,
	PrPixelFormat		prFormat,
	WebMiereOutputKind		kind,
	csSDK_int32			width,
	csSDK_int32			height)
{
	out->prFormat = prFormat;
	out->kind     = kind;
	out->width    = width;
	out->height   = height;
}


static void
ResolveFrameSize(
	const imFrameFormat	*format,
	csSDK_int32			nativeWidth,
	csSDK_int32			nativeHeight,
	csSDK_int32			*outWidth,
	csSDK_int32			*outHeight)
{
	csSDK_int32 w = (format != NULL) ? format->inFrameWidth : 0;
	csSDK_int32 h = (format != NULL) ? format->inFrameHeight : 0;
	if (w == 0)
	{
		w = nativeWidth;
	}
	if (h == 0)
	{
		h = nativeHeight;
	}
	*outWidth  = w;
	*outHeight = h;
}


static bool
IsValidYUV420PSize(csSDK_int32 width, csSDK_int32 height)
{
	return WebMiereIsValidYuv420Size(width, height);
}


static bool
SelectPreferredAnyFormat(
	bool				ppixAvailable,
	bool				ppix2Available,
	csSDK_int32			width,
	csSDK_int32			height,
	WebMiereOutputFormat	*out)
{
#if WEBMIERE_ENABLE_YUV420P_RETURN && WEBMIERE_PREFER_YUV420P_RETURN
	if (ppixAvailable && ppix2Available && IsValidYUV420PSize(width, height))
	{
		SetOutputFormat(out, PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709,
						WEBMIERE_OUTPUT_YUV420P8, width, height);
		return true;
	}
#endif

	if (ppixAvailable && WebMiereIsValidVideoSize(width, height))
	{
		SetOutputFormat(out, PrPixelFormat_BGRA_4444_8u,
						WEBMIERE_OUTPUT_BGRA8, width, height);
		return true;
	}

#if WEBMIERE_ENABLE_YUV420P_RETURN
	if (ppixAvailable && ppix2Available && IsValidYUV420PSize(width, height))
	{
		SetOutputFormat(out, PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709,
						WEBMIERE_OUTPUT_YUV420P8, width, height);
		return true;
	}
#endif

	return false;
}


static bool
SelectBgraFallbackFormat(
	const imFrameFormat	*formats,
	csSDK_int32			numFormats,
	bool				ppixAvailable,
	csSDK_int32			nativeWidth,
	csSDK_int32			nativeHeight,
	WebMiereOutputFormat	*out)
{
	if (!ppixAvailable)
	{
		return false;
	}

	if (formats == NULL || numFormats <= 0)
	{
		SetOutputFormat(out, PrPixelFormat_BGRA_4444_8u,
						WEBMIERE_OUTPUT_BGRA8, nativeWidth, nativeHeight);
		return WebMiereIsValidVideoSize(nativeWidth, nativeHeight);
	}

	for (csSDK_int32 i = 0; i < numFormats; i++)
	{
		const PrPixelFormat fmt = formats[i].inPixelFormat;
		if (fmt == 0 || fmt == PrPixelFormat_BGRA_4444_8u)
		{
			csSDK_int32 w = 0;
			csSDK_int32 h = 0;
			ResolveFrameSize(&formats[i], nativeWidth, nativeHeight, &w, &h);
			if (WebMiereIsValidVideoSize(w, h))
			{
				SetOutputFormat(out, PrPixelFormat_BGRA_4444_8u,
								WEBMIERE_OUTPUT_BGRA8, w, h);
				return true;
			}
		}
	}

	return false;
}


static bool
SelectOutputFormat(
	const imFrameFormat	*formats,
	csSDK_int32			numFormats,
	bool				ppixAvailable,
	bool				ppix2Available,
	csSDK_int32			nativeWidth,
	csSDK_int32			nativeHeight,
	WebMiereOutputFormat	*out)
{

	SetOutputFormat(out, PrPixelFormat_BGRA_4444_8u,
					WEBMIERE_OUTPUT_BGRA8, nativeWidth, nativeHeight);

	if (formats == NULL || numFormats <= 0)
	{
		return SelectPreferredAnyFormat(ppixAvailable, ppix2Available,
										nativeWidth, nativeHeight, out);
	}

	for (csSDK_int32 i = 0; i < numFormats; i++)
	{
		csSDK_int32 w = 0;
		csSDK_int32 h = 0;
		ResolveFrameSize(&formats[i], nativeWidth, nativeHeight, &w, &h);

		if (formats[i].inPixelFormat == 0)
		{
			if (SelectPreferredAnyFormat(ppixAvailable, ppix2Available, w, h, out))
			{
				return true;
			}
			continue;
		}

#if WEBMIERE_ENABLE_YUV420P_RETURN

		if (formats[i].inPixelFormat == PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709 &&
			ppixAvailable && ppix2Available && IsValidYUV420PSize(w, h))
		{
			SetOutputFormat(out, PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709,
							WEBMIERE_OUTPUT_YUV420P8, w, h);
			return true;
		}
#endif

		if (formats[i].inPixelFormat == PrPixelFormat_BGRA_4444_8u &&
			ppixAvailable && WebMiereIsValidVideoSize(w, h))
		{
			SetOutputFormat(out, PrPixelFormat_BGRA_4444_8u,
							WEBMIERE_OUTPUT_BGRA8, w, h);
			return true;
		}
	}

	return false;
}


static prMALError
CreateOutputPPix(
	ImporterLocalRecP		ldata,
	PPixHand				*outFrame,
	const WebMiereOutputFormat	&fmt,
	char					**outBuffer,
	csSDK_int32				*outRowBytes)
{
	if (outBuffer)   *outBuffer   = NULL;
	if (outRowBytes) *outRowBytes = 0;
	if (outFrame)    *outFrame    = NULL;

	if (ldata == NULL || ldata->PPixCreatorSuite == NULL || outFrame == NULL ||
		!WebMiereIsValidVideoSize(fmt.width, fmt.height))
	{
		return imOtherErr;
	}

	prRect theRect;
	prSetRect(&theRect, 0, 0, fmt.width, fmt.height);

	prMALError result = ldata->PPixCreatorSuite->CreatePPix(
		outFrame,
		PrPPixBufferAccess_ReadWrite,
		fmt.prFormat,
		&theRect);
	if (result != malNoError)
	{
		*outFrame = NULL;
		return result;
	}


	if (fmt.kind == WEBMIERE_OUTPUT_BGRA8 && outBuffer != NULL && outRowBytes != NULL)
	{
		if (ldata->PPixSuite == NULL)
		{
			DisposeOutputPPix(ldata, outFrame);
			return imOtherErr;
		}

		const prSuiteError gp = ldata->PPixSuite->GetPixels(*outFrame, PrPPixBufferAccess_ReadWrite, outBuffer);
		const prSuiteError gr = ldata->PPixSuite->GetRowBytes(*outFrame, outRowBytes);
		if (gp != suiteError_NoError || gr != suiteError_NoError ||
			*outBuffer == NULL || *outRowBytes == 0)
		{
			*outBuffer   = NULL;
			*outRowBytes = 0;
			DisposeOutputPPix(ldata, outFrame);
			return imOtherErr;
		}
	}
	return malNoError;
}


static void
DisposeOutputPPix(ImporterLocalRecP ldata, PPixHand *outFrame)
{
	if (outFrame != NULL && *outFrame != NULL && ldata->PPixSuite != NULL)
	{
		ldata->PPixSuite->Dispose(*outFrame);
		*outFrame = NULL;
	}
}

#if WEBMIERE_ENABLE_YUV420P_RETURN


static bool
TryGetSourceVideoYUV420P(
	ImporterLocalRecP		ldata,
	imSourceVideoRec		*sourceVideoRec,
	const WebMiereOutputFormat	&outFmt,
	VideoDecoder			*decoder,
	int64_t					theFrame)
{
	if (ldata->PPix2Suite == NULL)
	{
		return false;
	}

	prMALError cr = CreateOutputPPix(ldata, sourceVideoRec->outFrame, outFmt, NULL, NULL);
	if (cr != malNoError)
	{
				return false;
	}

	try
	{
		char			*yP = NULL, *uP = NULL, *vP = NULL;
		csSDK_uint32	yRB = 0, uRB = 0, vRB = 0;
		prSuiteError pe = ldata->PPix2Suite->GetYUV420PlanarBuffers(
			*sourceVideoRec->outFrame, PrPPixBufferAccess_ReadWrite,
			&yP, &yRB, &uP, &uRB, &vP, &vRB);
		if (pe != suiteError_NoError || yP == NULL || uP == NULL || vP == NULL)
		{
			DisposeOutputPPix(ldata, sourceVideoRec->outFrame);
			return false;
		}

		const int yRBs = static_cast<int>(static_cast<csSDK_int32>(yRB));
		const int uRBs = static_cast<int>(static_cast<csSDK_int32>(uRB));
		const int vRBs = static_cast<int>(static_cast<csSDK_int32>(vRB));
		const int w    = outFmt.width;
		const int h    = outFmt.height;


		bool ok = decoder->DecodeFrameToYUV420P(
			theFrame,
			reinterpret_cast<uint8_t*>(yP), yRBs,
			reinterpret_cast<uint8_t*>(uP), uRBs,
			reinterpret_cast<uint8_t*>(vP), vRBs,
			w, h);
		if (!ok)
		{
			DisposeOutputPPix(ldata, sourceVideoRec->outFrame);
			return false;
		}
		return true;
	}
	catch (...)
	{
		DisposeOutputPPix(ldata, sourceVideoRec->outFrame);
		return false;
	}
}
#endif
