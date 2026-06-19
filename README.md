# SkiaRelayDesk

Minimal CMake/C++23 demo that recreates the provided RelayDesk mockup with Skia on a native Win32 window.

## Build

```powershell
$env:VCPKG_ROOT = "..\out\vcpkg"
cmake --preset ninja-vs18
cmake --build --preset ninja-vs18-release
```

The executable is generated under `build/ninja-vs18/SkiaRelayDesk.exe`.

The `ninja-vs18` preset pins the same MSVC toolset used by the existing Skia vcpkg cache on this machine. A VS 2022 preset is also kept for IDE use, but it may relink against an older C++ runtime if the local Skia cache was built with VS 18.
