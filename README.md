<p align="center">
  <img src="docs/images/webmiere-hero.png" alt="WebMiere - YouTube VP9, AV1 SDR, and Opus importer for Adobe Premiere Pro" width="100%">
</p>

# WebMiere

**Drop supported YouTube-style WebM/MKV media straight into Adobe Premiere Pro.**

WebMiere is a native Windows x64 importer for the `.webm` and `.mkv` files commonly produced from YouTube video and audio streams.

**No ProRes transcode. No proxy prep. No WAV extraction.**

Import it, drop it on the timeline, and start editing.

Repository: [KawaiiEngine/WebMiere](https://github.com/KawaiiEngine/WebMiere)

> [!IMPORTANT]
> **NVIDIA systems only.**
>
> WebMiere requires a supported NVIDIA GPU, a compatible NVIDIA graphics driver, and the CUDA/NPP runtime DLLs included with the release.
>
> GeForce RTX 20 / 30 / 40 / 50 series GPUs are recommended for VP9 hardware decoding.
>
> AV1 SDR import requires AV1 hardware decode support. Use RTX 30 series or newer.
>
> AMD-only, Intel-only, and macOS systems are not supported.

## What WebMiere Does

- Imports supported WebM/MKV files (YouTube-style VP9/Opus and AV1 SDR/Opus) directly into Premiere Pro
- Uses NVIDIA NVDEC, CUDA, and NPP for the accelerated video path
- Returns YUV420P directly when Premiere accepts it
- Supports CPU VP9 decoding as a diagnostic fallback
- Handles stereo 48 kHz Opus audio without requiring a separate WAV file
- Prioritizes responsive timeline editing over broad format compatibility

Typical workflow:

```text
YouTube-style VP9 or AV1 SDR WebM/MKV + Opus -> Premiere Pro -> edit
```

WebMiere is deliberately specialized. It is not intended to replace a general-purpose Matroska importer.

## Supported Media

| Area | Supported |
| --- | --- |
| Source type | Ordinary YouTube-style media streams |
| Containers | WebM / Matroska (`.webm`, `.mkv`) |
| Video codec | VP9 Profile 0; AV1 Main |
| Pixel format | 8-bit YUV 4:2:0 |
| Color | SDR, BT.709 matrix, limited range |
| Frame rate | Constant frame rate |
| Maximum dimensions | 8192 × 4320 |
| Audio codec | Opus, optional |
| Audio format | Stereo, 48 kHz |
| Premiere video output | YUV420P 8-bit, with BGRA fallback when requested |
| Premiere audio output | Planar 32-bit float |
| Operating system | Windows x64 |
| Host | Adobe Premiere Pro 26.x; built with the Premiere Pro 26.0 C++ SDK |
| GPU | NVIDIA GPU required; AV1 requires NVIDIA AV1 hardware decode support |

## YouTube and Frame Rate

WebMiere is designed around the constant-frame-rate VP9 and AV1 SDR delivery streams normally encountered when downloading YouTube-style media, including common nominal rates such as 24, 25, 30, 48, 50, 59.94, and 60 fps.

A camera mode or video title may contain the term "VFR" while the actual YouTube-style VP9 or AV1 SDR stream is still ordinary CFR media. WebMiere cares about the encoded stream, not the title or the camera recording mode name.

True variable-frame-rate files are not supported. They may still import and be reported as a nominal CFR stream, but frame selection, seeking, audio/video synchronization, editing behavior, and exported results are not guaranteed.

## Unsupported Media

- True variable-frame-rate video
- VP9 or AV1 10-bit or 12-bit video
- VP9 or AV1 4:2:2, 4:4:4, RGB, or alpha
- HDR, BT.2020, PQ, or HLG media, including AV1 HDR media
- Full-range video
- H.264, HEVC, ProRes, and other non-VP9/non-AV1 video codecs
- AAC, Vorbis, and other non-Opus audio
- Mono, surround, multichannel, or non-48 kHz Opus
- AMD-only or Intel-only systems
- macOS

## Installation

1. Install or update the NVIDIA graphics driver.
2. Close Adobe Premiere Pro.
3. Launch `WebMiere-Setup.exe`.
4. Choose `Install`.
5. When Windows asks for permission, allow the WebMiere Worker installer to make changes.
6. Start Premiere Pro.

Default installation path:

```text
C:\Program Files\Adobe\Common\Plug-ins\7.0\MediaCore\WebMiere
```

The installed WebMiere directory must contain `WebMiere.prm` and the matching `ffmpeg` and `nvidia` runtime subdirectories supplied with that release. Do not mix FFmpeg, CUDA, or NPP files from different WebMiere builds.

A typical installed directory contains:

```text
WebMiere.prm
assets\
  WebMiere-App.ico
  licenses\
    README.md
    WebMiere-MPL-2.0.txt
    FFmpeg-COPYING.LGPLv3.txt
    FFmpeg-COPYING.GPLv3.txt
    NVIDIA-CUDA-Toolkit-12.9-EULA.txt
    Microsoft-Visual-Cpp-Redistributable.txt
    THIRD_PARTY_NOTICES.md
ffmpeg\
  avcodec-62.dll
  avformat-62.dll
  avutil-60.dll
  swscale-9.dll
  swresample-6.dll
nvidia\
  cudart64_12.dll
  nppc64_12.dll
  nppicc64_12.dll
  nppidei64_12.dll
  nppig64_12.dll
```

Driver-provided NVIDIA components such as `nvcuda.dll` are not bundled with WebMiere.

Standard Inno Setup logs are written to the Windows temporary directory. These are installer logs, not WebMiere importer runtime logs.

### Uninstall

1. Close Premiere Pro.
2. Launch `WebMiere-Setup.exe` and choose `Uninstall`, or uninstall WebMiere from Windows Apps / Installed apps.
3. Restart Premiere Pro.

## Release Integrity and Source Availability

Official release assets should be used as a matched set.

An official release includes:

- A SHA-256 manifest for the downloadable WebMiere package
- The corresponding WebMiere source tag or source archive under MPL-2.0
- The exact FFmpeg runtime build used by the plugin
- A compact installed license payload under `assets\licenses`
- The matching WebMiere FFmpeg runtime and development release packages, including:
  - corresponding FFmpeg source archives
  - configure records
  - license records
  - runtime probe report
  - source-change diff
  - SHA-256 manifests
- Third-party license texts and notices applicable to the shipped binaries, including the NVIDIA CUDA Toolkit 12.9 EULA used for this build
- GitHub Artifact Attestations where supported by the public release workflow

The FFmpeg build recipe and provenance records are maintained in [KawaiiEngine/WebMiere-FFmpeg](https://github.com/KawaiiEngine/WebMiere-FFmpeg).

Review `THIRD_PARTY_NOTICES.md` and the licenses of the exact runtime DLLs before redistributing a package.

## Troubleshooting

### WebMiere Does Not Appear in Premiere

- Confirm that the system has a supported NVIDIA GPU.
- Install a compatible NVIDIA graphics driver.
- Confirm that `WebMiere.prm`, `ffmpeg`, and `nvidia` are present in the same `WebMiere` directory.
- Confirm that the FFmpeg DLLs are in `WebMiere\ffmpeg` and the CUDA/NPP DLLs are in `WebMiere\nvidia`.
- Install the Microsoft Visual C++ 2015-2022 Redistributable for x64.
- Fully restart Premiere Pro.
- Check the Windows temporary directory for the standard Inno Setup installer log if installation failed.
- Check Premiere's plugin loading log or use Process Monitor to identify a missing DLL if installation succeeded but the importer does not load.

WebMiere directly links against the NVIDIA driver API. FFmpeg, CUDA Runtime, and NPP DLLs are delay-loaded and preloaded from WebMiere's runtime subdirectories when Premiere initializes the importer. If a required runtime DLL is missing, WebMiere safely refuses to initialize.

### A File Does Not Import

A `.webm` or `.mkv` extension does not guarantee compatibility. Check that the file is:

- VP9 Profile 0 or AV1 Main SDR, rather than another video codec
- Constant frame rate rather than true variable frame rate
- 8-bit YUV 4:2:0
- Tagged as BT.709 matrix and limited range
- SDR rather than HDR
- For AV1 media, the system has an NVIDIA GPU with AV1 hardware decode support
- Opus stereo at 48 kHz, or has no audio stream
- Fully downloaded and not truncated

If another importer is installed, WebMiere is designed to take supported VP9/Opus and AV1 SDR/Opus media and pass unsupported media back to Premiere so another importer can handle it.

If a YouTube download that should be supported does not import, download it again before investigating further. Incomplete downloads and unusual remuxing tools can produce files outside the normal supported YouTube-style VP9/Opus or AV1 SDR/Opus shape.

## Known Behavior and Design Choices

- WebMiere favors responsive editing over strict recovery. Short audio reads and some recoverable audio decode gaps may be padded with silence.
- Audio timing is normalized relative to the stream start time. Small startup residues are handled at the beginning of the stream rather than allowed to become progressive audio drift.
- YouTube/DASH muxing may produce small differences between video and audio end times. When a stream-specific duration is unavailable, a container-duration fallback can result in a short final-frame hold. This has not shown a visible problem in normal tested YouTube material.
- True VFR files may import as nominal CFR. WebMiere does not attempt exact VFR reconstruction and may select, repeat, or skip nearby frames without displaying a warning.
- Premiere may occasionally request BGRA output for thumbnails, isolated frames, effects, or internal display paths. This is expected; normal playback generally uses YUV420P when available.
- WebMiere does not maintain a long-term importer-side frame cache. Premiere owns timeline caching.

# Technical Notes

## Architecture

WebMiere is a Premiere importer plugin. Its exported entry point is `xImportEntry`.

High-level flow:

1. Premiere loads the importer and reads its IMPT resource.
2. WebMiere validates the container and stream metadata.
3. FFmpeg demuxes WebM/Matroska and decodes VP9 or AV1 video and Opus audio.
4. On the NVIDIA path, CUDA/NPP converts decoded surfaces into a Premiere-compatible layout.
5. WebMiere returns PPix video frames and planar float audio to Premiere.

Unsupported metadata is rejected as early as possible. Because the importer runs inside the Premiere process, the implementation deliberately prefers a narrow and testable media contract over broad codec coverage.

## Importer Selection and Fallback

WebMiere registers with elevated importer priority so supported VP9/Opus and AV1 SDR/Opus files are offered to it before more general importers.

Unsupported media must return `imBadFile` from the relevant open or metadata path so Premiere can try another importer. This behavior has been tested with competing Matroska importers using AV1 HDR/10-bit media, AV1/AAC media, and VP9 Profile 2 HDR media.

## Media Contract

The current target is ordinary YouTube-style CFR VP9/Opus media and AV1 SDR/Opus WebM/MKV media.

Video timestamps are mapped to a fixed frame index. Exact VFR timestamp reproduction is outside the project scope. A true VFR stream may be accepted using its nominal frame rate, but its frame timing is unsupported.

Audio is decoded as stereo 48 kHz Opus and converted to planar 32-bit float. Stream-relative timestamps are used for sample positioning. A small first-frame residue, bounded by Opus initial padding and container time-base rounding, may be clamped to sample zero.

## Video Path

WebMiere prefers YUV420P 8-bit BT.709 output when Premiere accepts it. If Premiere requests BGRA, WebMiere uses the BGRA path.

For CUDA decoding, stream and event synchronization ensures that NPP does not read an NVDEC surface before producer work completes. Required GPU conversion and copies are completed before Premiere-owned memory is returned.

Primary NPP conversions include:

- NV12 to YUV420P
- NV12 to BGR/BGRA
- Channel order and orientation adjustment

The implementation validates dimensions, pixel format, planes, line sizes, hardware frame context, and PPix layout before copy or conversion.

## Audio Path

Audio uses the FFmpeg decoder and libswresample to produce planar float samples.

Separate decoder state is maintained for random-access reads and sequential/conforming reads. Output buffers are zero-initialized, so unread portions remain silent rather than containing uninitialized data.

## NVIDIA Loading Model

WebMiere directly links against the NVIDIA driver API (`nvcuda.dll`). FFmpeg, CUDA Runtime, and NPP DLLs are delay-loaded. At importer startup, WebMiere resolves its own plugin directory, registers the `ffmpeg` and `nvidia` runtime subdirectories, and preloads the required DLLs from absolute paths before any FFmpeg, CUDA, or NPP API is called.

The exact CUDA/NPP DLL set is release-specific and must match the ABI used to build `WebMiere.prm`.

Consequences:

- On a supported NVIDIA system with the required DLLs, Premiere can load the plugin.
- Without the NVIDIA driver, Windows cannot resolve `nvcuda.dll` and rejects the plugin.
- Missing FFmpeg or CUDA/NPP runtime DLLs prevent WebMiere from initializing, though this check occurs after the plugin entry point is reached.
- If `cudart64_12.dll` is already loaded in the Premiere process, WebMiere records and reuses that module path. Otherwise it loads the bundled `nvidia\cudart64_12.dll`.
- macOS cannot load the Windows PE `.prm` binary.

This avoids presenting unsupported hardware as though it might work.

## Memory and Resource Policy

WebMiere does not keep a long-lived importer-side frame cache. It decodes requested frames and writes to the output buffer supplied by Premiere.

Resource usage is bounded by the following policies:

- Maximum frame dimensions are limited.
- Decoder state is isolated per importer instance.
- Video and audio reads are protected by locks.
- FFmpeg and CUDA resources are released explicitly during seek, flush, reopen, and close operations.
- CUDA buffers are released inside the context that created them when the active context changes.
- Exceptions are contained at the plugin entry-point boundary.

## Developer Switches

These environment variables are intended for diagnostics and development, not normal use.

| Setting | Values | Purpose |
| --- | --- | --- |
| `WEBMIERE_FORCE_CPU_DECODE` | `1` | Disables CUDA/NVDEC video decoding for diagnosis. The NVIDIA driver and CUDA/NPP runtime DLLs are still required because WebMiere initializes its runtime loader before decoding starts. |
| `WEBMIERE_CUDA_SYNC_MODE` | `event`, `same_stream`, `producer`, `context` | Selects a CUDA surface-ordering strategy for comparison and diagnosis. |

## Build

Dependencies are not vendored in the repository.

Requirements:

- Windows x64
- Visual Studio 2022
- MSVC v143
- Windows SDK `10.0.26100.0`
- Adobe Premiere Pro 26.0 C++ SDK
- FFmpeg shared development files matching the runtime DLL ABI
- CUDA Toolkit 12.x with NPP
- Compatible NVIDIA graphics driver

The current FFmpeg baseline is produced by `KawaiiEngine/WebMiere-FFmpeg` from pinned source revisions. The end-user runtime contains exactly these five FFmpeg DLLs:

```text
avcodec-62.dll
avformat-62.dll
avutil-60.dll
swscale-9.dll
swresample-6.dll
```

The development package contains the matching headers and five MSVC import libraries. Do not substitute development files from another FFmpeg build.

Default dependency roots:

| Dependency | Default path |
| --- | --- |
| Premiere SDK | `..\Premiere Pro 26.0 C++ SDK` |
| FFmpeg | `..\ffmpeg` |
| CUDA Toolkit | `..\v12.9` |

Open:

```text
WebMiere.sln
```

Recommended configuration:

```text
Release | x64
```

## Repository Layout

```text
WebMiere/
  docs/
    images/
      webmiere-hero.png
  src/
    AudioDecoder.cpp
    AudioDecoder.h
    Demuxer.cpp
    Demuxer.h
    VideoDecoder.cpp
    VideoDecoder.h
    WebMiere.cpp
    WebMiere.h
    WebMiere.rc
    WebMiereColorPolicy.h
    WebMiereLimits.h
    WebMiereVersion.h
  THIRD_PARTY_NOTICES.md
  WebMiere.sln
  WebMiere.vcxproj
  README.md
  LICENSE
```

## License

Copyright (c) 2026 KawaiiEngine (Sashimiso).

WebMiere source code is licensed under the Mozilla Public License 2.0. See `LICENSE`.

Binary releases identify the corresponding source tag or source archive so recipients can obtain the MPL-covered source code.

Third-party components remain under their respective licenses and are summarized in `THIRD_PARTY_NOTICES.md`.

## Artwork and Characters

The WebMiere source code is licensed under MPL-2.0.

Mina, Miere, the WebMiere logo, icons, installer artwork, and
promotional images are separate brand and character assets and
are not licensed under the MPL-2.0.

Non-commercial fan art is welcome.

See [ARTWORK_POLICY.md](ARTWORK_POLICY.md) for details.
