#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "skui_runtime.h"

#include <windows.h>

#include <string>

namespace skui::win32 {

struct WindowOptions {
    std::wstring title = L"SkiaUiDesk";
    int logicalWidth = 1672;
    int logicalHeight = 941;
    COLORREF clearColor = RGB(7, 12, 18);
    std::string documentPath;
    RuntimeOptions runtime;
};

class Dx12WindowApp {
public:
    explicit Dx12WindowApp(WindowOptions options);
    ~Dx12WindowApp();

    Dx12WindowApp(const Dx12WindowApp&) = delete;
    Dx12WindowApp& operator=(const Dx12WindowApp&) = delete;

    int run(HINSTANCE instance, int showCmd);

private:
    class Impl;
    Impl* impl_ = nullptr;
};

}  // namespace skui::win32
