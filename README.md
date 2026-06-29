# SkiaRelayDesk

Minimal CMake/C++23 demo that recreates the provided RelayDesk mockup with Skia on a native Win32 window.

## Build

```powershell
cmake --preset windows-vcpkg
cmake --build --preset windows-vcpkg-release
```

The executable is generated under `build/windows-vcpkg/Release/SkiaRelayDesk.exe`.

The default preset uses the VS 2026 generator and the vcpkg checkout under `../out/vcpkg`. A `ninja-vcpkg` preset is also available for command-line builds from a Visual Studio developer shell.
