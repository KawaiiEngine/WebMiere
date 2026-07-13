
// Copyright (c) 2026 KawaiiEngine (Sashimiso)
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.



#ifndef WEBMIERE_IMPORTER_H
#define WEBMIERE_IMPORTER_H


#include "PrSDKStructs.h"
#include "PrSDKImport.h"
#include "PrSDKPixelFormat.h"
#include "PrSDKPPixCreatorSuite.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKPPix2Suite.h"
#include "PrSDKTimeSuite.h"


#include <windows.h>


#define WEBMIERE_FILETYPE_WEBM   'VP9O'
#define WEBMIERE_FILETYPE_MKV    'VP9M'
#define WEBMIERE_VIDEO_SUBTYPE_VP9 'VP90'

#define WEBMIERE_FORMAT_NAME     "WebMiere (WebM/MKV)"
#define WEBMIERE_FORMAT_SHORT    "WebMiere"


#ifndef VP9O_ENABLE_YUV420P_RETURN
#define VP9O_ENABLE_YUV420P_RETURN  1
#endif
#ifndef VP9O_PREFER_YUV420P_RETURN
#define VP9O_PREFER_YUV420P_RETURN  1
#endif


class VideoDecoder;
class AudioDecoder;


enum Vp9oOutputKind
{
	VP9O_OUTPUT_BGRA8 = 0,
	VP9O_OUTPUT_YUV420P8
};


struct Vp9oOutputFormat
{
	PrPixelFormat	prFormat;
	Vp9oOutputKind	kind;
	csSDK_int32		width;
	csSDK_int32		height;
};


typedef struct
{
	imFileRef				fileRef;
	csSDK_int32				importerID;
	csSDK_int32				premiereStreamIdx;
	char					hasVideo;
	char					hasAudio;


	csSDK_int32				width;
	csSDK_int32				height;
	PrTime					frameRatePrTime;
	csSDK_int32				frameRateScale;
	csSDK_int32				frameRateSampleSize;
	csSDK_int64				numFrames;


	float					audioSampleRate;
	csSDK_int32				numChannels;
	PrAudioSample			numSampleFrames;
	csSDK_int32				ffmpegAudioStreamIndex;


	SPBasicSuite			*BasicSuite;
	PrSDKPPixCreatorSuite	*PPixCreatorSuite;
	PrSDKPPixSuite			*PPixSuite;
	PrSDKPPix2Suite			*PPix2Suite;
	PrSDKTimeSuite			*TimeSuite;


	prUTF16Char				filePath[2048];


	VideoDecoder			*decoder;
	AudioDecoder			*audioDecoder;
	AudioDecoder			*sequentialAudioDecoder;


	PrAudioSample			savedAudioPosition;
	PrAudioSample			savedSequentialAudioPosition;


	void					*locks;
} ImporterLocalRec, *ImporterLocalRecP, **ImporterLocalRecH;


extern "C" {
PREMPLUGENTRY DllExport xImportEntry(
	csSDK_int32		selector,
	imStdParms		*stdParms,
	void			*param1,
	void			*param2);
}

#endif
