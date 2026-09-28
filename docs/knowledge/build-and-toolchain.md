# Build and toolchain

## Unreal Engine 5.5 compatibility build

UE 5.5 Win64 links libpng 1.5.2. Skia's PNG codec performs libpng's runtime
version check, so compiling Skia with 1.6 headers and resolving its symbols from
UE's 1.5 library makes valid PNG files fail to decode.

Use the dedicated preset when producing libraries for the UE 5.5 plugin:

```powershell
cmake --preset ue55-v143-libpng15 "-DSKIAUI_FFMPEG_ROOT:PATH="
cmake --build --preset ue55-v143-libpng15-release --parallel
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

For the FFmpeg-enabled UE build, keep `SKIAUI_FFMPEG_ROOT` empty. The preset's
`ffmpeg-video` feature builds and statically links its own FFmpeg and libvpx
packages. Export those archives and headers together with `SkuiFfmpeg.lib`;
do not point the build back at an existing plugin copy.

The UE 5.5 release packages use the local `ffmpeg` and `libvpx` overlay ports.
The FFmpeg overlay removes vcpkg's `/Z7` flag and configures
`--disable-debug`. The libvpx overlay emits
`DebugInformationFormat=None` for Release and passes
`VCPKG_PLATFORM_TOOLSET_VERSION` to MSBuild, so it neither embeds `/Zi`
records nor falls back to the newest installed v143 compiler. Verify the build
log uses MSVC `14.38.33130` and contains no `/Zi` or `/Z7`.

`dumpbin /headers` can still show a small `.debug$S` section in each archive
member. Without `/Zi` or `/Z7`, these are minimal removable COFF compiler
records rather than full line and type debug information.
