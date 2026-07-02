# Third-Party Notices

This file summarizes factual third-party licensing and distribution information for WebMiere. It is not legal advice, and the applicable upstream license terms remain controlling.

## WebMiere

- Project license: MPL-2.0
- Publisher: KawaiiEngine
- Copyright: Copyright (c) 2026 KawaiiEngine (Sashimiso)
- Installed license text: `assets\licenses\WebMiere-MPL-2.0.txt`
- Source availability: binary releases identify the corresponding WebMiere source tag or source archive.

## Adobe Premiere Pro C++ SDK

- Adobe Premiere Pro 26.0 C++ SDK is a build-time dependency.
- Adobe SDK headers, samples, documentation, libraries, and binaries are not vendored or redistributed by this repository.
- Developers must obtain the SDK separately under Adobe's terms.

## FFmpeg

WebMiere dynamically links to the current KawaiiEngine shared FFmpeg build.

- FFmpeg version: 8.1.2
- FFmpeg commit: `38b88335f99e76ed89ff3c93f877fdefce736c13`
- Build identity: `n8.1.2-kawaiiengine-webmiere`
- License reported by the runtime: `LGPL version 3 or later`
- Build repository: `KawaiiEngine/WebMiere-FFmpeg`
- Linkage: dynamic/shared DLL linkage
- Local FFmpeg source changes: none
- Installed license texts: `assets\licenses\FFmpeg-COPYING.LGPLv3.txt` and `assets\licenses\FFmpeg-COPYING.GPLv3.txt`

Runtime contains exactly:

```text
avcodec-62.dll
avformat-62.dll
avutil-60.dll
swscale-9.dll
swresample-6.dll
```

The exact FFmpeg source archives, configure records, license records, runtime probe output, source-change diff, and SHA-256 manifests are included in the WebMiere FFmpeg runtime and development release packages rather than installed into the plugin directory.

## nv-codec-headers

- Version: 13.0.19.0
- Commit: `e844e5b26f46bb77479f063029595293aa8f812d`
- Used to build FFmpeg NVDEC support.
- The source archive is included in the WebMiere FFmpeg runtime and development release packages with the FFmpeg source material.
- The headers carry MIT-style copyright notices in the source files.

## NVIDIA CUDA and NPP

- WebMiere uses CUDA Runtime and NVIDIA Performance Primitives.
- Only runtime DLLs required by the final WebMiere binary may be redistributed.
- The complete CUDA Toolkit, compilers, headers, development tools, and import libraries must not be included in the end-user package.
- Installed license text: `assets\licenses\NVIDIA-CUDA-Toolkit-12.9-EULA.txt`, copied from the CUDA Toolkit 12.9 EULA used for this build.
- The final CUDA/NPP runtime DLL allowlist for this release is:

```text
cudart64_12.dll
nppc64_12.dll
nppicc64_12.dll
nppidei64_12.dll
nppig64_12.dll
```

- `nppc64_12.dll` is not a direct delay-load dependency of `WebMiere.prm`, but it is required by the shipped NPP runtime set and is preloaded with the other NVIDIA runtime DLLs.
- `nvcuda.dll` is supplied by the installed NVIDIA graphics driver and must not be bundled.

## Microsoft Runtime

- WebMiere uses the MSVC `/MD` runtime model.
- Microsoft Visual C++ 2015-2022 Redistributable x64 may be required.
- The installer may bundle `VC_redist.x64.exe` as a prerequisite payload and run it during installation.
- `VC_redist.x64.exe` is not installed into the final WebMiere plugin directory.
- Redistribution must follow Microsoft's applicable terms.
- Installed acquisition note: `assets\licenses\Microsoft-Visual-Cpp-Redistributable.txt`

## Installer

- The WebMiere setup executable is built with Inno Setup; see the Inno Setup project for its license terms.

## Installer and Brand Assets

- Mina and Miere are original WebMiere characters belonging to
  KawaiiEngine (Sashimiso).
- WebMiere logos, icons, installer artwork, promotional images,
  and character assets are not licensed under the MPL-2.0.
- Their use is governed by `ARTWORK_POLICY.md`.
- Non-commercial fan art is welcome under that policy.
- No third-party artwork or icons are included unless explicitly
  identified in this notice.
