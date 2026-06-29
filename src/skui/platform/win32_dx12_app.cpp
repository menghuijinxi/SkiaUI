#include "skui_win32_app.h"

#include "d3d_presenter.h"
#include "perf_trace.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkColorType.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkSurface.h"

#include <dbghelp.h>
#include <dwmapi.h>
#include <imm.h>
#include <shellscalingapi.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <windowsx.h>
#include <vector>

namespace skui::win32 {
namespace {

constexpr float kDefaultDpi = 96.0f;

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

std::string utf8FromWChar(wchar_t ch) {
    if (ch == 0 || (ch >= 0xD800 && ch <= 0xDFFF)) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, &ch, 1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, &ch, 1, out.data(), bytes, nullptr, nullptr);
    return out;
}

std::string utf8FromWide(std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}

std::wstring wideFromUtf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (chars <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), chars);
    return out;
}

std::string readClipboardText() {
    if (!OpenClipboard(nullptr)) {
        return {};
    }
    std::string out;
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* text = static_cast<const wchar_t*>(GlobalLock(handle))) {
            out = utf8FromWide(text);
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return out;
}

void writeClipboardText(std::string_view text) {
    const std::wstring wide = wideFromUtf8(text);
    if (!OpenClipboard(nullptr)) {
        return;
    }
    EmptyClipboard();
    const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (handle) {
        if (void* memory = GlobalLock(handle)) {
            std::memcpy(memory, wide.c_str(), bytes);
            GlobalUnlock(handle);
            SetClipboardData(CF_UNICODETEXT, handle);
            handle = nullptr;
        }
    }
    if (handle) {
        GlobalFree(handle);
    }
    CloseClipboard();
}

RuntimeOptions withPlatformCallbacks(RuntimeOptions options) {
    if (!options.readClipboardText) {
        options.readClipboardText = readClipboardText;
    }
    if (!options.writeClipboardText) {
        options.writeClipboardText = writeClipboardText;
    }
    return options;
}

std::wstring imeCompositionString(HWND hwnd, DWORD index) {
    HIMC context = ImmGetContext(hwnd);
    if (!context) {
        return {};
    }
    const LONG bytes = ImmGetCompositionStringW(context, index, nullptr, 0);
    std::wstring out;
    if (bytes > 0) {
        out.resize(static_cast<size_t>(bytes) / sizeof(wchar_t));
        ImmGetCompositionStringW(context, index, out.data(), bytes);
    }
    ImmReleaseContext(hwnd, context);
    return out;
}

}  // namespace

class Dx12WindowApp::Impl {
public:
    explicit Impl(WindowOptions options)
        : options_(std::move(options)),
          runtime_(withPlatformCallbacks(options_.runtime)),
          d3d_(options_.clearColor) {}

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
        const float dpiScale = dpiScaleForDpi(dpi_);
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

        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));
        ShowWindow(hwnd, showCmd);
        UpdateWindow(hwnd);

        MSG msg{};
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        return static_cast<int>(msg.wParam);
    }

private:
    WindowOptions options_;
    Runtime runtime_;
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
    bool trackingMouseLeave_ = false;
    std::wstring suppressedImeChars_;

    static Impl* get(HWND hwnd) {
        return reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    void markFrameDirty() {
        frameDirty_ = true;
    }

    void sendMouseEvent(HWND hwnd, EventType type, LPARAM lParam, MouseButton button = MouseButton::None) {
        Event event;
        event.type = type;
        event.button = button;
        event.shiftKey = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        event.ctrlKey = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (type != EventType::MouseLeave) {
            event.x = static_cast<float>(GET_X_LPARAM(lParam));
            event.y = static_cast<float>(GET_Y_LPARAM(lParam));
        }
        runtime_.handleEvent(event);
        if (runtime_.dirty()) {
            markFrameDirty();
            requestRepaint(hwnd, false);
        }
    }

    void sendWheelEvent(HWND hwnd, WPARAM wParam, LPARAM lParam, bool horizontal) {
        POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &point);
        Event event;
        event.type = EventType::MouseWheel;
        event.x = static_cast<float>(point.x);
        event.y = static_cast<float>(point.y);
        event.wheelDelta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam));
        event.shiftKey = horizontal || (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        event.ctrlKey = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        runtime_.handleEvent(event);
        if (runtime_.dirty()) {
            markFrameDirty();
            requestRepaint(hwnd, false);
        }
    }

    bool sendKeyEvent(HWND hwnd, WPARAM key) {
        Event event;
        event.type = EventType::KeyDown;
        event.key = static_cast<unsigned>(key);
        event.shiftKey = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        event.ctrlKey = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool consumed = runtime_.handleEvent(event);
        if (runtime_.dirty()) {
            markFrameDirty();
            requestRepaint(hwnd, false);
        }
        return consumed;
    }

    bool sendImeEvent(HWND hwnd, EventType type, std::string text = {}) {
        Event event;
        event.type = type;
        event.text = std::move(text);
        const bool consumed = runtime_.handleEvent(event);
        if (runtime_.dirty()) {
            markFrameDirty();
            requestRepaint(hwnd, false);
        }
        return consumed;
    }

    bool sendTextInputEvent(HWND hwnd, std::string text) {
        if (text.empty()) {
            return false;
        }
        Event event;
        event.type = EventType::TextInput;
        event.text = std::move(text);
        const bool consumed = runtime_.handleEvent(event);
        if (runtime_.dirty()) {
            markFrameDirty();
            requestRepaint(hwnd, false);
        }
        return consumed;
    }

    void beginMouseLeaveTracking(HWND hwnd) {
        if (trackingMouseLeave_) {
            return;
        }
        TRACKMOUSEEVENT track{};
        track.cbSize = sizeof(track);
        track.dwFlags = TME_LEAVE;
        track.hwndTrack = hwnd;
        if (TrackMouseEvent(&track)) {
            trackingMouseLeave_ = true;
        }
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

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }

        Impl* app = get(hwnd);
        if (!app) {
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }

        switch (message) {
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
            app->requestRepaint(hwnd, true);
            return 0;
        case WM_EXITSIZEMOVE:
            if (app->frameDirty_) {
                app->requestRepaint(hwnd, true);
            }
            return 0;
        case WM_DPICHANGED: {
            app->dpi_ = HIWORD(wParam);
            app->markFrameDirty();
            const auto* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd,
                         nullptr,
                         suggested->left,
                         suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            app->requestRepaint(hwnd, true);
            return 0;
        }
        case WM_MOUSEMOVE:
            app->beginMouseLeaveTracking(hwnd);
            app->sendMouseEvent(hwnd, EventType::MouseMove, lParam);
            return 0;
        case WM_MOUSELEAVE:
            app->trackingMouseLeave_ = false;
            app->sendMouseEvent(hwnd, EventType::MouseLeave, lParam);
            return 0;
        case WM_LBUTTONDOWN:
            SetCapture(hwnd);
            app->sendMouseEvent(hwnd, EventType::MouseDown, lParam, MouseButton::Left);
            return 0;
        case WM_LBUTTONDBLCLK:
            app->sendMouseEvent(hwnd, EventType::MouseDoubleClick, lParam, MouseButton::Left);
            return 0;
        case WM_LBUTTONUP:
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            app->sendMouseEvent(hwnd, EventType::MouseUp, lParam, MouseButton::Left);
            return 0;
        case WM_MBUTTONDOWN:
            SetCapture(hwnd);
            app->sendMouseEvent(hwnd, EventType::MouseDown, lParam, MouseButton::Middle);
            return 0;
        case WM_MBUTTONUP:
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            app->sendMouseEvent(hwnd, EventType::MouseUp, lParam, MouseButton::Middle);
            return 0;
        case WM_RBUTTONDOWN:
            SetCapture(hwnd);
            app->sendMouseEvent(hwnd, EventType::MouseDown, lParam, MouseButton::Right);
            return 0;
        case WM_RBUTTONUP:
            if (GetCapture() == hwnd) {
                ReleaseCapture();
            }
            app->sendMouseEvent(hwnd, EventType::MouseUp, lParam, MouseButton::Right);
            return 0;
        case WM_MOUSEWHEEL:
            app->sendWheelEvent(hwnd, wParam, lParam, false);
            return 0;
        case WM_MOUSEHWHEEL:
            app->sendWheelEvent(hwnd, wParam, lParam, true);
            return 0;
        case WM_KEYDOWN:
            if (app->sendKeyEvent(hwnd, wParam)) {
                return 0;
            }
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CHAR:
            if (!app->suppressedImeChars_.empty() &&
                static_cast<wchar_t>(wParam) == app->suppressedImeChars_.front()) {
                app->suppressedImeChars_.erase(app->suppressedImeChars_.begin());
                return 0;
            }
            if (wParam >= 0x20 && wParam != 0x7F) {
                if (app->sendTextInputEvent(hwnd, utf8FromWChar(static_cast<wchar_t>(wParam)))) {
                    return 0;
                }
            }
            break;
        case WM_IME_COMPOSITION:
            if (lParam & GCS_RESULTSTR) {
                const std::wstring result = imeCompositionString(hwnd, GCS_RESULTSTR);
                if (!result.empty()) {
                    app->suppressedImeChars_ += result;
                    app->sendTextInputEvent(hwnd, utf8FromWide(result));
                }
                app->sendImeEvent(hwnd, EventType::ImeEnd);
                return 0;
            }
            if (lParam & GCS_COMPSTR) {
                app->sendImeEvent(hwnd, EventType::ImeComposition, utf8FromWide(imeCompositionString(hwnd, GCS_COMPSTR)));
                return 0;
            }
            break;
        case WM_IME_ENDCOMPOSITION:
            app->sendImeEvent(hwnd, EventType::ImeEnd);
            return 0;
        case WM_PAINT:
            app->paint(hwnd);
            return 0;
        case WM_ERASEBKGND:
            app->eraseBackground(hwnd, reinterpret_cast<HDC>(wParam));
            return 1;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    bool renderD3D(HWND hwnd, int width, int height) {
        const bool ok = d3d_.render(
            hwnd,
            width,
            height,
            [this](SkCanvas& canvas, int drawWidth, int drawHeight) {
                runtime_.resize(drawWidth, drawHeight, dpiScaleForDpi(dpi_));
                runtime_.render(canvas);
            },
            [this](uint32_t* pixels, int drawWidth, int drawHeight, size_t rowBytes) {
                return renderCpuSurface(pixels, drawWidth, drawHeight, rowBytes);
            });
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
        runtime_.resize(width, height, dpiScaleForDpi(dpi_));
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
