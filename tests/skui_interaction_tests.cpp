#include "skui_runtime.h"
#include "skui_runtime_helpers.h"
#include "skui_dropdown.h"
#include "skui_virtual_table.h"
#ifdef _WIN32
#include "skui_win32_event_adapter.h"
#endif

#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/encode/SkJpegEncoder.h"
#include "include/encode/SkPngEncoder.h"
#include "include/encode/SkWebpEncoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#include <array>
#include <atomic>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr int kWidth = 140;
constexpr int kHeight = 90;

uint32_t pixelAt(const std::vector<uint32_t>& pixels, int x, int y) {
    return pixels[static_cast<size_t>(y) * kWidth + static_cast<size_t>(x)];
}

uint32_t pixelAt(const std::vector<uint32_t>& pixels, int width, int x, int y) {
    return pixels[static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)];
}

constexpr uint32_t solidColor(unsigned r, unsigned g, unsigned b) {
    return 0xFF000000u | (r << 16u) | (g << 8u) | b;
}

std::string utf8PathText(const std::filesystem::path& path) {
#ifdef _WIN32
    const std::wstring wide = path.wstring();
    if (wide.empty()) {
        return {};
    }
    const int utf8Size = WideCharToMultiByte(CP_UTF8,
                                             0,
                                             wide.data(),
                                             static_cast<int>(wide.size()),
                                             nullptr,
                                             0,
                                             nullptr,
                                             nullptr);
    if (utf8Size <= 0) {
        return path.string();
    }
    std::string utf8(static_cast<size_t>(utf8Size), '\0');
    WideCharToMultiByte(CP_UTF8,
                        0,
                        wide.data(),
                        static_cast<int>(wide.size()),
                        utf8.data(),
                        utf8Size,
                        nullptr,
                        nullptr);
    return utf8;
#else
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
#endif
}

bool isMostlyRed(uint32_t color) {
    const unsigned red = (color >> 16u) & 0xFFu;
    const unsigned green = (color >> 8u) & 0xFFu;
    const unsigned blue = color & 0xFFu;
    return red > 180u && green < 80u && blue < 80u;
}

bool isMostlyBlue(uint32_t color) {
    const unsigned red = (color >> 16u) & 0xFFu;
    const unsigned green = (color >> 8u) & 0xFFu;
    const unsigned blue = color & 0xFFu;
    return blue > 180u && red < 80u && green < 80u;
}

bool renderPixel(skui::Runtime& runtime, int x, int y, uint32_t& out) {
    std::vector<uint32_t> pixels;
    pixels.assign(static_cast<size_t>(kWidth) * kHeight, 0);
    if (!runtime.renderToBgraPixels(pixels.data(), kWidth, kHeight, static_cast<size_t>(kWidth) * sizeof(uint32_t), 1.0f)) {
        std::cerr << "render failed: " << runtime.lastError() << "\n";
        return false;
    }
    out = pixelAt(pixels, x, y);
    return true;
}

bool renderPixelAt(skui::Runtime& runtime, int width, int height, int x, int y, uint32_t& out) {
    std::vector<uint32_t> pixels;
    pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);
    if (!runtime.renderToBgraPixels(pixels.data(), width, height, static_cast<size_t>(width) * sizeof(uint32_t), 1.0f)) {
        std::cerr << "render failed: " << runtime.lastError() << "\n";
        return false;
    }
    out = pixelAt(pixels, width, x, y);
    return true;
}

bool renderPixels(skui::Runtime& runtime, std::vector<uint32_t>& pixels) {
    pixels.assign(static_cast<size_t>(kWidth) * kHeight, 0);
    if (!runtime.renderToBgraPixels(pixels.data(), kWidth, kHeight, static_cast<size_t>(kWidth) * sizeof(uint32_t), 1.0f)) {
        std::cerr << "render failed: " << runtime.lastError() << "\n";
        return false;
    }
    return true;
}

void waitForDirty(skui::Runtime& runtime) {
    for (int i = 0; i < 100 && !runtime.dirty(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

bool isBrightPixel(uint32_t color) {
    return ((color >> 16u) & 0xffu) > 180u &&
           ((color >> 8u) & 0xffu) > 180u &&
           (color & 0xffu) > 180u;
}

int countBrightPixels(const std::vector<uint32_t>& pixels, int left, int top, int right, int bottom) {
    int count = 0;
    for (int y = std::max(0, top); y < std::min(kHeight, bottom); ++y) {
        for (int x = std::max(0, left); x < std::min(kWidth, right); ++x) {
            if (isBrightPixel(pixelAt(pixels, x, y))) {
                ++count;
            }
        }
    }
    return count;
}

void sendMouse(skui::Runtime& runtime, skui::EventType type, float x, float y, bool shift = false) {
    skui::Event event;
    event.type = type;
    event.x = x;
    event.y = y;
    event.shiftKey = shift;
    if (type == skui::EventType::MouseDown ||
        type == skui::EventType::MouseDoubleClick ||
        type == skui::EventType::MouseUp) {
        event.button = skui::MouseButton::Left;
    }
    runtime.handleEvent(event);
}

void sendWheel(skui::Runtime& runtime, float x, float y, float delta, bool shift = false) {
    skui::Event event;
    event.type = skui::EventType::MouseWheel;
    event.x = x;
    event.y = y;
    event.wheelDelta = delta;
    event.shiftKey = shift;
    runtime.handleEvent(event);
}

void sendKey(skui::Runtime& runtime, unsigned key, bool shift = false, bool ctrl = false) {
    skui::Event event;
    event.type = skui::EventType::KeyDown;
    event.key = key;
    event.shiftKey = shift;
    event.ctrlKey = ctrl;
    runtime.handleEvent(event);
}

void sendText(skui::Runtime& runtime, std::string_view text) {
    skui::Event event;
    event.type = skui::EventType::TextInput;
    event.text = std::string(text);
    runtime.handleEvent(event);
}

void sendIme(skui::Runtime& runtime, skui::EventType type, std::string_view text = {}) {
    skui::Event event;
    event.type = type;
    event.text = std::string(text);
    runtime.handleEvent(event);
}

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << "\n";
        return false;
    }
    return true;
}

bool writeBytesFixture(const std::filesystem::path& path, const unsigned char* data, size_t size) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    return file.good();
}

bool writeRedBmpFixture(const std::filesystem::path& path) {
    static constexpr unsigned char kRedBmp[] = {
        0x42, 0x4D, 0x3A, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x36, 0x00, 0x00, 0x00, 0x28, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00,
        0x00, 0x00, 0x01, 0x00, 0x18, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x13, 0x0B,
        0x00, 0x00, 0x13, 0x0B, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0xFF, 0x00,
    };
    return writeBytesFixture(path, kRedBmp, sizeof(kRedBmp));
}

SkPixmap redFixturePixmap() {
    static constexpr std::array<unsigned char, 16> kRedRgba = {
        0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF,
        0xFF, 0x00, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0xFF,
    };
    static const SkImageInfo kInfo =
        SkImageInfo::Make(2, 2, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    return SkPixmap(kInfo, kRedRgba.data(), 2u * 4u);
}

bool writeSkDataFixture(const std::filesystem::path& path, sk_sp<SkData> data) {
    if (!data || data->size() == 0) {
        return false;
    }
    return writeBytesFixture(path,
                             static_cast<const unsigned char*>(data->data()),
                             data->size());
}

bool writeRedPngFixture(const std::filesystem::path& path) {
    SkPngEncoder::Options options;
    return writeSkDataFixture(path, SkPngEncoder::Encode(redFixturePixmap(), options));
}

bool writeSolidPngFixture(const std::filesystem::path& path,
                          unsigned char red,
                          unsigned char green,
                          unsigned char blue) {
    const std::array<unsigned char, 16> rgba = {
        red, green, blue, 0xFF, red, green, blue, 0xFF,
        red, green, blue, 0xFF, red, green, blue, 0xFF,
    };
    const SkImageInfo info = SkImageInfo::Make(2, 2, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    SkPixmap pixmap(info, rgba.data(), 2u * 4u);
    SkPngEncoder::Options options;
    return writeSkDataFixture(path, SkPngEncoder::Encode(pixmap, options));
}

bool writeBluePngFixture(const std::filesystem::path& path) {
    return writeSolidPngFixture(path, 0x00, 0x00, 0xFF);
}

bool writeRedJpegFixture(const std::filesystem::path& path) {
    SkJpegEncoder::Options options;
    options.fQuality = 95;
    return writeSkDataFixture(path, SkJpegEncoder::Encode(redFixturePixmap(), options));
}

bool writeRedWebpFixture(const std::filesystem::path& path) {
    SkWebpEncoder::Options options;
    options.fCompression = SkWebpEncoder::Compression::kLossless;
    return writeSkDataFixture(path, SkWebpEncoder::Encode(redFixturePixmap(), options));
}

#ifdef _WIN32
LRESULT CALLBACK testWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

class TestWin32Window {
public:
    TestWin32Window() {
        WNDCLASSW wc{};
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = testWindowProc;
        wc.lpszClassName = L"SkuiInteractionTestsWindow";
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return;
        }
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW,
                                wc.lpszClassName,
                                L"SkuiInteractionTestsWindow",
                                WS_OVERLAPPEDWINDOW,
                                0,
                                0,
                                80,
                                60,
                                nullptr,
                                nullptr,
                                wc.hInstance,
                                nullptr);
        if (hwnd_) {
            ShowWindow(hwnd_, SW_SHOWNORMAL);
            UpdateWindow(hwnd_);
        }
    }

    ~TestWin32Window() {
        if (hwnd_) {
            DestroyWindow(hwnd_);
        }
    }

    TestWin32Window(const TestWin32Window&) = delete;
    TestWin32Window& operator=(const TestWin32Window&) = delete;

    [[nodiscard]] HWND hwnd() const {
        return hwnd_;
    }

private:
    HWND hwnd_ = nullptr;
};
#endif

}  // namespace

int main() {
    constexpr std::string_view html = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .tile {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 80px;
      height: 40px;
      background-color: #112233;
    }
    .tile:hover {
      background-color: #334455;
    }
    .tile:active {
      background-color: #556677;
    }
    .selected {
      background-color: #778899;
    }
  </style>
</head>
<body>
  <div class="root">
    <div id="tile" class="tile" data-action="tile-click"></div>
  </div>
</body>
</html>
)html";

    skui::RuntimeOptions options;
    options.clearColor = SK_ColorBLACK;

    bool ok = true;
    {
        skui::RuntimeOptions scaledOptions = options;
        scaledOptions.scale = 1.5f;
        skui::Runtime scaledRuntime(scaledOptions);
        scaledRuntime.resize(300, 150, 2.0f);

        ok = expect(scaledRuntime.dpiScale() == 2.0f, "dpiScale should keep platform scale") && ok;
        ok = expect(scaledRuntime.scale() > 1.49f && scaledRuntime.scale() < 1.51f,
                    "runtime scale should expose user scale") && ok;
        ok = expect(scaledRuntime.effectiveScale() > 2.99f && scaledRuntime.effectiveScale() < 3.01f,
                    "effective scale should combine platform dpi and user scale") && ok;
        ok = expect(skui::runtimeLogicalWidth(scaledRuntime) == 100,
                    "logical width should use effective scale") && ok;
        ok = expect(skui::runtimeLogicalHeight(scaledRuntime) == 50,
                    "logical height should use effective scale") && ok;

        scaledRuntime.setScale(0.5f);
        ok = expect(scaledRuntime.effectiveScale() > 0.99f && scaledRuntime.effectiveScale() < 1.01f,
                    "setScale should update effective scale") && ok;
        ok = expect(skui::runtimeLogicalWidth(scaledRuntime) == 300,
                    "setScale should update logical width") && ok;
    }
    {
        skui::Runtime runtime(options);
        runtime.resize(kWidth, kHeight, 1.0f);

        int clickCount = 0;
        std::string lastAction;
        runtime.setElementEventCallback([&](const skui::ElementEvent& event) {
            if (event.type == skui::ElementEventType::Click) {
                ++clickCount;
                lastAction = event.action;
                runtime.addClassById(event.id, "selected");
            }
        });

        if (!runtime.loadDocumentFromString(html, "")) {
            std::cerr << "load failed: " << runtime.lastError() << "\n";
            return 1;
        }
        uint32_t normal = 0;
        uint32_t hover = 0;
        uint32_t active = 0;
        uint32_t selected = 0;

        ok = renderPixel(runtime, 20, 20, normal) && ok;
        sendMouse(runtime, skui::EventType::MouseMove, 20.0f, 20.0f);
        ok = renderPixel(runtime, 20, 20, hover) && ok;
        sendMouse(runtime, skui::EventType::MouseDown, 20.0f, 20.0f);
        ok = renderPixel(runtime, 20, 20, active) && ok;
        sendMouse(runtime, skui::EventType::MouseUp, 20.0f, 20.0f);
        ok = renderPixel(runtime, 20, 20, selected) && ok;
        ok = expect(normal != hover, "div:hover should change rendered output") && ok;
        ok = expect(hover != active, "div:active should change rendered output") && ok;
        ok = expect(clickCount == 1, "click should be emitted for data-action div") && ok;
        ok = expect(lastAction == "tile-click", "click action should be routed from data-action") && ok;
        ok = expect(runtime.hasClassById("tile", "selected"), "click callback should be able to mutate classes") && ok;
        ok = expect(selected != normal, "class mutation after click should repaint") && ok;
        ok = runtime.setTextById("tile", "label") && ok;
        int actionMoves = 0;
        runtime.setElementEventCallback([&](const skui::ElementEvent& event) {
            if (event.type == skui::ElementEventType::MouseMove && event.action == "tile-click") {
                ++actionMoves;
            }
        });
        sendMouse(runtime, skui::EventType::MouseMove, 20.0f, 20.0f);
        ok = expect(actionMoves == 1, "mouse move should be emitted for data-action elements") && ok;
    }

    constexpr std::string_view buttonActionHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .button {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 80px;
      height: 40px;
      background-color: #335577;
    }
    .button-label {
      position: absolute;
      left: 8px;
      top: 8px;
      width: 60px;
      height: 20px;
    }
  </style>
</head>
<body>
  <div class="root">
    <button class="button" data-action="button-click">
      <span class="button-label">Run</span>
    </button>
  </div>
</body>
</html>
)html";
    {
        skui::Runtime buttonRuntime(options);
        buttonRuntime.resize(kWidth, kHeight, 1.0f);
        ok = expect(buttonRuntime.loadDocumentFromString(buttonActionHtml), "button action document should load") && ok;
        int buttonClicks = 0;
        buttonRuntime.setElementEventCallback([&](const skui::ElementEvent& event) {
            if (event.type == skui::ElementEventType::Click && event.action == "button-click") {
                ++buttonClicks;
            }
        });
        sendMouse(buttonRuntime, skui::EventType::MouseDown, 20.0f, 20.0f);
        sendMouse(buttonRuntime, skui::EventType::MouseUp, 20.0f, 20.0f);
        ok = expect(buttonClicks == 1, "click should be emitted for data-action button") && ok;
        sendMouse(buttonRuntime, skui::EventType::MouseDoubleClick, 20.0f, 20.0f);
        sendMouse(buttonRuntime, skui::EventType::MouseUp, 20.0f, 20.0f);
        ok = expect(buttonClicks == 2, "double-click press should also emit a button click") && ok;
    }

#ifdef _WIN32
    {
        skui::Runtime adapterRuntime(options);
        adapterRuntime.resize(kWidth, kHeight, 1.0f);
        ok = expect(adapterRuntime.loadDocumentFromString(html, ""),
                    "win32 adapter action document should load") && ok;

        int mouseDowns = 0;
        int clicks = 0;
        adapterRuntime.setElementEventCallback([&](const skui::ElementEvent& event) {
            if (event.action != "tile-click") {
                return;
            }
            if (event.type == skui::ElementEventType::MouseDown) {
                ++mouseDowns;
            } else if (event.type == skui::ElementEventType::Click) {
                ++clicks;
            }
        });

        uint32_t normal = 0;
        uint32_t active = 0;
        ok = renderPixel(adapterRuntime, 20, 20, normal) && ok;
        adapterRuntime.clearDirty();

        int dirtyCallbacks = 0;
        skui::win32::Win32EventAdapter adapter(adapterRuntime);
        adapter.setRuntimeDirtyCallback([&] {
            ++dirtyCallbacks;
        });

        TestWin32Window window;
        ok = expect(window.hwnd() != nullptr, "win32 adapter test window should be created") && ok;
        if (window.hwnd()) {
            SetFocus(nullptr);
            const std::optional<LRESULT> downResult =
                adapter.handleMessage(window.hwnd(),
                                      WM_LBUTTONDOWN,
                                      MK_LBUTTON,
                                      MAKELPARAM(20, 20));
            ok = expect(downResult.has_value() && *downResult == 0,
                        "win32 adapter should consume left button down") && ok;
            ok = expect(mouseDowns == 1,
                        "win32 adapter should emit mouse down for data-action elements") && ok;
            ok = expect(dirtyCallbacks == 1,
                        "win32 adapter mouse down should request a repaint") && ok;
            ok = expect(GetFocus() == window.hwnd(),
                        "win32 adapter mouse down should focus the host window") && ok;
            ok = renderPixel(adapterRuntime, 20, 20, active) && ok;
            ok = expect(normal != active,
                        "win32 adapter mouse down should render the active state") && ok;

            const std::optional<LRESULT> upResult =
                adapter.handleMessage(window.hwnd(), WM_LBUTTONUP, 0, MAKELPARAM(20, 20));
            ok = expect(upResult.has_value() && *upResult == 0,
                        "win32 adapter should consume left button up") && ok;
            ok = expect(clicks == 1,
                        "win32 adapter should emit click after left button up") && ok;
        }
    }
#endif

    constexpr std::string_view pointerEventsHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .base {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 100px;
      height: 60px;
      background-color: #112233;
    }
    .base:hover {
      background-color: #445566;
    }
    .overlay {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 100px;
      height: 60px;
      pointer-events: none;
      background-color: rgba(255,255,255,0);
    }
  </style>
</head>
<body>
  <div id="base" class="base" data-action="base-click"></div>
  <div class="overlay"></div>
</body>
</html>
)html";
    skui::Runtime pointerEventsRuntime(options);
    pointerEventsRuntime.resize(kWidth, kHeight, 1.0f);
    if (!pointerEventsRuntime.loadDocumentFromString(pointerEventsHtml, "")) {
        std::cerr << "pointer-events load failed: " << pointerEventsRuntime.lastError() << "\n";
        return 1;
    }
    int pointerEventsClicks = 0;
    pointerEventsRuntime.setElementEventCallback([&](const skui::ElementEvent& event) {
        if (event.type == skui::ElementEventType::Click && event.action == "base-click") {
            ++pointerEventsClicks;
        }
    });
    uint32_t pointerEventsNormal = 0;
    uint32_t pointerEventsHover = 0;
    ok = renderPixel(pointerEventsRuntime, 20, 20, pointerEventsNormal) && ok;
    sendMouse(pointerEventsRuntime, skui::EventType::MouseMove, 20.0f, 20.0f);
    ok = renderPixel(pointerEventsRuntime, 20, 20, pointerEventsHover) && ok;
    sendMouse(pointerEventsRuntime, skui::EventType::MouseDown, 20.0f, 20.0f);
    sendMouse(pointerEventsRuntime, skui::EventType::MouseUp, 20.0f, 20.0f);
    ok = expect(pointerEventsNormal != pointerEventsHover, "pointer-events:none overlay should not block hover") && ok;
    ok = expect(pointerEventsClicks == 1, "pointer-events:none overlay should not block click") && ok;

    constexpr std::string_view selectorHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .box {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 60px;
      height: 50px;
    }
    .box .inner {
      position: absolute;
      left: 5px;
      top: 5px;
      width: 30px;
      height: 30px;
      background-color: #112233;
    }
    .inner {
      background-color: #334455;
    }
    .box > .inner:hover {
      background-color: #556677;
    }
    .inner[data-state=selected] {
      background-color: #778899;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="box">
      <div id="inner" class="inner"></div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime selectorRuntime(options);
    selectorRuntime.resize(kWidth, kHeight, 1.0f);
    if (!selectorRuntime.loadDocumentFromString(selectorHtml, "")) {
        std::cerr << "selector load failed: " << selectorRuntime.lastError() << "\n";
        return 1;
    }

    uint32_t selectorNormal = 0;
    uint32_t selectorHover = 0;
    uint32_t selectorAttribute = 0;
    uint32_t selectorInline = 0;
    ok = renderPixel(selectorRuntime, 20, 20, selectorNormal) && ok;
    sendMouse(selectorRuntime, skui::EventType::MouseMove, 20.0f, 20.0f);
    ok = renderPixel(selectorRuntime, 20, 20, selectorHover) && ok;
    sendMouse(selectorRuntime, skui::EventType::MouseLeave, 0.0f, 0.0f);
    ok = selectorRuntime.setAttributeById("inner", "data-state", "selected") && ok;
    ok = renderPixel(selectorRuntime, 20, 20, selectorAttribute) && ok;
    ok = selectorRuntime.setStyleById("inner", "background-color: #abcdef") && ok;
    ok = renderPixel(selectorRuntime, 20, 20, selectorInline) && ok;

    ok = expect(selectorNormal == solidColor(0x11, 0x22, 0x33), "descendant selector specificity should beat later class rule") && ok;
    ok = expect(selectorHover == solidColor(0x55, 0x66, 0x77), "child selector with :hover should apply") && ok;
    ok = expect(selectorAttribute == solidColor(0x77, 0x88, 0x99), "attribute selector should apply after setAttributeById") && ok;
    ok = expect(selectorInline == solidColor(0xAB, 0xCD, 0xEF), "setStyleById should update inline style") && ok;

    constexpr std::string_view textOverflowHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .button {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 110px;
      height: 44px;
      background-color: #112233;
      flex-direction: row;
      align-items: center;
      justify-content: center;
    }
    .label {
      width: 80px;
      height: 1px;
      color: #ffffff;
      font-size: 24px;
      font-weight: bold;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="button">
      <span class="label">TEXT</span>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime textOverflowRuntime(options);
    textOverflowRuntime.resize(kWidth, kHeight, 1.0f);
    if (!textOverflowRuntime.loadDocumentFromString(textOverflowHtml, "")) {
        std::cerr << "text overflow load failed: " << textOverflowRuntime.lastError() << "\n";
        return 1;
    }

    std::vector<uint32_t> textOverflowPixels;
    ok = renderPixels(textOverflowRuntime, textOverflowPixels) && ok;
    ok = expect(countBrightPixels(textOverflowPixels, 20, 12, 120, 30) > 20,
                "ordinary text should not be clipped by its own layout box") && ok;

    constexpr std::string_view inputHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .field {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 110px;
      height: 34px;
      background-color: #112233;
      color: #ffffff;
      font-size: 16px;
    }
    .field:focus {
      background-color: #334455;
    }
  </style>
</head>
<body>
  <div class="root">
    <input id="field" class="field" placeholder="search">
  </div>
</body>
</html>
)html";

    std::string clipboard;
    skui::RuntimeOptions inputOptions = options;
    inputOptions.readClipboardText = [&] { return clipboard; };
    inputOptions.writeClipboardText = [&](std::string_view text) { clipboard = std::string(text); };
    skui::Runtime inputRuntime(inputOptions);
    inputRuntime.resize(kWidth, kHeight, 1.0f);

    int inputEvents = 0;
    std::string inputValue;
    inputRuntime.setElementEventCallback([&](const skui::ElementEvent& event) {
        if (event.type == skui::ElementEventType::Input) {
            ++inputEvents;
            inputValue = event.value;
        }
    });

    if (!inputRuntime.loadDocumentFromString(inputHtml, "")) {
        std::cerr << "input load failed: " << inputRuntime.lastError() << "\n";
        return 1;
    }

    uint32_t inputNormal = 0;
    uint32_t inputFocused = 0;
    ok = renderPixel(inputRuntime, 20, 20, inputNormal) && ok;
    sendMouse(inputRuntime, skui::EventType::MouseDown, 20.0f, 20.0f);
    sendMouse(inputRuntime, skui::EventType::MouseUp, 20.0f, 20.0f);
    ok = renderPixel(inputRuntime, 20, 20, inputFocused) && ok;
    sendText(inputRuntime, "a");
    sendText(inputRuntime, "b");
    sendMouse(inputRuntime, skui::EventType::MouseDown, 11.0f, 20.0f, true);
    sendMouse(inputRuntime, skui::EventType::MouseUp, 11.0f, 20.0f, true);
    sendKey(inputRuntime, 'C', false, true);
    ok = expect(clipboard == "ab", "Shift+mouse should extend input selection from cursor to click position") && ok;
    sendMouse(inputRuntime, skui::EventType::MouseDown, 119.0f, 20.0f);
    sendMouse(inputRuntime, skui::EventType::MouseUp, 119.0f, 20.0f);
    sendKey(inputRuntime, 0x25);
    sendText(inputRuntime, "X");
    sendKey(inputRuntime, 0x08);
    sendKey(inputRuntime, 0x24);
    sendText(inputRuntime, ">");
    sendKey(inputRuntime, 0x23);
    sendText(inputRuntime, "<");
    sendMouse(inputRuntime, skui::EventType::MouseDown, 11.0f, 20.0f);
    sendMouse(inputRuntime, skui::EventType::MouseUp, 11.0f, 20.0f);
    sendText(inputRuntime, "^");
    sendMouse(inputRuntime, skui::EventType::MouseDown, 119.0f, 20.0f);
    sendMouse(inputRuntime, skui::EventType::MouseUp, 119.0f, 20.0f);
    sendText(inputRuntime, "$");
    sendMouse(inputRuntime, skui::EventType::MouseDown, 11.0f, 20.0f);
    sendMouse(inputRuntime, skui::EventType::MouseMove, 119.0f, 20.0f);
    sendMouse(inputRuntime, skui::EventType::MouseUp, 119.0f, 20.0f);
    sendText(inputRuntime, "all");
    sendKey(inputRuntime, 'A', false, true);
    sendKey(inputRuntime, 'C', false, true);
    ok = expect(clipboard == "all", "Ctrl+C should copy the selected input text") && ok;
    sendKey(inputRuntime, 'X', false, true);
    sendKey(inputRuntime, 'V', false, true);
    sendKey(inputRuntime, 0x25, true);
    sendKey(inputRuntime, 0x25, true);
    sendText(inputRuntime, "x");
    sendText(inputRuntime, " test");
    sendMouse(inputRuntime, skui::EventType::MouseDoubleClick, 50.0f, 20.0f);
    sendText(inputRuntime, "ok");
    sendIme(inputRuntime, skui::EventType::ImeComposition, "zhong");
    sendText(inputRuntime, "中");
    sendIme(inputRuntime, skui::EventType::ImeEnd);
    sendKey(inputRuntime, 'Z', false, true);
    ok = expect(inputValue == "ax ok", "Ctrl+Z should undo the previous single-line input edit") && ok;
    sendText(inputRuntime, "中");

    ok = expect(inputNormal != inputFocused, "input:focus should change rendered output") && ok;
    ok = expect(inputEvents == 17, "input text changes and undo should emit Input events") && ok;
    ok = expect(inputValue == "ax ok中", "single-line input should handle selection, clipboard, double-click, and IME commit") && ok;

    constexpr std::string_view selectableHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .normal {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 90px;
      height: 24px;
      color: #ffffff;
      font-size: 16px;
    }
    selectable {
      position: absolute;
      left: 10px;
      top: 45px;
      width: 90px;
      height: 24px;
      color: #ffffff;
      font-size: 16px;
    }
    .padded {
      position: absolute;
      left: 10px;
      top: 68px;
      width: 120px;
      height: 20px;
      padding-left: 18px;
      color: #ffffff;
      font-size: 16px;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="normal">plain copy</div>
    <selectable>hello copy</selectable>
    <selectable class="padded">能把最新的</selectable>
  </div>
</body>
</html>
)html";

    std::string selectableClipboard;
    skui::RuntimeOptions selectableOptions;
    selectableOptions.writeClipboardText = [&](std::string_view text) { selectableClipboard = std::string(text); };
    skui::Runtime selectableRuntime(selectableOptions);
    selectableRuntime.resize(kWidth, kHeight, 1.0f);
    if (!selectableRuntime.loadDocumentFromString(selectableHtml, "")) {
        std::cerr << "selectable load failed: " << selectableRuntime.lastError() << "\n";
        return 1;
    }
    sendMouse(selectableRuntime, skui::EventType::MouseDown, 10.0f, 20.0f);
    sendMouse(selectableRuntime, skui::EventType::MouseMove, 100.0f, 20.0f);
    sendMouse(selectableRuntime, skui::EventType::MouseUp, 100.0f, 20.0f);
    sendKey(selectableRuntime, 'C', false, true);
    ok = expect(selectableClipboard.empty(), "plain div text should not be selectable or copied") && ok;
    sendMouse(selectableRuntime, skui::EventType::MouseDown, 10.0f, 55.0f);
    sendMouse(selectableRuntime, skui::EventType::MouseMove, 100.0f, 55.0f);
    sendMouse(selectableRuntime, skui::EventType::MouseUp, 100.0f, 55.0f);
    sendKey(selectableRuntime, 'C', false, true);
    ok = expect(selectableClipboard == "hello copy", "selectable tag should support drag selection and Ctrl+C") && ok;
    sendMouse(selectableRuntime, skui::EventType::MouseDoubleClick, 78.0f, 78.0f);
    sendKey(selectableRuntime, 'C', false, true);
    ok = expect(selectableClipboard == "新", "selectable hit testing should account for text padding") && ok;

    constexpr std::string_view progressHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .bar {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 100px;
      height: 10px;
      background-color: #203040;
      color: #10c0b0;
    }
  </style>
</head>
<body>
  <div class="root">
    <progress id="bar" class="bar" value="40" max="100"></progress>
  </div>
</body>
</html>
)html";

    skui::Runtime progressRuntime(options);
    progressRuntime.resize(kWidth, kHeight, 1.0f);
    if (!progressRuntime.loadDocumentFromString(progressHtml, "")) {
        std::cerr << "progress load failed: " << progressRuntime.lastError() << "\n";
        return 1;
    }

    uint32_t progressFill = 0;
    uint32_t progressTrack = 0;
    ok = renderPixel(progressRuntime, 20, 15, progressFill) && ok;
    ok = renderPixel(progressRuntime, 80, 15, progressTrack) && ok;
    ok = expect(progressFill == solidColor(0x10, 0xC0, 0xB0), "progress value should render filled track") && ok;
    ok = expect(progressTrack == solidColor(0x20, 0x30, 0x40), "progress max range should leave unfilled track") && ok;

    constexpr std::string_view borderRadiusHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .corner-box {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 40px;
      height: 40px;
      background-color: #ABCDEF;
      border-radius: 14px 0 0 0;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="corner-box"></div>
  </div>
</body>
</html>
)html";
    skui::Runtime borderRadiusRuntime(options);
    borderRadiusRuntime.resize(kWidth, kHeight, 1.0f);
    if (!borderRadiusRuntime.loadDocumentFromString(borderRadiusHtml, "")) {
        std::cerr << "border radius load failed: " << borderRadiusRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t roundedTopLeft = 0;
    uint32_t squareTopRight = 0;
    uint32_t squareBottomLeft = 0;
    uint32_t squareBottomRight = 0;
    ok = renderPixel(borderRadiusRuntime, 12, 12, roundedTopLeft) && ok;
    ok = renderPixel(borderRadiusRuntime, 48, 12, squareTopRight) && ok;
    ok = renderPixel(borderRadiusRuntime, 12, 48, squareBottomLeft) && ok;
    ok = renderPixel(borderRadiusRuntime, 48, 48, squareBottomRight) && ok;
    ok = expect(roundedTopLeft == solidColor(0x00, 0x00, 0x00), "border-radius shorthand should round only the top-left corner") && ok;
    ok = expect(squareTopRight == solidColor(0xAB, 0xCD, 0xEF), "border-radius shorthand should keep the top-right corner square") && ok;
    ok = expect(squareBottomLeft == solidColor(0xAB, 0xCD, 0xEF), "border-radius shorthand should keep the bottom-left corner square") && ok;
    ok = expect(squareBottomRight == solidColor(0xAB, 0xCD, 0xEF), "border-radius shorthand should keep the bottom-right corner square") && ok;

    constexpr std::string_view dynamicDomHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .dock {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 140px;
      height: 90px;
    }
  </style>
</head>
<body>
  <div class="root">
    <div id="messages" class="dock"></div>
  </div>
</body>
</html>
)html";
    skui::Runtime dynamicDomRuntime(options);
    dynamicDomRuntime.resize(kWidth, kHeight, 1.0f);
    if (!dynamicDomRuntime.loadDocumentFromString(dynamicDomHtml, "")) {
        std::cerr << "dynamic DOM load failed: " << dynamicDomRuntime.lastError() << "\n";
        return 1;
    }

    uint32_t dynamicPixel = 0;
    ok = renderPixel(dynamicDomRuntime, 10, 10, dynamicPixel) && ok;
    ok = expect(dynamicPixel == solidColor(0x00, 0x00, 0x00),
                "empty dynamic container should render background") && ok;
    ok = expect(dynamicDomRuntime.appendHtmlById(
                    "messages",
                    R"html(<div id="first" style="position: absolute; left: 0px; top: 0px; width: 20px; height: 20px; background-color: #112233"></div>)html"),
                "appendHtmlById should append a new element") && ok;
    ok = renderPixel(dynamicDomRuntime, 10, 10, dynamicPixel) && ok;
    ok = expect(dynamicPixel == solidColor(0x11, 0x22, 0x33),
                "appended dynamic element should render") && ok;
    ok = expect(dynamicDomRuntime.appendHtmlById(
                    "messages",
                    R"html(<div id="last" style="position: absolute; left: 0px; top: 0px; width: 20px; height: 20px; background-color: #445566"></div>)html"),
                "appendHtmlById should draw appended siblings later") && ok;
    ok = renderPixel(dynamicDomRuntime, 10, 10, dynamicPixel) && ok;
    ok = expect(dynamicPixel == solidColor(0x44, 0x55, 0x66),
                "appended sibling should paint above earlier sibling") && ok;
    ok = expect(dynamicDomRuntime.prependHtmlById(
                    "messages",
                    R"html(<div id="prepended" style="position: absolute; left: 0px; top: 0px; width: 20px; height: 20px; background-color: #778899"></div>)html"),
                "prependHtmlById should insert before existing children") && ok;
    ok = renderPixel(dynamicDomRuntime, 10, 10, dynamicPixel) && ok;
    ok = expect(dynamicPixel == solidColor(0x44, 0x55, 0x66),
                "prepended sibling should stay behind later siblings") && ok;
    ok = expect(dynamicDomRuntime.replaceHtmlById(
                    "last",
                    R"html(<div id="replacement" style="position: absolute; left: 0px; top: 0px; width: 20px; height: 20px; background-color: #AABBCC"></div>)html"),
                "replaceHtmlById should replace an existing element") && ok;
    ok = renderPixel(dynamicDomRuntime, 10, 10, dynamicPixel) && ok;
    ok = expect(dynamicPixel == solidColor(0xAA, 0xBB, 0xCC),
                "replacement dynamic element should render") && ok;
    ok = expect(dynamicDomRuntime.setVisibleById("replacement", false),
                "setVisibleById should hide an element") && ok;
    ok = renderPixel(dynamicDomRuntime, 10, 10, dynamicPixel) && ok;
    ok = expect(dynamicPixel == solidColor(0x11, 0x22, 0x33),
                "hidden dynamic element should not render") && ok;
    ok = expect(dynamicDomRuntime.setVisibleById("replacement", true),
                "setVisibleById should show an element again") && ok;
    ok = renderPixel(dynamicDomRuntime, 10, 10, dynamicPixel) && ok;
    ok = expect(dynamicPixel == solidColor(0xAA, 0xBB, 0xCC),
                "shown dynamic element should render again") && ok;
    ok = expect(dynamicDomRuntime.removeElementById("replacement"),
                "removeElementById should remove an element") && ok;
    ok = renderPixel(dynamicDomRuntime, 10, 10, dynamicPixel) && ok;
    ok = expect(dynamicPixel == solidColor(0x11, 0x22, 0x33),
                "removed dynamic element should no longer render") && ok;

    constexpr std::string_view visibilityHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .stack {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 140px;
      flex-direction: column;
    }
    .slot {
      width: 40px;
      height: 20px;
      flex-shrink: 0;
    }
    .hidden-slot {
      visibility: hidden;
      background-color: #112233;
    }
    .child-fill {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 40px;
      height: 20px;
      background-color: #445566;
    }
    .after-slot {
      background-color: #778899;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="stack">
      <div id="hidden-slot" class="slot hidden-slot">
        <div class="child-fill"></div>
      </div>
      <div id="after-slot" class="slot after-slot"></div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime visibilityRuntime(options);
    visibilityRuntime.resize(kWidth, kHeight, 1.0f);
    if (!visibilityRuntime.loadDocumentFromString(visibilityHtml, "")) {
        std::cerr << "visibility load failed: " << visibilityRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t hiddenSlotPixel = 0;
    uint32_t preservedLayoutPixel = 0;
    ok = renderPixel(visibilityRuntime, 10, 10, hiddenSlotPixel) && ok;
    ok = renderPixel(visibilityRuntime, 10, 30, preservedLayoutPixel) && ok;
    ok = expect(hiddenSlotPixel == solidColor(0x00, 0x00, 0x00),
                "visibility:hidden should hide descendants") && ok;
    ok = expect(preservedLayoutPixel == solidColor(0x77, 0x88, 0x99),
                "visibility:hidden should preserve layout space") && ok;

    const std::filesystem::path imageFixtureDir = std::filesystem::temp_directory_path() / "skui-image-test";
    std::error_code ec;
    std::filesystem::create_directories(imageFixtureDir, ec);

    const auto imageHtmlForSource = [](std::string_view source) {
        std::string html = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .photo {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 20px;
      height: 20px;
    }
  </style>
</head>
<body>
  <div class="root">
    <img class="photo" src=")html";
        html += source;
        html += R"html(">
  </div>
</body>
</html>
)html";
        return html;
    };

    struct ImageFixture {
        const char* fileName = nullptr;
        bool (*write)(const std::filesystem::path&) = nullptr;
    };
    const ImageFixture imageFixtures[] = {
        {"red.bmp", writeRedBmpFixture},
        {"red.png", writeRedPngFixture},
        {"red.jpg", writeRedJpegFixture},
        {"red.webp", writeRedWebpFixture},
    };

    for (const ImageFixture& fixture : imageFixtures) {
        const std::filesystem::path imageFixturePath = imageFixtureDir / fixture.fileName;
        ok = expect(fixture.write(imageFixturePath),
                    std::string("bitmap test fixture should be written: ") + fixture.fileName) && ok;

        std::atomic_int imageRedraws{0};
        skui::RuntimeOptions imageOptions = options;
        imageOptions.requestRedraw = [&] {
            imageRedraws.fetch_add(1);
        };
        skui::Runtime imageRuntime(imageOptions);
        imageRuntime.resize(kWidth, kHeight, 1.0f);
        const std::string imageHtml = imageHtmlForSource(fixture.fileName);
        if (!imageRuntime.loadDocumentFromString(imageHtml, imageFixtureDir.string())) {
            std::cerr << "image load failed: " << imageRuntime.lastError() << "\n";
            return 1;
        }
        uint32_t imageBefore = 0;
        uint32_t imageAfter = 0;
        ok = renderPixel(imageRuntime, 20, 20, imageBefore) && ok;
        waitForDirty(imageRuntime);
        ok = renderPixel(imageRuntime, 20, 20, imageAfter) && ok;
        ok = expect(imageBefore == solidColor(0x00, 0x00, 0x00),
                    std::string("async bitmap image should not block the first render: ") + fixture.fileName) && ok;
        ok = expect(imageRedraws.load() > 0,
                    std::string("async bitmap image load should request redraw: ") + fixture.fileName) && ok;
        ok = expect(isMostlyRed(imageAfter),
                    std::string("async bitmap image should render after loading: ") + fixture.fileName) && ok;
    }

    {
        constexpr int imageCount = 12;
        constexpr int imageSize = 10;
        constexpr int imageGap = 2;
        std::string imageGridHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .photo {
      position: absolute;
      width: 10px;
      height: 10px;
    }
  </style>
</head>
<body>
  <div class="root">
)html";

        for (int i = 0; i < imageCount; ++i) {
            const std::string fileName = "parallel-" + std::to_string(i) + ".png";
            ok = expect(writeRedPngFixture(imageFixtureDir / fileName),
                        "parallel bitmap fixture should be written") && ok;
            imageGridHtml += "<img class=\"photo\" src=\"";
            imageGridHtml += fileName;
            imageGridHtml += "\" style=\"left: ";
            imageGridHtml += std::to_string(i * (imageSize + imageGap));
            imageGridHtml += "px; top: 0px;\">\n";
        }
        imageGridHtml += R"html(
  </div>
</body>
</html>
)html";

        std::atomic_int parallelImageRedraws{0};
        skui::RuntimeOptions parallelImageOptions = options;
        parallelImageOptions.bitmapLoadWorkerCount = 4;
        parallelImageOptions.requestRedraw = [&] {
            parallelImageRedraws.fetch_add(1);
        };
        skui::Runtime parallelImageRuntime(parallelImageOptions);
        parallelImageRuntime.resize(kWidth, kHeight, 1.0f);
        if (!parallelImageRuntime.loadDocumentFromString(imageGridHtml, imageFixtureDir.string())) {
            std::cerr << "parallel image load failed: "
                      << parallelImageRuntime.lastError() << "\n";
            return 1;
        }

        std::vector<uint32_t> parallelPixels;
        ok = renderPixels(parallelImageRuntime, parallelPixels) && ok;
        for (int i = 0; i < imageCount; ++i) {
            const int x = i * (imageSize + imageGap) + imageSize / 2;
            ok = expect(pixelAt(parallelPixels, x, imageSize / 2) ==
                            solidColor(0x00, 0x00, 0x00),
                        "parallel bitmap load should not block the first render") && ok;
        }

        bool allImagesReady = false;
        for (int retry = 0; retry < 100 && !allImagesReady; ++retry) {
            waitForDirty(parallelImageRuntime);
            ok = renderPixels(parallelImageRuntime, parallelPixels) && ok;
            allImagesReady = true;
            for (int i = 0; i < imageCount; ++i) {
                const int x = i * (imageSize + imageGap) + imageSize / 2;
                if (!isMostlyRed(pixelAt(parallelPixels, x, imageSize / 2))) {
                    allImagesReady = false;
                    break;
                }
            }
        }
        ok = expect(parallelImageRedraws.load() >= imageCount,
                    "parallel bitmap image loads should request redraws") && ok;
        ok = expect(allImagesReady,
                    "parallel bitmap images should all render after async loading") && ok;
    }

    {
        const std::filesystem::path unicodeImageDir = imageFixtureDir / std::filesystem::path(L"中文目录");
        std::filesystem::create_directories(unicodeImageDir, ec);
        const std::filesystem::path unicodeImagePath = unicodeImageDir / std::filesystem::path(L"红色图片.png");
        ok = expect(writeRedPngFixture(unicodeImagePath), "unicode bitmap test fixture should be written") && ok;

        std::atomic_int unicodeImageRedraws{0};
        skui::RuntimeOptions unicodeImageOptions = options;
        unicodeImageOptions.requestRedraw = [&] {
            unicodeImageRedraws.fetch_add(1);
        };
        skui::Runtime unicodeImageRuntime(unicodeImageOptions);
        unicodeImageRuntime.resize(kWidth, kHeight, 1.0f);
        if (!unicodeImageRuntime.loadDocumentFromString(imageHtmlForSource("红色图片.png"),
                                                        utf8PathText(unicodeImageDir))) {
            std::cerr << "unicode image load failed: " << unicodeImageRuntime.lastError() << "\n";
            return 1;
        }
        uint32_t unicodeImageBefore = 0;
        uint32_t unicodeImageAfter = 0;
        ok = renderPixel(unicodeImageRuntime, 20, 20, unicodeImageBefore) && ok;
        waitForDirty(unicodeImageRuntime);
        ok = renderPixel(unicodeImageRuntime, 20, 20, unicodeImageAfter) && ok;
        ok = expect(unicodeImageBefore == solidColor(0x00, 0x00, 0x00),
                    "unicode bitmap image should not block the first render") && ok;
        ok = expect(unicodeImageRedraws.load() > 0,
                    "unicode bitmap image load should request redraw") && ok;
        ok = expect(isMostlyRed(unicodeImageAfter),
                    "unicode bitmap image should render after loading") && ok;
    }

    {
        const std::filesystem::path cacheDir = std::filesystem::temp_directory_path() / "skui_bitmap_cache_budget";
        std::filesystem::create_directories(cacheDir);
        const std::filesystem::path redPath = cacheDir / "red.png";
        const std::filesystem::path bluePath = cacheDir / "blue.png";
        ok = expect(writeRedPngFixture(redPath), "red bitmap budget fixture should be written") && ok;
        ok = expect(writeBluePngFixture(bluePath), "blue bitmap budget fixture should be written") && ok;

        constexpr std::string_view budgetHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .photo {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 40px;
      height: 40px;
    }
  </style>
</head>
<body>
  <div class="root"><img id="photo" class="photo" src="red.png"></div>
</body>
</html>
)html";

        skui::RuntimeOptions budgetOptions = options;
        budgetOptions.assetRoot = utf8PathText(cacheDir);
        budgetOptions.bitmapCacheBudgetBytes = 16;
        skui::Runtime budgetRuntime(budgetOptions);
        budgetRuntime.resize(kWidth, kHeight, 1.0f);
        ok = expect(budgetRuntime.loadDocumentFromString(budgetHtml, utf8PathText(cacheDir)),
                    "bitmap cache budget document should load") && ok;

        uint32_t budgetPixel = 0;
        ok = renderPixel(budgetRuntime, 20, 20, budgetPixel) && ok;
        ok = expect(budgetPixel == solidColor(0x00, 0x00, 0x00),
                    "budgeted bitmap should not block the first red render") && ok;
        waitForDirty(budgetRuntime);
        ok = renderPixel(budgetRuntime, 20, 20, budgetPixel) && ok;
        ok = expect(isMostlyRed(budgetPixel), "red bitmap should render after async load") && ok;

        ok = expect(budgetRuntime.setAttributeById("photo", "src", "blue.png"),
                    "bitmap src should switch to blue") && ok;
        ok = renderPixel(budgetRuntime, 20, 20, budgetPixel) && ok;
        ok = expect(budgetPixel == solidColor(0x00, 0x00, 0x00),
                    "budgeted bitmap should not block the first blue render") && ok;
        waitForDirty(budgetRuntime);
        ok = renderPixel(budgetRuntime, 20, 20, budgetPixel) && ok;
        ok = expect(isMostlyBlue(budgetPixel), "blue bitmap should render after async load") && ok;

        ok = expect(budgetRuntime.setAttributeById("photo", "src", "red.png"),
                    "bitmap src should switch back to red") && ok;
        ok = renderPixel(budgetRuntime, 20, 20, budgetPixel) && ok;
        ok = expect(budgetPixel == solidColor(0x00, 0x00, 0x00),
                    "evicted red bitmap should be loaded again after switching back") && ok;
        waitForDirty(budgetRuntime);
        ok = renderPixel(budgetRuntime, 20, 20, budgetPixel) && ok;
        ok = expect(isMostlyRed(budgetPixel), "evicted red bitmap should render after reload") && ok;

        std::filesystem::remove_all(cacheDir);
    }

    {
        constexpr std::string_view imageScrollerShellHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .screen {
      flex-grow: 1;
      position: relative;
      background-color: #07111c;
      color: #eef6fc;
      font-size: 16px;
    }
    .toolbar {
      position: absolute;
      left: 8px;
      top: 8px;
      right: 8px;
      height: 30px;
      background-color: #0e1c2b;
      border-color: #80a4c0;
      border-width: 1px;
      border-style: solid;
      border-radius: 4px;
    }
    .title {
      position: absolute;
      left: 6px;
      top: 4px;
      width: 100px;
      height: 18px;
      font-size: 14px;
    }
    .viewer {
      position: absolute;
      left: 8px;
      top: 46px;
      right: 8px;
      bottom: 8px;
      overflow-y: auto;
      background-color: #040b12;
    }
  </style>
</head>
<body>
  <div class="screen">
    <div class="toolbar"><div class="title">Images</div></div>
    <div class="viewer"></div>
  </div>
</body>
</html>
)html";

        skui::Runtime imageScrollerShellRuntime(options);
        imageScrollerShellRuntime.resize(kWidth, kHeight, 1.0f);
        if (!imageScrollerShellRuntime.loadDocumentFromString(imageScrollerShellHtml, "")) {
            std::cerr << "image scroller shell load failed: "
                      << imageScrollerShellRuntime.lastError() << "\n";
            return 1;
        }

        uint32_t toolbarPixel = 0;
        uint32_t viewerPixel = 0;
        ok = renderPixel(imageScrollerShellRuntime, 12, 12, toolbarPixel) && ok;
        ok = renderPixel(imageScrollerShellRuntime, 12, 50, viewerPixel) && ok;
        ok = expect(toolbarPixel != solidColor(0x00, 0x00, 0x00),
                    "image scroller shell toolbar should render on the first frame") && ok;
        ok = expect(viewerPixel != solidColor(0x00, 0x00, 0x00),
                    "image scroller shell viewer should render on the first frame") && ok;
    }

    {
        const std::filesystem::path relayDocument =
            std::filesystem::current_path() / "assets" / "skui_relay_demo" / "relaydesk.html";
        skui::RuntimeOptions relayOptions;
        relayOptions.assetRoot = utf8PathText(relayDocument.parent_path());
        relayOptions.clearColor = SkColorSetRGB(248, 250, 252);
        skui::Runtime relayRuntime(relayOptions);
        relayRuntime.resize(1100, 760, 1.0f);
        ok = expect(relayRuntime.loadDocument(utf8PathText(relayDocument)),
                    "relay demo document should load") && ok;

        constexpr int relayWidth = 1100;
        constexpr int relayHeight = 760;
        std::vector<uint32_t> relayPixels(static_cast<size_t>(relayWidth) *
                                              static_cast<size_t>(relayHeight),
                                          solidColor(248, 250, 252));
        ok = expect(relayRuntime.renderToBgraPixels(relayPixels.data(),
                                                    relayWidth,
                                                    relayHeight,
                                                    static_cast<size_t>(relayWidth) * sizeof(uint32_t),
                                                    1.0f),
                    "relay demo document should render through renderToBgraPixels") && ok;
        ok = expect(pixelAt(relayPixels, relayWidth, 48, 48) != solidColor(248, 250, 252),
                    "relay demo first frame should draw visible content") && ok;
    }

    constexpr std::string_view responsiveHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 100%;
      height: 90px;
      background-color: #000000;
    }
    .half {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 50%;
      height: 30px;
      background-color: #224466;
    }
    .media-box {
      position: absolute;
      left: 0px;
      top: 40px;
      width: 100%;
      height: 30px;
      background-color: #111111;
    }
    @media (max-width: 100px) {
      .media-box {
        background-color: #abcdef;
      }
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="half"></div>
    <div class="media-box"></div>
  </div>
</body>
</html>
)html";

    skui::Runtime responsiveRuntime(options);
    responsiveRuntime.resize(140, 90, 1.0f);
    if (!responsiveRuntime.loadDocumentFromString(responsiveHtml, "")) {
        std::cerr << "responsive load failed: " << responsiveRuntime.lastError() << "\n";
        return 1;
    }

    uint32_t halfInside = 0;
    uint32_t halfOutside = 0;
    uint32_t mediaWide = 0;
    uint32_t mediaNarrow = 0;
    ok = renderPixelAt(responsiveRuntime, 140, 90, 60, 15, halfInside) && ok;
    ok = renderPixelAt(responsiveRuntime, 140, 90, 90, 15, halfOutside) && ok;
    ok = renderPixelAt(responsiveRuntime, 140, 90, 20, 55, mediaWide) && ok;
    responsiveRuntime.resize(80, 90, 1.0f);
    ok = renderPixelAt(responsiveRuntime, 80, 90, 20, 55, mediaNarrow) && ok;
    ok = expect(halfInside == solidColor(0x22, 0x44, 0x66), "percentage width should resolve against parent width") && ok;
    ok = expect(halfOutside == solidColor(0x00, 0x00, 0x00), "percentage width should leave remaining parent area") && ok;
    ok = expect(mediaWide == solidColor(0x11, 0x11, 0x11), "media query should not match wide viewport") && ok;
    ok = expect(mediaNarrow == solidColor(0xAB, 0xCD, 0xEF), "media query should recompute after resize") && ok;

    constexpr std::string_view cursorHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .resize {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 20px;
      height: 60px;
      cursor: ew-resize;
      background-color: #223344;
    }
    .pointer {
      position: absolute;
      left: 50px;
      top: 10px;
      width: 30px;
      height: 30px;
      cursor: pointer;
      background-color: #445566;
    }
    .child {
      width: 20px;
      height: 20px;
      background-color: #667788;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="resize"></div>
    <div class="pointer">
      <div class="child"></div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime cursorRuntime(options);
    cursorRuntime.resize(kWidth, kHeight, 1.0f);
    if (!cursorRuntime.loadDocumentFromString(cursorHtml, "")) {
        std::cerr << "cursor load failed: " << cursorRuntime.lastError() << "\n";
        return 1;
    }
    sendMouse(cursorRuntime, skui::EventType::MouseMove, 15.0f, 20.0f);
    ok = expect(cursorRuntime.cursor() == skui::Cursor::EWResize, "cursor property should expose resize cursor on hover") && ok;
    sendMouse(cursorRuntime, skui::EventType::MouseMove, 55.0f, 15.0f);
    ok = expect(cursorRuntime.cursor() == skui::Cursor::Pointer, "cursor property should inherit to child hit targets") && ok;
    sendMouse(cursorRuntime, skui::EventType::MouseMove, 120.0f, 80.0f);
    ok = expect(cursorRuntime.cursor() == skui::Cursor::Default, "cursor should fall back to default outside styled nodes") && ok;

    constexpr std::string_view batchStyleHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .a {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 10px;
      height: 10px;
      background-color: #111111;
    }
    .b {
      position: absolute;
      left: 20px;
      top: 0px;
      width: 10px;
      height: 10px;
      background-color: #222222;
    }
  </style>
</head>
<body>
  <div class="root">
    <div id="box-a" class="a"></div>
    <div id="box-b" class="b"></div>
  </div>
</body>
</html>
)html";

    skui::Runtime batchRuntime(options);
    batchRuntime.resize(kWidth, kHeight, 1.0f);
    if (!batchRuntime.loadDocumentFromString(batchStyleHtml, "")) {
        std::cerr << "batch style load failed: " << batchRuntime.lastError() << "\n";
        return 1;
    }
    ok = expect(batchRuntime.setStylesById({{"box-a", "width:30px;height:20px;background-color:#abcdef;"},
                                            {"box-b", "left:60px;width:30px;height:20px;background-color:#778899;"}}),
                "batch style update should report success when at least one id is updated") && ok;
    uint32_t batchA = 0;
    uint32_t batchB = 0;
    ok = renderPixel(batchRuntime, 25, 10, batchA) && ok;
    ok = renderPixel(batchRuntime, 65, 10, batchB) && ok;
    ok = expect(batchA == solidColor(0xAB, 0xCD, 0xEF), "batch style update should apply first node style") && ok;
    ok = expect(batchB == solidColor(0x77, 0x88, 0x99), "batch style update should apply second node style") && ok;

    constexpr std::string_view dropdownHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .hidden {
      display: none;
    }
    .option-selected {
      color: #00ff00;
    }
  </style>
</head>
<body>
  <div class="root">
    <div id="selected">A</div>
    <div id="arrow">v</div>
    <div id="backdrop" class="hidden"></div>
    <div id="menu" class="hidden">
      <div id="option-0" class="option option-selected">A</div>
      <div id="option-1" class="option">B</div>
      <div id="option-2" class="option">C</div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime dropdownRuntime(options);
    dropdownRuntime.resize(kWidth, kHeight, 1.0f);
    if (!dropdownRuntime.loadDocumentFromString(dropdownHtml, "")) {
        std::cerr << "dropdown load failed: " << dropdownRuntime.lastError() << "\n";
        return 1;
    }
    skui::DropdownState dropdown({
        "selected",
        "arrow",
        "menu",
        "backdrop",
        "option-",
        "hidden",
        "option-selected",
        "^",
        "v",
        3,
    });
    dropdown.setOpen(dropdownRuntime, true);
    ok = expect(dropdown.open(), "dropdown should report open state") && ok;
    ok = expect(!dropdownRuntime.hasClassById("menu", "hidden"),
                "dropdown open should show menu") && ok;
    ok = expect(!dropdownRuntime.hasClassById("backdrop", "hidden"),
                "dropdown open should show backdrop") && ok;
    ok = expect(dropdown.select(dropdownRuntime, 2, "C"), "dropdown should select valid option") && ok;
    ok = expect(dropdown.selectedIndex() == 2, "dropdown should store selected index") && ok;
    ok = expect(dropdownRuntime.hasClassById("menu", "hidden"),
                "dropdown select should close menu") && ok;
    ok = expect(dropdownRuntime.hasClassById("option-2", "option-selected"),
                "dropdown select should mark selected option") && ok;
    ok = expect(!dropdownRuntime.hasClassById("option-0", "option-selected"),
                "dropdown select should clear previous option") && ok;
    ok = expect(!dropdown.select(dropdownRuntime, 9, "invalid"),
                "dropdown should reject out-of-range options") && ok;

    constexpr std::string_view virtualScrollHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .virtual-list {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 70px;
      height: 40px;
      overflow-y: auto;
      overflow-x: auto;
      background-color: #111111;
    }
    .row {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 50px;
      height: 20px;
      background-color: #223344;
      color: #ffffff;
      font-size: 12px;
    }
    .sentinel {
      position: absolute;
      left: 0px;
      top: 80px;
      width: 50px;
      height: 20px;
      background-color: #abcdef;
    }
  </style>
</head>
<body>
  <div class="root">
    <div id="virtual-list" class="virtual-list" data-action="virtual-scroll" data-virtual-width="400" data-virtual-height="1000">
      <div id="pool-row" class="row">row 0</div>
    </div>
    <div id="label-a" class="row" style="left:90px;top:10px;background-color:#111111">a</div>
    <div id="label-b" class="row" style="left:90px;top:40px;background-color:#111111">b</div>
  </div>
</body>
</html>
)html";

    skui::Runtime virtualRuntime(options);
    virtualRuntime.resize(kWidth, kHeight, 1.0f);
    float virtualScrollY = -1.0f;
    float virtualScrollX = -1.0f;
    int scrollEvents = 0;
    virtualRuntime.setElementEventCallback([&](const skui::ElementEvent& event) {
        if (event.type == skui::ElementEventType::Scroll && event.action == "virtual-scroll") {
            virtualScrollX = event.scrollX;
            virtualScrollY = event.scrollY;
            ++scrollEvents;
        }
    });
    if (!virtualRuntime.loadDocumentFromString(virtualScrollHtml, "")) {
        std::cerr << "virtual scroll load failed: " << virtualRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t virtualScrollbar = 0;
    ok = renderPixel(virtualRuntime, 75, 25, virtualScrollbar) && ok;
    ok = expect(virtualScrollbar == solidColor(0x44, 0xDC, 0xD2), "virtual content height should create a scrollbar without real child height") && ok;
    sendWheel(virtualRuntime, 20.0f, 20.0f, -240.0f);
    ok = expect(scrollEvents == 1, "virtual scroll container should emit a scroll event") && ok;
    ok = expect(virtualScrollY > 0.0f, "scroll event should expose vertical scroll offset") && ok;
    sendWheel(virtualRuntime, 20.0f, 20.0f, -240.0f, true);
    ok = expect(scrollEvents == 2, "virtual horizontal scroll should emit a scroll event") && ok;
    ok = expect(virtualScrollX > 0.0f, "scroll event should expose horizontal scroll offset") && ok;
    ok = expect(virtualRuntime.setTextsById({{"label-a", "updated A"}, {"label-b", "updated B"}}),
                "batch text update should report success") && ok;
    ok = expect(virtualRuntime.setAttributesById({{"virtual-list", "data-virtual-height", "1600"}}),
                "batch attribute update should report success for virtual dimensions") && ok;
    ok = expect(virtualRuntime.setStyleById("pool-row", "position:absolute;left:0px;top:5000px;width:50px;height:20px;background-color:#223344;color:#ffffff;font-size:12px;"),
                "moving a pooled row should report success") && ok;
    ok = expect(virtualRuntime.setAttributesById({{"virtual-list", "data-virtual-height", "1000"}}),
                "resetting virtual height should report success") && ok;
    sendWheel(virtualRuntime, 20.0f, 20.0f, -24000.0f);
    ok = expect(virtualScrollY <= 960.0f, "pooled row position should not expand virtual scroll range") && ok;

    skui::VirtualTableGeometry tableGeometry("row-",
                                             "cell-",
                                             "header-",
                                             {{"id", 40}, {"name", 70}, {"type", 50}},
                                             24,
                                             18,
                                             22);
    ok = expect(tableGeometry.contentWidth() == 184, "virtual table geometry should include row gutter width") && ok;
    ok = expect(tableGeometry.contentHeight(10) == 238, "virtual table geometry should include header and row heights") && ok;
    ok = expect(tableGeometry.cellId(2, "name") == "cell-3-name", "virtual table geometry should build pooled cell ids") && ok;

    skui::VirtualWindowState windowState({100, tableGeometry.rowHeight(), tableGeometry.headerHeight(), 8, 3, 1});
    const skui::VirtualWindowFrame windowFrame = windowState.update(44.0f, 88);
    ok = expect(windowFrame.firstItem == 2, "virtual window should calculate first visible item from scroll offset") && ok;
    ok = expect(tableGeometry.rowTop(windowFrame, 0) == 62, "virtual table row top should include header offset") && ok;

    constexpr std::string_view tableAdapterHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 180px;
      height: 120px;
      background-color: #000000;
    }
    .viewport {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 130px;
      height: 70px;
      overflow-y: auto;
      overflow-x: auto;
      background-color: #111111;
    }
    .header,
    .row-bg,
    .row-handle,
    .cell {
      position: absolute;
      height: 20px;
    }
    .header,
    .row-bg {
      background-color: #223344;
    }
    .row-selected {
      background-color: #008877;
    }
    .cell {
      display: flex;
      align-items: center;
      color: #ffffff;
      font-size: 12px;
    }
    .cell-selected {
      background-color: #005555;
    }
  </style>
</head>
<body>
  <div class="root">
    <div id="adapter-table" class="viewport">
      <div id="adapter-handle-header" class="header" style="left:0px;top:0px;width:20px"></div>
      <div id="adapter-header-id" class="header" style="left:20px;top:0px;width:40px"></div>
      <div id="adapter-header-name" class="header" style="left:60px;top:0px;width:60px"></div>
      <div id="adapter-row-1-bg" class="row-bg" style="left:0px;top:20px;width:120px"></div>
      <div id="adapter-row-1-handle" class="row-handle" style="left:0px;top:20px;width:20px"></div>
      <div id="adapter-cell-1-id" class="cell" style="left:20px;top:20px;width:40px"></div>
      <div id="adapter-cell-1-name" class="cell" style="left:60px;top:20px;width:60px"></div>
      <div id="adapter-row-2-bg" class="row-bg" style="left:0px;top:40px;width:120px"></div>
      <div id="adapter-row-2-handle" class="row-handle" style="left:0px;top:40px;width:20px"></div>
      <div id="adapter-cell-2-id" class="cell" style="left:20px;top:40px;width:40px"></div>
      <div id="adapter-cell-2-name" class="cell" style="left:60px;top:40px;width:60px"></div>
      <div id="adapter-row-3-bg" class="row-bg" style="left:0px;top:60px;width:120px"></div>
      <div id="adapter-row-3-handle" class="row-handle" style="left:0px;top:60px;width:20px"></div>
      <div id="adapter-cell-3-id" class="cell" style="left:20px;top:60px;width:40px"></div>
      <div id="adapter-cell-3-name" class="cell" style="left:60px;top:60px;width:60px"></div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime tableAdapterRuntime(options);
    tableAdapterRuntime.resize(180, 120, 1.0f);
    ok = expect(tableAdapterRuntime.loadDocumentFromString(tableAdapterHtml),
                "virtual table adapter document should load") && ok;
    skui::VirtualTableGeometry adapterGeometry("adapter-row-",
                                               "adapter-cell-",
                                               "adapter-header-",
                                               {{"id", 40}, {"name", 60}},
                                               20,
                                               20,
                                               20);
    skui::VirtualTableRenderConfig adapterRenderConfig{
        "adapter-table",
        "adapter-handle-header",
        "row-bg",
        "row-selected",
        "row:",
        "handle:",
        "cell",
        "cell-selected",
        "col-",
    };
    skui::VirtualTableAdapter adapter(adapterGeometry,
                                      {20, adapterGeometry.rowHeight(), adapterGeometry.headerHeight(), 3, 3, 0},
                                      adapterRenderConfig);
    skui::VirtualTableDataSource adapterData;
    adapterData.itemCount = 20;
    adapterData.row = [](const skui::VirtualTableRowContext& context) {
        skui::VirtualTableRowData row;
        row.selected = context.rowIndex == 2;
        return row;
    };
    adapterData.cell = [](const skui::VirtualTableCellContext& context) {
        skui::VirtualTableCellData cell;
        cell.text = std::string(context.columnId) + "-" + std::to_string(context.rowIndex);
        cell.action = "cell:" + std::to_string(context.rowIndex) + ":" + std::string(context.columnId);
        cell.selected = context.rowIndex == 2 && context.columnId == "name";
        return cell;
    };
    ok = expect(adapter.refresh(tableAdapterRuntime, 40.0f, 70, adapterData, true),
                "virtual table adapter should render from a data source") && ok;
    ok = expect(adapter.firstItem() == 2,
                "virtual table adapter should expose the current first row") && ok;
    ok = expect(tableAdapterRuntime.hasClassById("adapter-row-1-bg", "row-selected"),
                "virtual table adapter should apply data-source row selection") && ok;
    ok = expect(tableAdapterRuntime.hasClassById("adapter-cell-1-name", "cell-selected"),
                "virtual table adapter should apply data-source cell selection") && ok;
    std::string adapterAction;
    tableAdapterRuntime.setElementEventCallback([&](const skui::ElementEvent& event) {
        if (event.type == skui::ElementEventType::Click) {
            adapterAction = event.action;
        }
    });
    sendMouse(tableAdapterRuntime, skui::EventType::MouseDown, 72.0f, 72.0f);
    sendMouse(tableAdapterRuntime, skui::EventType::MouseUp, 72.0f, 72.0f);
    ok = expect(adapterAction == "cell:2:name",
                "virtual table adapter should route data-source cell action") && ok;

    skui::VirtualTablePanelConfig panelConfig;
    panelConfig.panelLeft = 10;
    panelConfig.minPanelWidth = 60;
    panelConfig.rightGap = 5;
    panelConfig.contentInset = 4;
    panelConfig.minContentWidth = 40;
    panelConfig.toolbarExtraWidth = 2;
    panelConfig.toolbarBaseHeight = 20;
    panelConfig.tableBaseTop = 30;
    panelConfig.tableMinHeight = 15;
    panelConfig.tableBottomGap = 6;
    panelConfig.reservedBottomHeight = 8;
    panelConfig.toolbar = {{30, 30, 30}, 20, 5};
    skui::VirtualTablePanelLayout panelLayout(panelConfig, 90);
    skui::Runtime panelRuntime(options);
    panelRuntime.resize(120, 100, 1.0f);
    ok = expect(panelLayout.update(panelRuntime, 200), "virtual table panel should request initial style render") && ok;
    const skui::VirtualTablePanelFrame firstPanelFrame = panelLayout.frame();
    ok = expect(firstPanelFrame.panelWidth == 105, "virtual table panel should clamp width to viewport") && ok;
    ok = expect(firstPanelFrame.toolbarHeight == 45, "virtual table panel should wrap toolbar controls") && ok;
    ok = expect(firstPanelFrame.tableHeight == 31, "virtual table panel should use remaining table height") && ok;
    panelLayout.markRendered();
    panelRuntime.resize(120, 220, 1.0f);
    ok = expect(!panelLayout.update(panelRuntime, 200), "table height-only resize should not request panel style updates") && ok;
    ok = expect(panelLayout.frame().tableHeight > firstPanelFrame.tableHeight,
                "table height-only resize should still update viewport height for virtual rows") && ok;

    constexpr std::string_view flexWrapHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .toolbar {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 64px;
      height: 90px;
      flex-direction: row;
      flex-wrap: wrap;
      align-items: flex-start;
    }
    .tool {
      width: 40px;
      height: 20px;
      margin-right: 8px;
      margin-bottom: 8px;
      background-color: #222222;
    }
    .a {
      background-color: #ff0000;
    }
    .b {
      background-color: #00ff00;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="toolbar">
      <div class="tool a"></div>
      <div class="tool b"></div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime flexWrapRuntime(options);
    flexWrapRuntime.resize(kWidth, kHeight, 1.0f);
    if (!flexWrapRuntime.loadDocumentFromString(flexWrapHtml, "")) {
        std::cerr << "flex wrap load failed: " << flexWrapRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t firstRowProbe = 0;
    uint32_t secondRowProbe = 0;
    ok = renderPixel(flexWrapRuntime, 20, 18, firstRowProbe) && ok;
    ok = renderPixel(flexWrapRuntime, 20, 46, secondRowProbe) && ok;
    ok = expect(firstRowProbe == solidColor(0xFF, 0x00, 0x00), "flex-wrap should keep first child on first row") && ok;
    ok = expect(secondRowProbe == solidColor(0x00, 0xFF, 0x00), "flex-wrap should move overflowing child to next row") && ok;

    constexpr std::string_view scrollHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .vertical {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 50px;
      height: 40px;
      overflow-y: auto;
      overflow-x: hidden;
      background-color: #111111;
    }
    .top {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 50px;
      height: 20px;
      background-color: #223344;
    }
    .bottom {
      position: absolute;
      left: 0px;
      top: 60px;
      width: 50px;
      height: 40px;
      background-color: #abcdef;
    }
    .horizontal {
      position: absolute;
      left: 80px;
      top: 10px;
      width: 40px;
      height: 40px;
      overflow-x: auto;
      overflow-y: hidden;
      background-color: #111111;
    }
    .wide {
      position: absolute;
      left: 70px;
      top: 0px;
      width: 40px;
      height: 40px;
      background-color: #778899;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="vertical">
      <div class="top"></div>
      <div class="bottom"></div>
    </div>
    <div class="horizontal">
      <div class="wide"></div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime scrollRuntime(options);
    scrollRuntime.resize(kWidth, kHeight, 1.0f);
    if (!scrollRuntime.loadDocumentFromString(scrollHtml, "")) {
        std::cerr << "scroll load failed: " << scrollRuntime.lastError() << "\n";
        return 1;
    }

    uint32_t verticalInitial = 0;
    uint32_t verticalClipped = 0;
    uint32_t verticalScrolled = 0;
    uint32_t horizontalInitial = 0;
    uint32_t horizontalScrolled = 0;
    uint32_t verticalScrollbar = 0;
    uint32_t horizontalScrollbar = 0;
    uint32_t verticalTrackScrolled = 0;
    ok = renderPixel(scrollRuntime, 20, 20, verticalInitial) && ok;
    ok = renderPixel(scrollRuntime, 20, 45, verticalClipped) && ok;
    ok = renderPixel(scrollRuntime, 55, 25, verticalScrollbar) && ok;
    ok = renderPixel(scrollRuntime, 100, 45, horizontalScrollbar) && ok;
    sendWheel(scrollRuntime, 20.0f, 20.0f, -240.0f);
    ok = renderPixel(scrollRuntime, 20, 20, verticalScrolled) && ok;
    ok = renderPixel(scrollRuntime, 90, 20, horizontalInitial) && ok;
    sendWheel(scrollRuntime, 90.0f, 20.0f, -240.0f, true);
    ok = renderPixel(scrollRuntime, 90, 20, horizontalScrolled) && ok;
    sendMouse(scrollRuntime, skui::EventType::MouseDown, 55.0f, 45.0f);
    sendMouse(scrollRuntime, skui::EventType::MouseUp, 55.0f, 45.0f);
    ok = renderPixel(scrollRuntime, 20, 20, verticalTrackScrolled) && ok;
    ok = expect(verticalInitial == solidColor(0x22, 0x33, 0x44), "scroll container should show initial child content") && ok;
    ok = expect(verticalClipped == solidColor(0x11, 0x11, 0x11), "overflow-y container should clip offscreen children") && ok;
    ok = expect(verticalScrollbar == solidColor(0x45, 0xDE, 0xD5), "overflow-y container should render a vertical scrollbar thumb") && ok;
    ok = expect(horizontalScrollbar == solidColor(0x44, 0xDC, 0xD2), "overflow-x container should render a horizontal scrollbar thumb") && ok;
    ok = expect(verticalScrolled == solidColor(0xAB, 0xCD, 0xEF), "mouse wheel should scroll vertical overflow content") && ok;
    ok = expect(horizontalInitial == solidColor(0x11, 0x11, 0x11), "overflow-x container should clip horizontal children before scrolling") && ok;
    ok = expect(horizontalScrolled == solidColor(0x77, 0x88, 0x99), "Shift+mouse wheel should scroll horizontal overflow content") && ok;
    ok = expect(verticalTrackScrolled == solidColor(0xAB, 0xCD, 0xEF), "clicking vertical scrollbar track should update scroll position") && ok;

    constexpr std::string_view stableScrollbarHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .stable {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 50px;
      height: 40px;
      overflow-y: auto;
      overflow-x: hidden;
      scrollbar-gutter: stable;
      background-color: #111111;
    }
    .fill {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 50px;
      height: 80px;
      background-color: #abcdef;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="stable">
      <div class="fill"></div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime stableScrollbarRuntime(options);
    stableScrollbarRuntime.resize(kWidth, kHeight, 1.0f);
    if (!stableScrollbarRuntime.loadDocumentFromString(stableScrollbarHtml, "")) {
        std::cerr << "stable scrollbar load failed: " << stableScrollbarRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t stableContent = 0;
    uint32_t stableGutter = 0;
    uint32_t stableThumb = 0;
    ok = renderPixel(stableScrollbarRuntime, 20, 20, stableContent) && ok;
    ok = renderPixel(stableScrollbarRuntime, 52, 45, stableGutter) && ok;
    ok = renderPixel(stableScrollbarRuntime, 55, 25, stableThumb) && ok;
    ok = expect(stableContent == solidColor(0xAB, 0xCD, 0xEF), "stable scrollbar container should still render visible child content") && ok;
    ok = expect(stableGutter == solidColor(0x1D, 0x24, 0x2B), "scrollbar-gutter stable should reserve space outside child content") && ok;
    ok = expect(stableThumb == solidColor(0x44, 0xDC, 0xD2), "scrollbar-gutter stable should still render a vertical thumb") && ok;

    constexpr std::string_view textareaHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .notes {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 120px;
      height: 64px;
      background-color: #112233;
      color: #ffffff;
      font-size: 14px;
    }
    .notes:focus {
      background-color: #334455;
    }
  </style>
</head>
<body>
  <div class="root">
    <textarea id="notes" class="notes" placeholder="notes"></textarea>
  </div>
</body>
</html>
)html";

    std::string textareaClipboard;
    skui::RuntimeOptions textareaOptions = options;
    textareaOptions.readClipboardText = [&] { return textareaClipboard; };
    textareaOptions.writeClipboardText = [&](std::string_view text) { textareaClipboard = std::string(text); };
    skui::Runtime textareaRuntime(textareaOptions);
    textareaRuntime.resize(kWidth, kHeight, 1.0f);

    std::string textareaValue;
    textareaRuntime.setElementEventCallback([&](const skui::ElementEvent& event) {
        if (event.type == skui::ElementEventType::Input) {
            textareaValue = event.value;
        }
    });

    if (!textareaRuntime.loadDocumentFromString(textareaHtml, "")) {
        std::cerr << "textarea load failed: " << textareaRuntime.lastError() << "\n";
        return 1;
    }

    sendMouse(textareaRuntime, skui::EventType::MouseDown, 20.0f, 20.0f);
    sendMouse(textareaRuntime, skui::EventType::MouseUp, 20.0f, 20.0f);
    sendText(textareaRuntime, "one");
    sendKey(textareaRuntime, 0x0D);
    sendText(textareaRuntime, "two");
    ok = expect(textareaValue == "one\ntwo", "textarea should accept Enter and preserve line breaks") && ok;
    sendKey(textareaRuntime, 'A', false, true);
    sendKey(textareaRuntime, 'C', false, true);
    ok = expect(textareaClipboard == "one\ntwo", "Ctrl+C should copy multiline textarea selection") && ok;
    textareaClipboard = "red\r\nblue";
    sendKey(textareaRuntime, 'V', false, true);
    ok = expect(textareaValue == "red\nblue", "Ctrl+V should normalize CRLF text for textarea") && ok;
    sendKey(textareaRuntime, 'Z', false, true);
    ok = expect(textareaValue == "one\ntwo", "Ctrl+Z should undo textarea edits") && ok;
    sendMouse(textareaRuntime, skui::EventType::MouseDown, 80.0f, 36.0f);
    sendMouse(textareaRuntime, skui::EventType::MouseUp, 80.0f, 36.0f);
    sendKey(textareaRuntime, 0x24);
    sendText(textareaRuntime, "2-");
    ok = expect(textareaValue == "one\n2-two", "textarea Home should move to the current line start") && ok;

    return ok ? 0 : 1;
}
