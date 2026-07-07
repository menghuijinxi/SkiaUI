#include "skui_win32_event_adapter.h"

#include <imm.h>
#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>

namespace skui::win32 {
namespace {

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
    const int bytes = WideCharToMultiByte(CP_UTF8,
                                          0,
                                          text.data(),
                                          static_cast<int>(text.size()),
                                          nullptr,
                                          0,
                                          nullptr,
                                          nullptr);
    if (bytes <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        text.data(),
                        static_cast<int>(text.size()),
                        out.data(),
                        bytes,
                        nullptr,
                        nullptr);
    return out;
}

std::wstring wideFromUtf8(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int chars = MultiByteToWideChar(CP_UTF8,
                                          0,
                                          text.data(),
                                          static_cast<int>(text.size()),
                                          nullptr,
                                          0);
    if (chars <= 0) {
        return {};
    }
    std::wstring out(static_cast<size_t>(chars), L'\0');
    MultiByteToWideChar(CP_UTF8,
                        0,
                        text.data(),
                        static_cast<int>(text.size()),
                        out.data(),
                        chars);
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

HCURSOR cursorHandle(Cursor cursor) {
    LPCWSTR id = IDC_ARROW;
    switch (cursor) {
    case Cursor::Pointer:
        id = IDC_HAND;
        break;
    case Cursor::Text:
        id = IDC_IBEAM;
        break;
    case Cursor::EWResize:
        id = IDC_SIZEWE;
        break;
    case Cursor::NSResize:
        id = IDC_SIZENS;
        break;
    case Cursor::Move:
        id = IDC_SIZEALL;
        break;
    case Cursor::Crosshair:
        id = IDC_CROSS;
        break;
    case Cursor::NotAllowed:
        id = IDC_NO;
        break;
    case Cursor::Auto:
    case Cursor::Default:
    default:
        id = IDC_ARROW;
        break;
    }
    return LoadCursorW(nullptr, id);
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

void beginMousePress(HWND hwnd) {
    if (!hwnd) {
        return;
    }
    SetFocus(hwnd);
    SetCapture(hwnd);
}

}  // namespace

RuntimeOptions withWin32PlatformCallbacks(RuntimeOptions options) {
    if (!options.readClipboardText) {
        options.readClipboardText = readClipboardText;
    }
    if (!options.writeClipboardText) {
        options.writeClipboardText = writeClipboardText;
    }
    return options;
}

Win32EventAdapter::Win32EventAdapter(Runtime& runtime) : runtime_(runtime) {}

void Win32EventAdapter::setRuntimeDirtyCallback(std::function<void()> callback) {
    onRuntimeDirty_ = std::move(callback);
}

std::optional<LRESULT> Win32EventAdapter::handleMessage(HWND hwnd,
                                                        UINT message,
                                                        WPARAM wParam,
                                                        LPARAM lParam) {
    switch (message) {
    case WM_MOUSEMOVE:
        beginMouseLeaveTracking(hwnd);
        sendMouseEvent(EventType::MouseMove, lParam);
        return 0;
    case WM_MOUSELEAVE:
        trackingMouseLeave_ = false;
        sendMouseEvent(EventType::MouseLeave, lParam);
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lParam) == HTCLIENT) {
            updateCursor();
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        beginMousePress(hwnd);
        sendMouseEvent(EventType::MouseDown, lParam, MouseButton::Left);
        return 0;
    case WM_LBUTTONDBLCLK:
        beginMousePress(hwnd);
        sendMouseEvent(EventType::MouseDoubleClick, lParam, MouseButton::Left);
        return 0;
    case WM_LBUTTONUP:
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        sendMouseEvent(EventType::MouseUp, lParam, MouseButton::Left);
        return 0;
    case WM_MBUTTONDOWN:
        beginMousePress(hwnd);
        sendMouseEvent(EventType::MouseDown, lParam, MouseButton::Middle);
        return 0;
    case WM_MBUTTONUP:
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        sendMouseEvent(EventType::MouseUp, lParam, MouseButton::Middle);
        return 0;
    case WM_RBUTTONDOWN:
        beginMousePress(hwnd);
        sendMouseEvent(EventType::MouseDown, lParam, MouseButton::Right);
        return 0;
    case WM_RBUTTONUP:
        if (GetCapture() == hwnd) {
            ReleaseCapture();
        }
        sendMouseEvent(EventType::MouseUp, lParam, MouseButton::Right);
        return 0;
    case WM_MOUSEWHEEL:
        sendWheelEvent(hwnd, wParam, lParam, false);
        return 0;
    case WM_MOUSEHWHEEL:
        sendWheelEvent(hwnd, wParam, lParam, true);
        return 0;
    case WM_KEYDOWN:
        if (sendKeyEvent(wParam)) {
            return 0;
        }
        break;
    case WM_CHAR:
        if (!suppressedImeChars_.empty() &&
            static_cast<wchar_t>(wParam) == suppressedImeChars_.front()) {
            suppressedImeChars_.erase(suppressedImeChars_.begin());
            return 0;
        }
        if (wParam >= 0x20 && wParam != 0x7F) {
            if (sendTextInputEvent(utf8FromWChar(static_cast<wchar_t>(wParam)))) {
                return 0;
            }
        }
        break;
    case WM_IME_COMPOSITION:
        if (lParam & GCS_RESULTSTR) {
            const std::wstring result = imeCompositionString(hwnd, GCS_RESULTSTR);
            if (!result.empty()) {
                suppressedImeChars_ += result;
                (void)sendTextInputEvent(utf8FromWide(result));
            }
            (void)sendImeEvent(EventType::ImeEnd);
            return 0;
        }
        if (lParam & GCS_COMPSTR) {
            (void)sendImeEvent(EventType::ImeComposition,
                               utf8FromWide(imeCompositionString(hwnd, GCS_COMPSTR)));
            return 0;
        }
        break;
    case WM_IME_ENDCOMPOSITION:
        (void)sendImeEvent(EventType::ImeEnd);
        return 0;
    default:
        break;
    }
    return std::nullopt;
}

void Win32EventAdapter::updateCursor() {
    HCURSOR next = cursorHandle(runtime_.cursor());
    if (!next) {
        next = LoadCursorW(nullptr, IDC_ARROW);
    }
    if (next != currentCursor_) {
        currentCursor_ = next;
    }
    SetCursor(currentCursor_);
}

void Win32EventAdapter::notifyRuntimeDirty() const {
    if (runtime_.dirty() && onRuntimeDirty_) {
        onRuntimeDirty_();
    }
}

void Win32EventAdapter::sendMouseEvent(EventType type,
                                      LPARAM lParam,
                                      MouseButton button) {
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
    updateCursor();
    notifyRuntimeDirty();
}

void Win32EventAdapter::sendWheelEvent(HWND hwnd,
                                      WPARAM wParam,
                                      LPARAM lParam,
                                      bool horizontal) {
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
    notifyRuntimeDirty();
}

bool Win32EventAdapter::sendKeyEvent(WPARAM key) {
    Event event;
    event.type = EventType::KeyDown;
    event.key = static_cast<unsigned>(key);
    event.shiftKey = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    event.ctrlKey = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool consumed = runtime_.handleEvent(event);
    notifyRuntimeDirty();
    return consumed;
}

bool Win32EventAdapter::sendImeEvent(EventType type, std::string text) {
    Event event;
    event.type = type;
    event.text = std::move(text);
    const bool consumed = runtime_.handleEvent(event);
    notifyRuntimeDirty();
    return consumed;
}

bool Win32EventAdapter::sendTextInputEvent(std::string text) {
    if (text.empty()) {
        return false;
    }
    Event event;
    event.type = EventType::TextInput;
    event.text = std::move(text);
    const bool consumed = runtime_.handleEvent(event);
    notifyRuntimeDirty();
    return consumed;
}

void Win32EventAdapter::beginMouseLeaveTracking(HWND hwnd) {
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

}  // namespace skui::win32
