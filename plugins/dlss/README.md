# ryGT DLSS plugin

An optional plugin that lets ryGT scale the game picture with NVIDIA DLSS instead of AMD FSR 1.
NVIDIA's DLSS SDK can't be linked into the GPL renderer, so DLSS lives in this separate
MIT-licensed DLL. Since FEAT-GTS-269 the ryGT7 package ships it, built by CI, as `dlss\ryGT_dlss.dll`;
NVIDIA's runtime is not shipped (see Install).

## Licensing

Two licences apply to the shipped `ryGT_dlss.dll`:

- **This source (`ryGT_dlss.cpp`, `rygt_dlss_api.h`): MIT**, RyGT-Team's code ([LICENSE](LICENSE),
  shipped as `licenses\ryGT_dlss\LICENSE`).
- **NVIDIA's NGX loader (`nvsdk_ngx_d.lib`), linked in as object code:** NVIDIA's, under the
  NVIDIA RTX SDKs License (the SDK's `LICENSE.txt`, shipped as `licenses\NVIDIA-DLSS-SDK\LICENSE.txt`).
  It is not MIT or GPL. It may be distributed only as object code inside an application, not on
  its own, and not reverse engineered.

That licence forbids putting the SDK under an open-source licence that would require its source,
derivative works or free redistribution. So NVIDIA's code goes only into this plugin, never into
the GPL renderer or runtime; the renderer loads the plugin at run time through `rygt_dlss_api.h`.
Keep it that way: don't link `nvsdk_ngx*.lib` or include NVIDIA's headers anywhere else. NVIDIA's
runtime DLLs are not shipped either: players download them with the launcher. The licence also asks
for attribution, which the launcher's About page and the README's credits give.

## What it does

The renderer only has the finished 1920x1080 game picture, with no motion vectors or depth. The
plugin runs DLSS on it with zero motion vectors, a flat depth image, no jitter and its history reset
every frame. That makes it a single-frame upscaler, not the temporal DLSS games ship with. It works
on RTX cards, picks the DLSS quality mode that fits the window size (DLAA when the window is
1920x1080), and falls back to FSR when DLSS is unavailable.

## Build

You need clang-cl, Visual Studio's C++ libraries, NVIDIA's DLSS SDK
(<https://github.com/NVIDIA/DLSS>, the `include` and `lib\Windows_x86_64\x64` folders) and the
Vulkan headers (shadGT's `externals\vulkan-headers` by default).

```
set DLSS_SDK=C:\path\to\DLSS
build.cmd C:\path\to\output
```

CI builds it (`ci/rygt7/build-windows.ps1`) against the SDK files pinned by checksum in
`ci/rygt7/runtime-toolchain.json` (`dlss_sdk`: NVIDIA DLSS SDK v310.9.16, its headers and
`nvsdk_ngx_d.lib`), and the packager places it, alone, in the package's `dlss` folder.

## Install

The package's `dlss` folder holds `ryGT_dlss.dll` and nothing else. NVIDIA's runtime
(`nvngx_dlss.dll`, and for frame generation `nvngx_dlssg.dll` and Streamline's `sl.*.dll`) comes
from the launcher: **Download NVIDIA DLSS** on the Graphics page, or **Add from a folder...** for
your own copies. A plugin you built yourself goes in the same place, `dlss\ryGT_dlss.dll`.

`rygt_dlss_api.h` is the interface between the renderer and the plugin; shadGT's
`ryty_gpu/rygt_dlss_api.h` must stay identical.
