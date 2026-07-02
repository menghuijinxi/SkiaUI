#include "skui_win32_app.h"

#include "skui_win32_event_adapter.h"

#include "d3d_presenter.h"
#include "perf_trace.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <dbghelp.h>
#include <dwmapi.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace skui::win32 {
namespace {

constexpr float kDefaultDpi = 96.0f;
constexpr UINT kRequestRedrawMessage = WM_APP + 0x531;

float dpiScaleForDpi(UINT dpi) {
    return static_cast<float>(std::max<UINT>(dpi, 1)) / kDefaultDpi;
}

UINT systemDpi() {
    const UINT dpi = GetDpiForSystem();
    return dpi != 0 ? dpi : static_cast<UINT>(kDefaultDpi);
}

UINT windowDpi(HWND hwnd) {
    const UINT dpi = GetDpiForWindow(hwnd);
    return dpi != 0 ? dpi : systemDpi();
}

void adjustWindowRectForDpi(RECT& rect, UINT dpi) {
    if (!AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi)) {
        AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
    }
}

}  // namespace

class Dx12WindowApp::Impl {
public:
    explicit Impl(WindowOptions options)
        : options_(std::move(options)),
          runtime_(withWin32PlatformCallbacks(configureRuntimeCallbacks(options_.runtime))),
          eventAdapter_(runtime_),
          d3d_(options_.clearColor) {
        eventAdapter_.setRuntimeDirtyCallback([this] {
            markFrameDirty();
            if (hwnd_) {
                requestRepaint(hwnd_, false);
            }
        });
    }

    ~Impl() {
        if (backgroundBrush_) {
            DeleteObject(backgroundBrush_);
            backgroundBrush_ = nullptr;
        }
    }

    int run(HINSTANCE instance, int showCmd) {
        SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);
        dpi_ = systemDpi();
        backgroundBrush_ = CreateSolidBrush(options_.clearColor);

        if (options_.onRuntimeReady) {
            options_.onRuntimeReady(runtime_);
        }

        if (!options_.documentPath.empty()) {
            runtime_.loadDocument(options_.documentPath);
        }

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        wc.hInstance = instance;
        wc.lpfnWndProc = &Dx12WindowApp::Impl::WndProc;
        wc.lpszClassName = L"SkiaUiDeskWindow";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = backgroundBrush_;
        RegisterClassExW(&wc);

        RECT workArea{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
        const int workWidth = workArea.right - workArea.left;
        const int workHeight = workArea.bottom - workArea.top;
        const float dpiScale = effectiveWindowScale();
        const int desiredClientWidth = static_cast<int>(std::round(options_.logicalWidth * dpiScale));
        const int desiredClientHeight = static_cast<int>(std::round(options_.logicalHeight * dpiScale));
        RECT desired{0, 0, desiredClientWidth, desiredClientHeight};
        adjustWindowRectForDpi(desired, dpi_);
        const int frameWidth = desired.right - desired.left;
        const int frameHeight = desired.bottom - desired.top;
        const float scale = std::min(1.0f, std::min((workWidth - 80.0f) / frameWidth, (workHeight - 80.0f) / frameHeight));
        const int width = static_cast<int>(desiredClientWidth * std::max(0.72f, scale));
        const int height = static_cast<int>(desiredClientHeight * std::max(0.72f, scale));
        RECT initialRect{0, 0, width, height};
        adjustWindowRectForDpi(initialRect, dpi_);
        const int windowWidth = initialRect.right - initialRect.left;
        const int windowHeight = initialRect.bottom - initialRect.top;
        const int x = workArea.left + (workWidth - windowWidth) / 2;
        const int y = workArea.top + (workHeight - windowHeight) / 2;

        HWND hwnd = CreateWindowExW(0,
                                    wc.lpszClassName,
                                    options_.title.c_str(),
                                    WS_OVERLAPPEDWINDOW,
                                    x,
                                    y,
                                    windowWidth,
                                    windowHeight,
                                    nullptr,
                                    nullptr,
                                    instance,
                                    this);
        if (!hwnd) {
            return 1;
        }
        hwnd_ = hwnd;

        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        ShowWindow(hwnd, showCmd);
        UpdateWindow(hwnd);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        hwnd_ = nullptr;
        return static_cast<int>(msg.wParam);
    }

private:
    WindowOptions options_;
    HWND hwnd_ = nullptr;
    std::atomic_bool redrawMessagePending_{false};
    Runtime runtime_;
    Win32EventAdapter eventAdapter_;
    D3DPresenter d3d_;
    std::vector<uint32_t> pixels_;
    int cpuSurfaceWidth_ = 0;
    int cpuSurfaceHeight_ = 0;
    UINT dpi_ = static_cast<UINT>(kDefaultDpi);
    HBRUSH backgroundBrush_ = nullptr;
    bool paintActive_ = false;
    bool frameDirty_ = true;
    bool hasPresentedFrame_ = false;
    int presentedWidth_ = 0;
    int presentedHeight_ = 0;
    int runtimeLayoutWidth_ = 0;
    int runtimeLayoutHeight_ = 0;
    float runtimeLayoutDpiScale_ = 0.0f;
    static Impl* get(HWND hwnd) {
        return reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    float runtimeDpiScale() const {
        return options_.useSystemDpiScale ? dpiScaleForDpi(dpi_) : 1.0f;
    }

    float effectiveWindowScale() const {
        return std::max(0.1f, runtimeDpiScale() * std::max(0.1f, options_.runtime.scale));
    }

    RuntimeOptions configureRuntimeCallbacks(RuntimeOptions options) {
        const RequestRedrawCallback previous = std::move(options.requestRedraw);
        options.requestRedraw = [this, previous] {
            if (previous) {
                previous();
            }
            HWND hwnd = hwnd_;
            if (!hwnd) {
                return;
            }
            bool expected = false;
            if (redrawMessagePending_.compare_exchange_strong(expected, true)) {
                PostMessageW(hwnd, kRequestRedrawMessage, 0, 0);
            }
        };
        return options;
    }

    void markFrameDirty() {
        frameDirty_ = true;
    }

    void requestRepaint(HWND hwnd, bool immediate) {
        const UINT flags = RDW_INVALIDATE | RDW_NOCHILDREN |
                           (immediate ? (RDW_ERASE | RDW_UPDATENOW) : 0);
        RedrawWindow(hwnd, nullptr, nullptr, flags);
    }

    void eraseBackground(HWND hwnd, HDC hdc) {
        RECT client{};
        GetClientRect(hwnd, &client);
        if (backgroundBrush_) {
            FillRect(hdc, &client, backgroundBrush_);
            return;
        }
        SetDCBrushColor(hdc, options_.clearColor);
        FillRect(hdc, &client, reinterpret_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }

    void resizeRuntimeForRender(int width, int height) {
        const bool traceEnabled = perf::Trace::enabled();
        const auto traceStart = traceEnabled ? perf::Trace::now() : perf::Trace::Clock::time_point{};
        const float dpiScale = runtimeDpiScale();
        const bool layoutChanged = width != runtimeLayoutWidth_ ||
                                   height != runtimeLayoutHeight_ ||
                                   dpiScale != runtimeLayoutDpiScale_;
        runtime_.beginUpdate();
        runtime_.resize(width, height, dpiScale);
        if (layoutChanged) {
            runtimeLayoutWidth_ = width;
            runtimeLayoutHeight_ = height;
            runtimeLayoutDpiScale_ = dpiScale;
            if (options_.onRuntimeResize) {
                options_.onRuntimeResize(runtime_);
            }
        }
        runtime_.endUpdate();
        if (!layoutChanged) {
            if (traceEnabled) {
                perf::Trace::write("skui_app", "resize_runtime_noop", width, height, perf::Trace::elapsedMs(traceStart));
            }
            return;
        }
        if (traceEnabled) {
            perf::Trace::write("skui_app", "resize_runtime", width, height, perf::Trace::elapsedMs(traceStart));
        }
    }

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }

        Impl* app = get(hwnd);
        if (!app) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        if (const std::optional<LRESULT> result =
                app->eventAdapter_.handleMessage(hwnd, message, wParam, lParam)) {
            return *result;
        }

        switch (message) {
        case kRequestRedrawMessage:
            app->redrawMessagePending_.store(false);
            app->markFrameDirty();
            app->requestRepaint(hwnd, false);
            return 0;
        case WM_WINDOWPOSCHANGING: {
            auto* pos = reinterpret_cast<WINDOWPOS*>(lParam);
            if (pos && !(pos->flags & SWP_NOSIZE)) {
                pos->flags |= SWP_NOCOPYBITS;
            }
            break;
        }
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) {
                return 0;
            }
            app->markFrameDirty();
            app->requestRepaint(hwnd, false);
            return 0;
        case WM_EXITSIZEMOVE:
            if (app->frameDirty_) {
                app->requestRepaint(hwnd, true);
            }
            return 0;
        case WM_DPICHANGED: {
            app->dpi_ = HIWORD(wParam);
            app->markFrameDirty();
            if (app->options_.useSystemDpiScale) {
                const auto* suggested = reinterpret_cast<RECT*>(lParam);
                SetWindowPos(hwnd,
                             nullptr,
                             suggested->left,
                             suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                app->requestRepaint(hwnd, true);
            } else {
                app->requestRepaint(hwnd, false);
            }
            return 0;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_PAINT:
            app->paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            app->eraseBackground(hwnd, reinterpret_cast<HDC>(wParam));
            return 1;
        case WM_DESTROY:
            app->hwnd_ = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool renderD3D(HWND hwnd, int width, int height) {
        const bool traceEnabled = perf::Trace::enabled();
        const auto traceStart = traceEnabled ? perf::Trace::now() : perf::Trace::Clock::time_point{};
        const bool ok = d3d_.render(
            hwnd,
            width,
            height,
            [this](SkCanvas& canvas, int drawWidth, int drawHeight) {
                resizeRuntimeForRender(drawWidth, drawHeight);
                runtime_.render(canvas);
            },
            [this](uint32_t* pixels, int drawWidth, int drawHeight, size_t rowBytes) {
                return renderCpuSurface(pixels, drawWidth, drawHeight, rowBytes);
            });
        if (traceEnabled) {
            perf::Trace::write("skui_app", ok ? "render_d3d_ok" : "render_d3d_fail", width, height, perf::Trace::elapsedMs(traceStart));
        }
        return ok;
    }

    bool renderCpuSurface(uint32_t* pixels, int width, int height, size_t rowBytes) {
        width = std::max(1, width);
        height = std::max(1, height);
        if (!pixels || rowBytes < static_cast<size_t>(width) * sizeof(uint32_t)) {
            return false;
        }
        const SkImageInfo info = SkImageInfo::Make(width,
                                                   height,
                                                   kBGRA_8888_SkColorType,
                                                   kPremul_SkAlphaType);
        sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(info, pixels, rowBytes);
        if (!surface) {
            return false;
        }
        resizeRuntimeForRender(width, height);
        runtime_.render(*surface->getCanvas());
        return true;
    }

    bool renderCpuSurface(int width, int height) {
        width = std::max(1, width);
        height = std::max(1, height);
        if (width != cpuSurfaceWidth_ || height != cpuSurfaceHeight_ || pixels_.empty()) {
            cpuSurfaceWidth_ = width;
            cpuSurfaceHeight_ = height;
            pixels_.resize(static_cast<size_t>(cpuSurfaceWidth_) * static_cast<size_t>(cpuSurfaceHeight_));
        }
        return renderCpuSurface(pixels_.data(),
                                cpuSurfaceWidth_,
                                cpuSurfaceHeight_,
                                static_cast<size_t>(cpuSurfaceWidth_) * sizeof(uint32_t));
    }

    void renderCpuFallback(HDC hdc, const PAINTSTRUCT& ps, int width, int height) {
        if (!renderCpuSurface(width, height)) {
            return;
        }

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = cpuSurfaceWidth_;
        bmi.bmiHeader.biHeight = -cpuSurfaceHeight_;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        const int paintX = std::max<LONG>(0, ps.rcPaint.left);
        const int paintY = std::max<LONG>(0, ps.rcPaint.top);
        const int paintRight = std::min<LONG>(cpuSurfaceWidth_, ps.rcPaint.right);
        const int paintBottom = std::min<LONG>(cpuSurfaceHeight_, ps.rcPaint.bottom);
        const int paintWidth = paintRight - paintX;
        const int paintHeight = paintBottom - paintY;
        if (paintWidth > 0 && paintHeight > 0) {
            StretchDIBits(hdc,
                          paintX,
                          paintY,
                          paintWidth,
                          paintHeight,
                          paintX,
                          paintY,
                          paintWidth,
                          paintHeight,
                          pixels_.data(),
                          &bmi,
                          DIB_RGB_COLORS,
                          SRCCOPY);
        }
    }

    void paint(HWND hwnd) {
        if (paintActive_) {
            ValidateRect(hwnd, nullptr);
            return;
        }
        paintActive_ = true;

        PAINTSTRUCT ps{};
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT client{};
        GetClientRect(hwnd, &client);
        const int width = std::max<LONG>(1, client.right - client.left);
        const int height = std::max<LONG>(1, client.bottom - client.top);
        dpi_ = windowDpi(hwnd);

        if (!frameDirty_ && hasPresentedFrame_ && width == presentedWidth_ && height == presentedHeight_) {
            EndPaint(hwnd, &ps);
            paintActive_ = false;
            return;
        }

        const bool renderedD3D = renderD3D(hwnd, width, height);
        if (renderedD3D) {
            hasPresentedFrame_ = true;
            presentedWidth_ = width;
            presentedHeight_ = height;
            frameDirty_ = false;
        } else {
            renderCpuFallback(hdc, ps, width, height);
        }

        EndPaint(hwnd, &ps);
        paintActive_ = false;
    }
};

Dx12WindowApp::Dx12WindowApp(WindowOptions options) : impl_(new Impl(std::move(options))) {}

Dx12WindowApp::~Dx12WindowApp() {
    delete impl_;
}

int Dx12WindowApp::run(HINSTANCE instance, int showCmd) {
    return impl_->run(instance, showCmd);
}

}  // namespace skui::win32
