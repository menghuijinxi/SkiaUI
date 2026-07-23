#include "skui_win32_event_adapter.h"

#include <imm.h>
#include <shlobj_core.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <charconv>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

std::string readClipboardBytes(UINT format) {
    HANDLE handle = GetClipboardData(format);
    if (!handle) {
        return {};
    }
    const SIZE_T size = GlobalSize(handle);
    const auto* memory = static_cast<const char*>(GlobalLock(handle));
    if (!memory || size == 0) {
        if (memory) {
            GlobalUnlock(handle);
        }
        return {};
    }
    std::string bytes(memory, memory + size);
    GlobalUnlock(handle);
    while (!bytes.empty() && bytes.back() == '\0') {
        bytes.pop_back();
    }
    return bytes;
}

std::optional<size_t> htmlClipboardOffset(std::string_view payload,
                                          std::string_view label) {
    const size_t labelPosition = payload.find(label);
    if (labelPosition == std::string_view::npos) {
        return std::nullopt;
    }
    const size_t valueBegin = labelPosition + label.size();
    const size_t valueEnd = payload.find_first_of("\r\n", valueBegin);
    const std::string_view value = payload.substr(
        valueBegin,
        valueEnd == std::string_view::npos
            ? std::string_view::npos
            : valueEnd - valueBegin);
    size_t offset = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), offset);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
    }
    return offset;
}

std::string readClipboardHtml() {
    const UINT format = RegisterClipboardFormatW(L"HTML Format");
    if (format == 0 || !IsClipboardFormatAvailable(format)) {
        return {};
    }
    const std::string payload = readClipboardBytes(format);
    const std::optional<size_t> start =
        htmlClipboardOffset(payload, "StartFragment:");
    const std::optional<size_t> end =
        htmlClipboardOffset(payload, "EndFragment:");
    if (start && end && *start <= *end && *end <= payload.size()) {
        return payload.substr(*start, *end - *start);
    }

    constexpr std::string_view kStartMarker = "<!--StartFragment-->";
    constexpr std::string_view kEndMarker = "<!--EndFragment-->";
    const size_t markerStart = payload.find(kStartMarker);
    const size_t markerEnd = payload.find(kEndMarker);
    if (markerStart == std::string::npos || markerEnd == std::string::npos ||
        markerEnd < markerStart + kStartMarker.size()) {
        return {};
    }
    const size_t fragmentStart = markerStart + kStartMarker.size();
    return payload.substr(fragmentStart, markerEnd - fragmentStart);
}

std::vector<std::string> readClipboardFilePaths() {
    std::vector<std::string> paths;
    if (!IsClipboardFormatAvailable(CF_HDROP)) {
        return paths;
    }
    const auto drop = static_cast<HDROP>(GetClipboardData(CF_HDROP));
    if (!drop) {
        return paths;
    }
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);
    paths.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        const UINT length = DragQueryFileW(drop, index, nullptr, 0);
        if (length == 0) {
            continue;
        }
        std::wstring path(static_cast<size_t>(length) + 1u, L'\0');
        const UINT copied =
            DragQueryFileW(drop, index, path.data(), length + 1u);
        if (copied == 0) {
            continue;
        }
        path.resize(copied);
        paths.push_back(utf8FromWide(path));
    }
    return paths;
}

ClipboardContent readClipboardContent() {
    ClipboardContent content;
    if (!OpenClipboard(nullptr)) {
        return content;
    }
    content.html = readClipboardHtml();
    content.filePaths = readClipboardFilePaths();
    if (HANDLE handle = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* text = static_cast<const wchar_t*>(GlobalLock(handle))) {
            content.text = utf8FromWide(text);
            GlobalUnlock(handle);
        }
    }
    CloseClipboard();
    return content;
}

bool setClipboardBytes(UINT format,
                       const void* bytes,
                       size_t byteCount) {
    if (format == 0 || !bytes || byteCount == 0) {
        return false;
    }
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, byteCount);
    if (!handle) {
        return false;
    }
    void* memory = GlobalLock(handle);
    if (!memory) {
        GlobalFree(handle);
        return false;
    }
    std::memcpy(memory, bytes, byteCount);
    GlobalUnlock(handle);
    if (!SetClipboardData(format, handle)) {
        GlobalFree(handle);
        return false;
    }
    return true;
}

bool writeHtmlClipboardOffset(std::string& header,
                              std::string_view label,
                              size_t offset) {
    constexpr size_t kOffsetDigits = 10;
    const size_t position = header.find(label);
    if (position == std::string::npos ||
        offset > 9'999'999'999ull) {
        return false;
    }
    char digits[kOffsetDigits];
    std::fill(std::begin(digits), std::end(digits), '0');
    char value[32];
    const auto result = std::to_chars(
        std::begin(value), std::end(value), offset);
    if (result.ec != std::errc{}) {
        return false;
    }
    const size_t length = static_cast<size_t>(result.ptr - value);
    std::copy(value, result.ptr, digits + kOffsetDigits - length);
    header.replace(position + label.size(), kOffsetDigits, digits, kOffsetDigits);
    return true;
}

std::string makeHtmlClipboardPayload(std::string_view fragment) {
    std::string header =
        "Version:1.0\r\n"
        "StartHTML:0000000000\r\n"
        "EndHTML:0000000000\r\n"
        "StartFragment:0000000000\r\n"
        "EndFragment:0000000000\r\n";
    constexpr std::string_view kHtmlPrefix =
        "<html><body><!--StartFragment-->";
    constexpr std::string_view kHtmlSuffix =
        "<!--EndFragment--></body></html>";
    const size_t startHtml = header.size();
    const size_t startFragment = startHtml + kHtmlPrefix.size();
    const size_t endFragment = startFragment + fragment.size();
    const size_t endHtml = endFragment + kHtmlSuffix.size();
    if (!writeHtmlClipboardOffset(header, "StartHTML:", startHtml) ||
        !writeHtmlClipboardOffset(header, "EndHTML:", endHtml) ||
        !writeHtmlClipboardOffset(
            header, "StartFragment:", startFragment) ||
        !writeHtmlClipboardOffset(header, "EndFragment:", endFragment)) {
        return {};
    }
    header += kHtmlPrefix;
    header += fragment;
    header += kHtmlSuffix;
    header.push_back('\0');
    return header;
}

HGLOBAL createDroppedFilesHandle(
    const std::vector<std::string>& filePaths) {
    std::vector<std::wstring> paths;
    size_t characterCount = 1;
    paths.reserve(filePaths.size());
    for (const std::string& filePath : filePaths) {
        std::wstring path = wideFromUtf8(filePath);
        if (path.empty()) {
            continue;
        }
        characterCount += path.size() + 1u;
        paths.push_back(std::move(path));
    }
    if (paths.empty() ||
        characterCount >
            (std::numeric_limits<SIZE_T>::max() - sizeof(DROPFILES)) /
                sizeof(wchar_t)) {
        return nullptr;
    }

    const SIZE_T bytes = sizeof(DROPFILES) +
                         characterCount * sizeof(wchar_t);
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!handle) {
        return nullptr;
    }
    void* memory = GlobalLock(handle);
    if (!memory) {
        GlobalFree(handle);
        return nullptr;
    }
    auto* dropFiles = static_cast<DROPFILES*>(memory);
    dropFiles->pFiles = sizeof(DROPFILES);
    dropFiles->fWide = TRUE;
    auto* destination = reinterpret_cast<wchar_t*>(
        static_cast<unsigned char*>(memory) + sizeof(DROPFILES));
    for (const std::wstring& path : paths) {
        destination = std::copy(path.begin(), path.end(), destination);
        *destination++ = L'\0';
    }
    *destination = L'\0';
    GlobalUnlock(handle);
    return handle;
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

void writeClipboardContent(const ClipboardContent& content) {
    if (!OpenClipboard(nullptr)) {
        return;
    }
    EmptyClipboard();

    if (!content.html.empty()) {
        const UINT format = RegisterClipboardFormatW(L"HTML Format");
        const std::string payload = makeHtmlClipboardPayload(content.html);
        if (!payload.empty()) {
            (void)setClipboardBytes(
                format, payload.data(), payload.size());
        }
    }
    if (!content.filePaths.empty()) {
        HGLOBAL handle = createDroppedFilesHandle(content.filePaths);
        if (handle && !SetClipboardData(CF_HDROP, handle)) {
            GlobalFree(handle);
        }
    }
    if (!content.text.empty()) {
        const std::wstring wide = wideFromUtf8(content.text);
        const size_t bytes = (wide.size() + 1u) * sizeof(wchar_t);
        (void)setClipboardBytes(CF_UNICODETEXT, wide.c_str(), bytes);
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
    if (!options.readClipboardContent) {
        options.readClipboardContent = readClipboardContent;
    }
    if (!options.writeClipboardContent) {
        options.writeClipboardContent = writeClipboardContent;
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
