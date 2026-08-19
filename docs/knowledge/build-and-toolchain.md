# Build and toolchain

## Unreal Engine 5.5 compatibility build

UE 5.5 Win64 links libpng 1.5.2. Skia's PNG codec performs libpng's runtime
version check, so compiling Skia with 1.6 headers and resolving its symbols from
UE's 1.5 library makes valid PNG files fail to decode.

Use the dedicated preset when producing libraries for the UE 5.5 plugin:

```powershell
$ProjectFfmpegRoot = 'E:/Project/Init_Ue_Project_UE5_5/Plugins/FFmpeg_UE/Source/ThirdParty/x64-windows'
cmake --preset ue55-v143-libpng15 -DSKIAUI_FFMPEG_ROOT=$ProjectFfmpegRoot
cmake --build --preset ue55-v143-libpng15-release --parallel
$env:Path = "$ProjectFfmpegRoot/bin;$env:Path"
ctest --test-dir build/ue55-v143-libpng15 -C Release --output-on-failure
cmake --install build/ue55-v143-libpng15 --config Release
```

The preset intentionally has its own binary directory and target triplet. It
uses MSVC 14.38.33130 and the `cmake/vcpkg-overlays/ue55-ports/libpng` port,
which builds official libpng 1.5.2 source. Normal presets continue to use the
repository baseline's libpng version.

`SKIAUI_REQUIRED_LIBPNG_VERSION=1.5.2` is a release guard. Configuration must
fail if CMake resolves any other libpng headers. The overlay also declares zlib
through pkg-config so standalone tests link the same dependency graph used to
compile Skia.

When exporting to UE, copy the SkiaUI and Skia family archives but do not copy
`libpng15.lib`, `libpng16.lib`, or vcpkg's `zs.lib`. Both editor and packaged UE
targets must use public dependencies on UE's `UElibPNG` and `zlib`, keeping one
PNG/zlib implementation in the process. Verify the final Unreal link response
file contains UE's `libpng15_static.lib` and `zlibstatic.lib`, and does not
contain `libpng16.lib` or `zs.lib`.

For the FFmpeg-enabled UE build, configure `SKIAUI_FFMPEG_ROOT` with the
project's `Plugins/FFmpeg_UE/Source/ThirdParty/x64-windows` directory. This
builds `SkuiFfmpeg.lib` against the same FFmpeg headers and import libraries
that Unreal loads at runtime. Do not let this preset resolve its default vcpkg
FFmpeg package, because a different FFmpeg major version is not ABI-compatible.
