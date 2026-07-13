
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
#include "PrSDKImporterFileManagerSuite.h"

#include <string>
#include <cstring>
#include <mutex>
#include <new>
#include <memory>
#include <cstdint>
#include <limits>
#include <cwchar>
#include <cstdio>


struct Vp9oInstanceLocks
{
	std::recursive_mutex video;
	std::recursive_mutex audio;
};


namespace {
class Vp9oScopedLock
{
public:
	explicit Vp9oScopedLock(std::recursive_mutex *m) : mMutex(m) { if (mMutex) mMutex->lock(); }
	~Vp9oScopedLock() { if (mMutex) mMutex->unlock(); }
	Vp9oScopedLock(const Vp9oScopedLock&) = delete;
	Vp9oScopedLock& operator=(const Vp9oScopedLock&) = delete;
private:
	std::recursive_mutex *mMutex;
};


class Vp9oHandleLock
{
public:
	Vp9oHandleLock(imStdParms *stdParms, ImporterLocalRecH h)
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

	~Vp9oHandleLock()
	{
		Unlock();
	}

	Vp9oHandleLock(const Vp9oHandleLock&) = delete;
	Vp9oHandleLock& operator=(const Vp9oHandleLock&) = delete;

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
Vp9oVideoLock(ImporterLocalRecH h)
{
	if (h != NULL && *h != NULL && (*h)->locks != NULL)
	{
		return &reinterpret_cast<Vp9oInstanceLocks*>((*h)->locks)->video;
	}
	return NULL;
}


static std::recursive_mutex *
Vp9oVideoLock(ImporterLocalRecP p)
{
	if (p != NULL && p->locks != NULL)
	{
		return &reinterpret_cast<Vp9oInstanceLocks*>(p->locks)->video;
	}
	return NULL;
}


static std::recursive_mutex *
Vp9oAudioLock(ImporterLocalRecH h)
{
	if (h != NULL && *h != NULL && (*h)->locks != NULL)
	{
		return &reinterpret_cast<Vp9oInstanceLocks*>((*h)->locks)->audio;
	}
	return NULL;
}


static std::recursive_mutex *
Vp9oAudioLock(ImporterLocalRecP p)
{
	if (p != NULL && p->locks != NULL)
	{
		return &reinterpret_cast<Vp9oInstanceLocks*>(p->locks)->audio;
	}
	return NULL;
}


static void
Vp9oFreeLocks(ImporterLocalRecH h)
{
	if (h != NULL && *h != NULL && (*h)->locks != NULL)
	{
		delete reinterpret_cast<Vp9oInstanceLocks*>((*h)->locks);
		(*h)->locks = NULL;
	}
}


static bool
Vp9oEnsureLocks(ImporterLocalRecP p)
{
	if (p == NULL)
	{
		return false;
	}
	if (p->locks == NULL)
	{
		p->locks = new (std::nothrow) Vp9oInstanceLocks();
	}
	return p->locks != NULL;
}


static bool
Vp9oHasMemoryFuncs(imStdParms *stdParms)
{
	return stdParms != NULL &&
		   stdParms->piSuites != NULL &&
		   stdParms->piSuites->memFuncs != NULL;
}


static bool
Vp9oHasRequiredSuites(imStdParms *stdParms)
{
	return Vp9oHasMemoryFuncs(stdParms) &&
		   stdParms->piSuites->utilFuncs != NULL;
}


static bool
Vp9oSetImporterStreamFileCount(
	imStdParms		*stdParms,
	csSDK_uint32	importerID,
	csSDK_int32		streamIndex,
	csSDK_int32		fileCount)
{
	if (!Vp9oHasRequiredSuites(stdParms) || fileCount <= 0)
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
Vp9oDisposePrivateDataHandle(imStdParms *stdParms, ImporterLocalRecH ldataH, void **privateData)
{
	if (privateData != NULL)
	{
		*privateData = NULL;
	}
	if (Vp9oHasMemoryFuncs(stdParms) && ldataH != NULL)
	{
		stdParms->piSuites->memFuncs->disposeHandle(reinterpret_cast<char**>(ldataH));
	}
}


static void
Vp9oReleaseSuites(ImporterLocalRecP ldata)
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
Vp9oComputeFrameRatePrTime(PrTime ticksPerSecond, int frameRateNum, int frameRateDen, PrTime *outPrTime)
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
Vp9oSaturatingAddAudioSamples(PrAudioSample a, int64_t b)
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
Vp9oResetFileInfoStreamFields(imFileInfoRec8 *fileInfo8)
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
Vp9oApplyProbeStreamMapping(
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
										   Vp9oOutputFormat *out);
static bool       SelectOutputFormat(const imFrameFormat *formats, csSDK_int32 numFormats,
									 bool ppixAvailable, bool ppix2Available,
									 csSDK_int32 nativeWidth, csSDK_int32 nativeHeight,
									 Vp9oOutputFormat *out);
static prMALError CreateOutputPPix(ImporterLocalRecP ldata, PPixHand *outFrame,
								   const Vp9oOutputFormat &fmt,
								   char **outBuffer, csSDK_int32 *outRowBytes);
static void       DisposeOutputPPix(ImporterLocalRecP ldata, PPixHand *outFrame);
#if VP9O_ENABLE_YUV420P_RETURN

static bool       TryGetSourceVideoYUV420P(ImporterLocalRecP ldata, imSourceVideoRec *sourceVideoRec,
										   const Vp9oOutputFormat &outFmt, VideoDecoder *decoder,
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
			Vp9oScopedLock lock(Vp9oVideoLock(ldata));
			if (ldata->decoder)
			{
				delete ldata->decoder;
				ldata->decoder = NULL;
			}
		}
		{
			Vp9oScopedLock lock(Vp9oAudioLock(ldata));
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
Vp9oFiletypeForPath(const prUTF16Char *path)
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
Vp9oCopyFilePath(
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
constexpr DWORD kVp9oRuntimePathMax = 4096;

struct Vp9oRuntimeLoaderState
{
	bool					ok;
	bool					logged;
	DWORD					error;
	int						moduleCount;
	int						dirCount;
	HMODULE					modules[10];
	DLL_DIRECTORY_COOKIE	dirCookies[2];
	wchar_t					errorPath[kVp9oRuntimePathMax];
	wchar_t					errorMessage[1024];
	wchar_t					cudartPath[kVp9oRuntimePathMax];
};

static std::once_flag gRuntimeLoaderOnce;
static Vp9oRuntimeLoaderState gRuntimeLoaderState = {};

static bool
Vp9oCopyWide(wchar_t *dst, size_t dstCount, const wchar_t *src)
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
Vp9oAppendLeaf(const wchar_t *dir, const wchar_t *leaf, wchar_t *out, size_t outCount)
{
	if (!Vp9oCopyWide(out, outCount, dir))
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
Vp9oFormatWin32Error(DWORD error, wchar_t *out, size_t outCount)
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
Vp9oSetRuntimeFailure(Vp9oRuntimeLoaderState *state, const wchar_t *path, DWORD error)
{
	if (state == nullptr)
	{
		return false;
	}
	state->ok = false;
	state->error = error;
	Vp9oCopyWide(state->errorPath, kVp9oRuntimePathMax, path ? path : L"");
	Vp9oFormatWin32Error(error, state->errorMessage, sizeof(state->errorMessage) / sizeof(state->errorMessage[0]));
	return false;
}


static void
Vp9oLogRuntimeFailureOnce(Vp9oRuntimeLoaderState *state)
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
Vp9oGetPluginDirectory(wchar_t *pluginDir, size_t pluginDirCount, Vp9oRuntimeLoaderState *state)
{
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(
			GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&Vp9oGetPluginDirectory),
			&module))
	{
		return Vp9oSetRuntimeFailure(state, L"WebMiere.prm", GetLastError());
	}

	const DWORD len = GetModuleFileNameW(module, pluginDir, static_cast<DWORD>(pluginDirCount));
	if (len == 0)
	{
		return Vp9oSetRuntimeFailure(state, L"WebMiere.prm", GetLastError());
	}
	if (len >= pluginDirCount)
	{
		return Vp9oSetRuntimeFailure(state, L"WebMiere.prm", ERROR_INSUFFICIENT_BUFFER);
	}

	for (DWORD i = len; i > 0; i--)
	{
		if (pluginDir[i - 1] == L'\\' || pluginDir[i - 1] == L'/')
		{
			pluginDir[i - 1] = 0;
			return true;
		}
	}
	return Vp9oSetRuntimeFailure(state, pluginDir, ERROR_INVALID_NAME);
}


static bool
Vp9oAddRuntimeDirectory(Vp9oRuntimeLoaderState *state, const wchar_t *path)
{
	DLL_DIRECTORY_COOKIE cookie = AddDllDirectory(path);
	if (cookie == nullptr)
	{
		return Vp9oSetRuntimeFailure(state, path, GetLastError());
	}
	if (state->dirCount < static_cast<int>(sizeof(state->dirCookies) / sizeof(state->dirCookies[0])))
	{
		state->dirCookies[state->dirCount++] = cookie;
	}
	return true;
}


static bool
Vp9oLoadRuntimeDll(Vp9oRuntimeLoaderState *state, const wchar_t *path)
{
	const DWORD flags =
		LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
		LOAD_LIBRARY_SEARCH_USER_DIRS |
		LOAD_LIBRARY_SEARCH_SYSTEM32;
	HMODULE module = LoadLibraryExW(path, nullptr, flags);
	if (module == nullptr)
	{
		return Vp9oSetRuntimeFailure(state, path, GetLastError());
	}
	if (state->moduleCount < static_cast<int>(sizeof(state->modules) / sizeof(state->modules[0])))
	{
		state->modules[state->moduleCount++] = module;
	}
	return true;
}


static bool
Vp9oLoadCudart(Vp9oRuntimeLoaderState *state, const wchar_t *nvidiaDir)
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
			return Vp9oSetRuntimeFailure(state, L"cudart64_12.dll", GetLastError());
		}
		if (len >= sizeof(state->cudartPath) / sizeof(state->cudartPath[0]))
		{
			return Vp9oSetRuntimeFailure(state, L"cudart64_12.dll", ERROR_INSUFFICIENT_BUFFER);
		}
		return Vp9oLoadRuntimeDll(state, state->cudartPath);
	}

	wchar_t path[kVp9oRuntimePathMax] = {};
	if (!Vp9oAppendLeaf(nvidiaDir, L"cudart64_12.dll", path, kVp9oRuntimePathMax))
	{
		return Vp9oSetRuntimeFailure(state, nvidiaDir, ERROR_INSUFFICIENT_BUFFER);
	}
	if (!Vp9oLoadRuntimeDll(state, path))
	{
		return false;
	}
	Vp9oCopyWide(state->cudartPath, sizeof(state->cudartPath) / sizeof(state->cudartPath[0]), path);
	return true;
}


static bool
Vp9oLoadRuntimeDllFromDir(Vp9oRuntimeLoaderState *state, const wchar_t *dir, const wchar_t *name)
{
	wchar_t path[kVp9oRuntimePathMax] = {};
	if (!Vp9oAppendLeaf(dir, name, path, kVp9oRuntimePathMax))
	{
		return Vp9oSetRuntimeFailure(state, dir, ERROR_INSUFFICIENT_BUFFER);
	}
	return Vp9oLoadRuntimeDll(state, path);
}


static void
Vp9oRuntimeLoaderOnce()
{
	Vp9oRuntimeLoaderState *state = &gRuntimeLoaderState;
	wchar_t pluginDir[kVp9oRuntimePathMax] = {};
	wchar_t ffmpegDir[kVp9oRuntimePathMax] = {};
	wchar_t nvidiaDir[kVp9oRuntimePathMax] = {};

	if (!Vp9oGetPluginDirectory(pluginDir, kVp9oRuntimePathMax, state) ||
		!Vp9oAppendLeaf(pluginDir, L"ffmpeg", ffmpegDir, kVp9oRuntimePathMax) ||
		!Vp9oAppendLeaf(pluginDir, L"nvidia", nvidiaDir, kVp9oRuntimePathMax))
	{
		if (state->error == 0)
		{
			Vp9oSetRuntimeFailure(state, pluginDir, ERROR_INSUFFICIENT_BUFFER);
		}
		return;
	}

	if (!Vp9oAddRuntimeDirectory(state, ffmpegDir) ||
		!Vp9oAddRuntimeDirectory(state, nvidiaDir) ||
		!Vp9oLoadCudart(state, nvidiaDir) ||
		!Vp9oLoadRuntimeDllFromDir(state, nvidiaDir, L"nppc64_12.dll") ||
		!Vp9oLoadRuntimeDllFromDir(state, nvidiaDir, L"nppicc64_12.dll") ||
		!Vp9oLoadRuntimeDllFromDir(state, nvidiaDir, L"nppidei64_12.dll") ||
		!Vp9oLoadRuntimeDllFromDir(state, nvidiaDir, L"nppig64_12.dll") ||
		!Vp9oLoadRuntimeDllFromDir(state, ffmpegDir, L"avutil-60.dll") ||
		!Vp9oLoadRuntimeDllFromDir(state, ffmpegDir, L"swresample-6.dll") ||
		!Vp9oLoadRuntimeDllFromDir(state, ffmpegDir, L"swscale-9.dll") ||
		!Vp9oLoadRuntimeDllFromDir(state, ffmpegDir, L"avcodec-62.dll") ||
		!Vp9oLoadRuntimeDllFromDir(state, ffmpegDir, L"avformat-62.dll"))
	{
		return;
	}

	state->ok = true;
}


static bool
Vp9oEnsureRuntimeLoaded()
{
	std::call_once(gRuntimeLoaderOnce, Vp9oRuntimeLoaderOnce);
	if (!gRuntimeLoaderState.ok)
	{
		Vp9oLogRuntimeFailureOnce(&gRuntimeLoaderState);
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
		if (!Vp9oEnsureRuntimeLoaded())
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
	importInfo->priority			= 224;


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
	(void)stdParms;
	if (pixelFormatRec == NULL)
	{
		return imOtherErr;
	}
	prMALError result = malNoError;


	PrPixelFormat formats[3];
	csSDK_size_t  count = 0;


#if VP9O_ENABLE_YUV420P_RETURN && VP9O_PREFER_YUV420P_RETURN
	AppendPixelFormatUnique(formats, &count, PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709);
#endif


	AppendPixelFormatUnique(formats, &count, PrPixelFormat_BGRA_4444_8u);
#if VP9O_ENABLE_YUV420P_RETURN
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

	if (!Vp9oHasRequiredSuites(stdParms) ||
		fileAccessInfo8 == NULL || fileInfo8 == NULL ||
		fileAccessInfo8->filepath == NULL)
	{
		return imOtherErr;
	}
	Vp9oResetFileInfoStreamFields(fileInfo8);

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

	Vp9oHandleLock handleLock(stdParms, ldataH);
	if (!handleLock.IsValid())
	{
		if (freshAlloc)
		{
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileInfo8->privatedata);
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
	if (!Vp9oEnsureLocks(ldata))
	{
		if (freshAlloc)
		{
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileInfo8->privatedata);
		}
		return imMemErr;
	}

	if (!Vp9oCopyFilePath(ldata->filePath,
						  sizeof(ldata->filePath) / sizeof(ldata->filePath[0]),
						  fileAccessInfo8->filepath))
	{
		if (freshAlloc)
		{
			Vp9oFreeLocks(ldataH);
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileInfo8->privatedata);
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
			Vp9oFreeLocks(ldataH);
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileInfo8->privatedata);
		}
		return imOtherErr;
	}


	if (ldata->PPixCreatorSuite == NULL ||
		ldata->PPixSuite == NULL ||
		ldata->TimeSuite == NULL)
	{
		Vp9oReleaseSuites(ldata);
		if (freshAlloc)
		{
			Vp9oFreeLocks(ldataH);
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileInfo8->privatedata);
		}
		return imOtherErr;
	}


	PrTime ticksPerSecond = 0;
	ldata->TimeSuite->GetTicksPerSecond(&ticksPerSecond);

	const int frScale  = probe.frameRateNum;
	const int frSample = probe.frameRateDen;
	PrTime frameRatePrTime = 0;
	if (!Vp9oComputeFrameRatePrTime(ticksPerSecond, frScale, frSample, &frameRatePrTime))
	{
		if (freshAlloc)
		{
			Vp9oReleaseSuites(ldata);
			Vp9oFreeLocks(ldataH);
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileInfo8->privatedata);
		}
		return imBadFile;
	}

	if (!Vp9oApplyProbeStreamMapping(ldata, probe, fileInfo8->streamIdx))
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
		fileInfo8->vidInfo.alphaType		= alphaNone;
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

	if (!Vp9oHasMemoryFuncs(stdParms) ||
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

	Vp9oHandleLock handleLock(stdParms, ldataH);
	if (!handleLock.IsValid())
	{
		if (freshAlloc)
		{
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
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
	if (!Vp9oEnsureLocks(ldata))
	{
		if (freshAlloc)
		{
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
		}
		return imMemErr;
	}
	if (ldata->premiereStreamIdx != fileOpenRec8->inStreamIdx)
	{
		if (freshAlloc)
		{
			Vp9oFreeLocks(ldataH);
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
		}
		return imBadFile;
	}

	if (!freshAlloc)
	{
		ReleaseOpenResources(ldata, NULL);
	}


	if (!Vp9oCopyFilePath(ldata->filePath,
						  sizeof(ldata->filePath) / sizeof(ldata->filePath[0]),
						  fileOpenRec8->fileinfo.filepath))
	{
		if (freshAlloc)
		{
			Vp9oFreeLocks(ldataH);
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
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
			!Vp9oApplyProbeStreamMapping(ldata, probe, fileOpenRec8->inStreamIdx))
		{
			Vp9oFreeLocks(ldataH);
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
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
			Vp9oFreeLocks(ldataH);
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
		}
		*fileRef = imInvalidHandleValue;
		return imBadFile;
	}

	ldata->fileRef					= h;
	*fileRef						= h;
	fileOpenRec8->fileinfo.fileref	= h;
	fileOpenRec8->fileinfo.filetype	= Vp9oFiletypeForPath(fileOpenRec8->fileinfo.filepath);
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
			Vp9oFreeLocks(ldataH);
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
		}
		return imBadFile;
	}
	if (ldata->hasVideo && !Vp9oIsValidVideoSize(ldata->width, ldata->height))
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
			Vp9oFreeLocks(ldataH);
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
		}
		return imBadFile;
	}

	const csSDK_int32 maxOpenFileCount =
		1 +
		(ldata->hasVideo ? 1 : 0) +
		(ldata->hasAudio ? 2 : 0);
	if (!Vp9oSetImporterStreamFileCount(
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
			Vp9oFreeLocks(ldataH);
			handleLock.Unlock();
			Vp9oDisposePrivateDataHandle(stdParms, ldataH, &fileOpenRec8->privatedata);
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
	if (!Vp9oHasMemoryFuncs(stdParms))
	{
		return imOtherErr;
	}
	ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(privateData);

	Vp9oHandleLock handleLock(stdParms, ldataH);
	ReleaseOpenResources(handleLock.Get(), fileRef);

	return malNoError;
}


static prMALError
SDKCloseFile(
	imStdParms	*stdParms,
	imFileRef	*fileRef,
	void		*privateData)
{
	if (!Vp9oHasMemoryFuncs(stdParms))
	{
		return imOtherErr;
	}
	ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(privateData);

	Vp9oHandleLock handleLock(stdParms, ldataH);
	ImporterLocalRecP ldata = handleLock.Get();
	ReleaseOpenResources(ldata, fileRef);

	if (ldata)
	{
		Vp9oReleaseSuites(ldata);
		Vp9oFreeLocks(ldataH);

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

	if (!Vp9oHasMemoryFuncs(stdParms) || preferredFrameSizeRec == NULL)
	{
		return imOtherErr;
	}
	ImporterLocalRecH	ldataH	= reinterpret_cast<ImporterLocalRecH>(preferredFrameSizeRec->inPrivateData);

	Vp9oHandleLock handleLock(stdParms, ldataH);
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
	if (!Vp9oHasMemoryFuncs(stdParms) || sourceVideoRec == NULL)
	{
		return imOtherErr;
	}
	ImporterLocalRecH	ldataH	= reinterpret_cast<ImporterLocalRecH>(sourceVideoRec->inPrivateData);

	Vp9oHandleLock handleLock(stdParms, ldataH);
	ImporterLocalRecP ldata = handleLock.Get();
	if (ldata == NULL)
	{
		return imOtherErr;
	}
	if (!ldata->hasVideo)
	{
		return imOtherErr;
	}
	if (!Vp9oEnsureLocks(ldata))
	{
		return imMemErr;
	}

	Vp9oScopedLock lock(Vp9oVideoLock(ldata));

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


	Vp9oOutputFormat	outFmt;
	if (!SelectOutputFormat(sourceVideoRec->inFrameFormats, sourceVideoRec->inNumFrameFormats,
							ppixAvailable, ppix2Available, ldata->width, ldata->height, &outFmt))
	{
		return imOtherErr;
	}


#if VP9O_ENABLE_YUV420P_RETURN
	if (outFmt.kind == VP9O_OUTPUT_YUV420P8)
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
			return readFailed ? imFileReadFailed : imOtherErr;
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


	PrAudioSample next = Vp9oSaturatingAddAudioSamples(startSample, reqSize);
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

	if (!Vp9oHasMemoryFuncs(stdParms) || audioRec7 == NULL)
	{
		return imOtherErr;
	}

	ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(audioRec7->privateData);

	Vp9oHandleLock handleLock(stdParms, ldataH);
	ImporterLocalRecP ldata = handleLock.Get();
	if (ldata == NULL)
	{
		return imOtherErr;
	}
	if (!ldata->hasAudio)
	{
		return imOtherErr;
	}
	if (!Vp9oEnsureLocks(ldata))
	{
		return imMemErr;
	}

	Vp9oScopedLock lock(Vp9oAudioLock(ldata));
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

	if (!Vp9oHasMemoryFuncs(stdParms) || audioRec7 == NULL)
	{
		return imOtherErr;
	}

	ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(audioRec7->privateData);

	Vp9oHandleLock handleLock(stdParms, ldataH);
	ImporterLocalRecP ldata = handleLock.Get();
	if (ldata == NULL)
	{
		return imOtherErr;
	}
	if (!ldata->hasAudio)
	{
		return imOtherErr;
	}
	if (!Vp9oEnsureLocks(ldata))
	{
		return imMemErr;
	}

	Vp9oScopedLock lock(Vp9oAudioLock(ldata));
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

	if (!Vp9oHasMemoryFuncs(stdParms) || audioRec7 == NULL)
	{
		return imOtherErr;
	}

	ImporterLocalRecH ldataH = reinterpret_cast<ImporterLocalRecH>(audioRec7->privateData);

	Vp9oHandleLock handleLock(stdParms, ldataH);
	ImporterLocalRecP ldata = handleLock.Get();
	if (ldata == NULL)
	{
		return imOtherErr;
	}
	if (!ldata->hasAudio)
	{
		return imOtherErr;
	}
	if (!Vp9oEnsureLocks(ldata))
	{
		return imMemErr;
	}

	Vp9oScopedLock lock(Vp9oAudioLock(ldata));
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

	Vp9oScopedLock lock(Vp9oVideoLock(ldataH));

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

	Vp9oScopedLock lock(Vp9oAudioLock(ldataH));

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

	Vp9oScopedLock lock(Vp9oAudioLock(ldataH));

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
	Vp9oOutputFormat	*out,
	PrPixelFormat		prFormat,
	Vp9oOutputKind		kind,
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
	return Vp9oIsValidYuv420Size(width, height);
}


static bool
SelectPreferredAnyFormat(
	bool				ppixAvailable,
	bool				ppix2Available,
	csSDK_int32			width,
	csSDK_int32			height,
	Vp9oOutputFormat	*out)
{
#if VP9O_ENABLE_YUV420P_RETURN && VP9O_PREFER_YUV420P_RETURN
	if (ppixAvailable && ppix2Available && IsValidYUV420PSize(width, height))
	{
		SetOutputFormat(out, PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709,
						VP9O_OUTPUT_YUV420P8, width, height);
		return true;
	}
#endif

	if (ppixAvailable && Vp9oIsValidVideoSize(width, height))
	{
		SetOutputFormat(out, PrPixelFormat_BGRA_4444_8u,
						VP9O_OUTPUT_BGRA8, width, height);
		return true;
	}

#if VP9O_ENABLE_YUV420P_RETURN
	if (ppixAvailable && ppix2Available && IsValidYUV420PSize(width, height))
	{
		SetOutputFormat(out, PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709,
						VP9O_OUTPUT_YUV420P8, width, height);
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
	Vp9oOutputFormat	*out)
{
	if (!ppixAvailable)
	{
		return false;
	}

	if (formats == NULL || numFormats <= 0)
	{
		SetOutputFormat(out, PrPixelFormat_BGRA_4444_8u,
						VP9O_OUTPUT_BGRA8, nativeWidth, nativeHeight);
		return Vp9oIsValidVideoSize(nativeWidth, nativeHeight);
	}

	for (csSDK_int32 i = 0; i < numFormats; i++)
	{
		const PrPixelFormat fmt = formats[i].inPixelFormat;
		if (fmt == 0 || fmt == PrPixelFormat_BGRA_4444_8u)
		{
			csSDK_int32 w = 0;
			csSDK_int32 h = 0;
			ResolveFrameSize(&formats[i], nativeWidth, nativeHeight, &w, &h);
			if (Vp9oIsValidVideoSize(w, h))
			{
				SetOutputFormat(out, PrPixelFormat_BGRA_4444_8u,
								VP9O_OUTPUT_BGRA8, w, h);
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
	Vp9oOutputFormat	*out)
{

	SetOutputFormat(out, PrPixelFormat_BGRA_4444_8u,
					VP9O_OUTPUT_BGRA8, nativeWidth, nativeHeight);

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

#if VP9O_ENABLE_YUV420P_RETURN

		if (formats[i].inPixelFormat == PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709 &&
			ppixAvailable && ppix2Available && IsValidYUV420PSize(w, h))
		{
			SetOutputFormat(out, PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_PLANAR_8u_709,
							VP9O_OUTPUT_YUV420P8, w, h);
			return true;
		}
#endif

		if (formats[i].inPixelFormat == PrPixelFormat_BGRA_4444_8u &&
			ppixAvailable && Vp9oIsValidVideoSize(w, h))
		{
			SetOutputFormat(out, PrPixelFormat_BGRA_4444_8u,
							VP9O_OUTPUT_BGRA8, w, h);
			return true;
		}
	}

	return false;
}


static prMALError
CreateOutputPPix(
	ImporterLocalRecP		ldata,
	PPixHand				*outFrame,
	const Vp9oOutputFormat	&fmt,
	char					**outBuffer,
	csSDK_int32				*outRowBytes)
{
	if (outBuffer)   *outBuffer   = NULL;
	if (outRowBytes) *outRowBytes = 0;
	if (outFrame)    *outFrame    = NULL;

	if (ldata == NULL || ldata->PPixCreatorSuite == NULL || outFrame == NULL ||
		!Vp9oIsValidVideoSize(fmt.width, fmt.height))
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


	if (fmt.kind == VP9O_OUTPUT_BGRA8 && outBuffer != NULL && outRowBytes != NULL)
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

#if VP9O_ENABLE_YUV420P_RETURN


static bool
TryGetSourceVideoYUV420P(
	ImporterLocalRecP		ldata,
	imSourceVideoRec		*sourceVideoRec,
	const Vp9oOutputFormat	&outFmt,
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
