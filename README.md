# SkiaRelayDesk / SkUI

CMake/C++23 demos and a lightweight Skia UI runtime. The current SkUI layer parses a focused HTML/CSS subset, lays it out with Yoga, renders through Skia, and can be embedded into other native projects.

## Build

```powershell
cmake --preset windows-vcpkg
cmake --build --preset windows-vcpkg-release
```

The executables are generated under `build/windows-vcpkg/Release/`.

The default preset uses the VS 2026 generator and the vcpkg checkout under `../out/vcpkg`. A `ninja-vcpkg` preset is also available for command-line builds from a Visual Studio developer shell.

## Docs

- [SkUI CSS / DOM 支持说明](docs/skui_css_dom_support.md)
- [SkUI 项目集成指南](docs/skui_integration_guide.md)
