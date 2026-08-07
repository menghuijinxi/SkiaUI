#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "skui_runtime.h"

#include <windows.h>

#include <bitset>
#include <functional>
#include <optional>
#include <string>

namespace skui::win32 {

RuntimeOptions withWin32PlatformCallbacks(RuntimeOptions options);

class Win32EventAdapter {
public:
    explicit Win32EventAdapter(Runtime& runtime);

    void setRuntimeDirtyCallback(std::function<void()> callback);
    [[nodiscard]] std::optional<LRESULT> handleMessage(HWND hwnd,
                                                       UINT message,
                                                       WPARAM wParam,
                                                       LPARAM lParam);
    void updateCursor();

private:
    void notifyRuntimeDirty() const;
    [[nodiscard]] bool sendMouseEvent(EventType type,
                                       LPARAM lParam,
                                       MouseButton button = MouseButton::None);
    [[nodiscard]] bool sendWheelEvent(HWND hwnd,
                                      WPARAM wParam,
                                      LPARAM lParam,
                                      bool horizontal);
    [[nodiscard]] bool sendKeyEvent(EventType type, WPARAM key);
    [[nodiscard]] bool sendImeEvent(EventType type, std::string text = {});
    [[nodiscard]] bool sendTextInputEvent(std::string text);
    void updateImePosition(HWND hwnd) const;
    [[nodiscard]] bool queryImeCharacterPosition(HWND hwnd, LPARAM lParam) const;
    void beginMouseLeaveTracking(HWND hwnd);

    Runtime& runtime_;
    std::function<void()> onRuntimeDirty_;
    bool trackingMouseLeave_ = false;
    bool captureOwned_ = false;
    MouseButton capturedButton_ = MouseButton::None;
    std::bitset<256> consumedKeys_;
    std::wstring suppressedImeChars_;
    HCURSOR currentCursor_ = nullptr;
};

}  // namespace skui::win32
