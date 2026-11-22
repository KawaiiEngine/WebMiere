# Changelog

## 1.3.0 — 2026-11-22

- Added VP9 alpha support for transparent WebM/MKV clips in Adobe Premiere Pro.
- Added GPU-accelerated alpha decoding and optimized transparency-aware resizing.

## 1.2.2 — 2026-07-29

- First release in the reorganized public repository.
- Reorganized the bundled FFmpeg licensing, checksum, build, provenance, and corresponding source materials.
- Added WebMiere Checker.

## 1.2.1 — 2026-07-17

- Dramatically improved random audio seeking in long OBS-style multi-track WebM/MKV recordings.
- Added video Cue-based seeking for nearby Opus packets without decoding video.
- Preserved the existing audio-stream seek path as a fallback.
- Maintained audio timing accuracy with no cumulative drift in long-duration validation.

## 1.2.0 — 2026-07-14

- Added OBS-style multi-track Matroska import with separate Premiere Pro stereo tracks.
- Added support for up to six independent Opus stereo audio streams in compatible WebM/MKV media.
- Preserved container audio-stream order and added whole-file rejection for unsupported multi-audio media.
- Improved audio EOF handling and importer resource reporting.

## 1.1.1 — 2026-07-11

- Restored the AV1 software decode fallback path using libdav1d.

## 1.1.0 — 2026-07-09

- Added AV1 SDR import support for compatible WebM/MKV media.
- Updated the bundled FFmpeg build with AV1 decoder, AV1 parser, and AV1 NVDEC support.
- Updated documentation and release artwork for VP9/AV1 media.

## 1.0.1 — 2026-07-08

- Fixed an issue where playback near the end of some media could incorrectly trigger CPU fallback from the NVIDIA/CUDA decode path.

## 1.0.0 — 2026-07-02

- Initial public release.