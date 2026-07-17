#include "skui_runtime.h"
#include "skui_runtime_helpers.h"
#include "skui_dropdown.h"
#include "skui_virtual_table.h"
#ifdef _WIN32
#include "skui_win32_event_adapter.h"
#endif

#include "include/core/SkColorSpace.h"
#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkSurface.h"
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
#include <limits>
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

bool isMostlyGreen(uint32_t color) {
    const unsigned red = (color >> 16u) & 0xFFu;
    const unsigned green = (color >> 8u) & 0xFFu;
    const unsigned blue = color & 0xFFu;
    return green > 180u && red < 80u && blue < 80u;
}

bool isDimBlue(uint32_t color) {
    const unsigned red = (color >> 16u) & 0xFFu;
    const unsigned green = (color >> 8u) & 0xFFu;
    const unsigned blue = color & 0xFFu;
    return blue >= 35u && blue <= 90u && red < 30u && green < 50u;
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

bool renderPixelWithColorSpace(skui::Runtime& runtime,
                               int x,
                               int y,
                               const sk_sp<SkColorSpace>& colorSpace,
                               uint32_t& out) {
    std::vector<uint32_t> pixels;
    pixels.assign(static_cast<size_t>(kWidth) * kHeight, 0);
    const SkImageInfo info = SkImageInfo::Make(kWidth,
                                               kHeight,
                                               kBGRA_8888_SkColorType,
                                               kPremul_SkAlphaType,
                                               colorSpace);
    const sk_sp<SkSurface> surface = SkSurfaces::WrapPixels(
        info,
        pixels.data(),
        static_cast<size_t>(kWidth) * sizeof(uint32_t));
    if (!surface) {
        std::cerr << "linear render surface creation failed\n";
        return false;
    }

    runtime.render(*surface->getCanvas());
    out = pixelAt(pixels, x, y);
    return true;
}

bool renderLinearPixel(skui::Runtime& runtime, int x, int y, uint32_t& out) {
    return renderPixelWithColorSpace(runtime, x, y, SkColorSpace::MakeSRGBLinear(), out);
}

bool renderSrgbPixel(skui::Runtime& runtime, int x, int y, uint32_t& out) {
    return renderPixelWithColorSpace(runtime, x, y, SkColorSpace::MakeSRGB(), out);
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

bool renderAfterScroll(skui::Runtime& runtime) {
    uint32_t unused = 0;
    return renderPixel(runtime, 0, 0, unused);
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

bool writeTextFixture(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
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

bool writeSemiTransparentSrgbPngFixture(const std::filesystem::path& path) {
    static constexpr std::array<unsigned char, 16> kRgba = {
        166, 255, 253, 128, 166, 255, 253, 128,
        166, 255, 253, 128, 166, 255, 253, 128,
    };
    const SkImageInfo info = SkImageInfo::Make(2,
                                               2,
                                               kRGBA_8888_SkColorType,
                                               kUnpremul_SkAlphaType,
                                               SkColorSpace::MakeSRGB());
    const SkPixmap pixmap(info, kRgba.data(), 2u * 4u);
    SkPngEncoder::Options options;
    return writeSkDataFixture(path, SkPngEncoder::Encode(pixmap, options));
}

bool writeSpritePngFixture(const std::filesystem::path& path) {
    std::array<unsigned char, 64> rgba{};
    const auto fillFrame = [&rgba](int frameX,
                                   int frameY,
                                   unsigned char red,
                                   unsigned char green,
                                   unsigned char blue) {
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 2; ++x) {
                const int pixelX = frameX * 2 + x;
                const int pixelY = frameY * 2 + y;
                const size_t offset =
                    (static_cast<size_t>(pixelY) * 4u +
                     static_cast<size_t>(pixelX)) *
                    4u;
                rgba[offset] = red;
                rgba[offset + 1] = green;
                rgba[offset + 2] = blue;
                rgba[offset + 3] = 0xFF;
            }
        }
    };
    fillFrame(0, 0, 0xFF, 0x00, 0x00);
    fillFrame(1, 0, 0x00, 0xFF, 0x00);
    fillFrame(0, 1, 0x00, 0x00, 0xFF);
    fillFrame(1, 1, 0xFF, 0xFF, 0xFF);

    const SkImageInfo info =
        SkImageInfo::Make(4, 4, kRGBA_8888_SkColorType, kPremul_SkAlphaType);
    SkPixmap pixmap(info, rgba.data(), 4u * 4u);
    SkPngEncoder::Options options;
    return writeSkDataFixture(path,
                              SkPngEncoder::Encode(pixmap, options));
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
        scaledOptions.textScale = 1.25f;
        skui::Runtime scaledRuntime(scaledOptions);
        scaledRuntime.resize(300, 150, 2.0f);

        ok = expect(scaledRuntime.dpiScale() == 2.0f, "dpiScale should keep platform scale") && ok;
        ok = expect(scaledRuntime.scale() > 1.49f && scaledRuntime.scale() < 1.51f,
                    "runtime scale should expose user scale") && ok;
        ok = expect(scaledRuntime.textScale() > 1.24f &&
                        scaledRuntime.textScale() < 1.26f,
                    "runtime text scale should expose configured text scale") && ok;
        ok = expect(scaledRuntime.effectiveScale() > 3.74f &&
                        scaledRuntime.effectiveScale() < 3.76f,
                    "effective scale should combine dpi, user, and text scales") && ok;
        ok = expect(skui::runtimeLogicalWidth(scaledRuntime) == 80,
                    "logical width should include text scale") && ok;
        ok = expect(skui::runtimeLogicalHeight(scaledRuntime) == 40,
                    "logical height should include text scale") && ok;

        scaledRuntime.setTextScale(1.5f);
        ok = expect(scaledRuntime.textScale() > 1.49f &&
                        scaledRuntime.textScale() < 1.51f,
                    "setTextScale should update text scale") && ok;
        ok = expect(scaledRuntime.effectiveScale() > 4.49f &&
                        scaledRuntime.effectiveScale() < 4.51f,
                    "text scale should update the content scale") && ok;
        ok = expect(skui::runtimeLogicalWidth(scaledRuntime) == 67,
                    "setTextScale should update logical width") && ok;

        scaledRuntime.setScale(0.5f);
        ok = expect(scaledRuntime.effectiveScale() > 1.49f &&
                        scaledRuntime.effectiveScale() < 1.51f,
                    "setScale should update effective scale") && ok;
        ok = expect(skui::runtimeLogicalWidth(scaledRuntime) == 200,
                    "setScale should update logical width") && ok;

        scaledRuntime.setTextScale(0.0f);
        ok = expect(scaledRuntime.textScale() > 0.09f &&
                        scaledRuntime.textScale() < 0.11f,
                    "text scale should clamp invalid small values") && ok;
    }
    {
        constexpr std::string_view textScaleHtml = R"html(
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
    .label {
      position: absolute;
      left: 4px;
      top: 4px;
      color: #ffffff;
      font-size: 10px;
    }
    .marker {
      position: absolute;
      left: 50px;
      top: 10px;
      width: 20px;
      height: 20px;
      background-color: #ff0000;
    }
  </style>
</head>
<body>
  <div class="root">
    <span class="label">Text scale</span>
    <div class="marker" data-action="scaled-marker"></div>
  </div>
</body>
</html>
)html";

        skui::Runtime textScaleRuntime(options);
        textScaleRuntime.resize(kWidth, kHeight, 1.0f);
        if (!textScaleRuntime.loadDocumentFromString(textScaleHtml, "")) {
            std::cerr << "text scale load failed: "
                      << textScaleRuntime.lastError() << "\n";
            return 1;
        }
        int markerClicks = 0;
        textScaleRuntime.setElementEventCallback(
            [&](const skui::ElementEvent& event) {
                if (event.type == skui::ElementEventType::Click &&
                    event.action == "scaled-marker") {
                    ++markerClicks;
                }
            });

        std::vector<uint32_t> normalTextPixels;
        std::vector<uint32_t> scaledTextPixels;
        ok = renderPixels(textScaleRuntime, normalTextPixels) && ok;
        textScaleRuntime.setTextScale(2.0f);
        ok = renderPixels(textScaleRuntime, scaledTextPixels) && ok;
        const int normalTextPixelCount =
            countBrightPixels(normalTextPixels, 0, 0, 100, 60);
        const int scaledTextPixelCount =
            countBrightPixels(scaledTextPixels, 0, 0, 100, 60);
        ok = expect(scaledTextPixelCount > normalTextPixelCount + 20,
                    "text scale should enlarge rendered text") && ok;
        const uint32_t red = solidColor(255, 0, 0);
        ok = expect(pixelAt(normalTextPixels, 55, 15) == red &&
                        pixelAt(scaledTextPixels, 55, 15) != red &&
                        pixelAt(scaledTextPixels, 115, 30) == red,
                    "text scale should uniformly scale non-text geometry") && ok;
        sendMouse(textScaleRuntime, skui::EventType::MouseDown, 55.0f, 15.0f);
        sendMouse(textScaleRuntime, skui::EventType::MouseUp, 55.0f, 15.0f);
        ok = expect(markerClicks == 0,
                    "text scale should remove the old physical hit target") && ok;
        sendMouse(textScaleRuntime, skui::EventType::MouseDown, 115.0f, 30.0f);
        sendMouse(textScaleRuntime, skui::EventType::MouseUp, 115.0f, 30.0f);
        ok = expect(markerClicks == 1,
                    "text scale should update the physical hit target") && ok;
    }
    {
        constexpr std::string_view browserRootHtml = R"html(
<!doctype html>
<html id="document-root">
<head>
  <style>
    html {
      background-color: #112233;
    }
    body {
      background-color: #445566;
      width: 40px;
      height: 30px;
    }
  </style>
</head>
<body id="document-body"></body>
</html>
)html";

        skui::Runtime browserRootRuntime(options);
        browserRootRuntime.resize(120, 80, 1.0f);
        if (!browserRootRuntime.loadDocumentFromString(browserRootHtml, "")) {
            std::cerr << "browser root load failed: " << browserRootRuntime.lastError() << "\n";
            return 1;
        }
        uint32_t bodyPixel = 0;
        uint32_t htmlPixel = 0;
        ok = renderPixel(browserRootRuntime, 10, 10, bodyPixel) && ok;
        ok = renderPixel(browserRootRuntime, 80, 60, htmlPixel) && ok;
        ok = expect(bodyPixel == solidColor(0x44, 0x55, 0x66),
                    "body should remain a styleable document node") && ok;
        ok = expect(htmlPixel == solidColor(0x11, 0x22, 0x33),
                    "html should remain the viewport-sized document root") && ok;

        constexpr std::string_view autoBodyHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    html {
      background-color: #112233;
    }
    .screen {
      flex-grow: 1;
      position: relative;
    }
    .panel {
      position: absolute;
      left: 10px;
      top: 10px;
      right: 10px;
      bottom: 10px;
      background-color: #ff0000;
    }
  </style>
</head>
<body>
  <div class="screen">
    <div class="panel"></div>
  </div>
</body>
</html>
)html";

        skui::Runtime autoBodyRuntime(options);
        autoBodyRuntime.resize(120, 80, 1.0f);
        if (!autoBodyRuntime.loadDocumentFromString(autoBodyHtml, "")) {
            std::cerr << "auto body load failed: " << autoBodyRuntime.lastError() << "\n";
            return 1;
        }
        uint32_t collapsedPanelPixel = 0;
        ok = renderPixel(autoBodyRuntime, 20, 20, collapsedPanelPixel) && ok;
        ok = expect(collapsedPanelPixel == solidColor(0x11, 0x22, 0x33),
                    "auto-height body should not lend viewport height to flex-grow descendants") && ok;
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

    constexpr std::string_view zIndexHtml = R"html(
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
    .layer {
      position: absolute;
      top: 10px;
      width: 40px;
      height: 40px;
    }
    .higher {
      left: 10px;
      z-index: 2;
      background-color: #ff0000;
    }
    .lower {
      left: 10px;
      z-index: -1;
      background-color: #0000ff;
    }
    .tie-first {
      left: 70px;
      z-index: auto;
      background-color: #00ff00;
    }
    .tie-last {
      left: 70px;
      background-color: #ffff00;
    }
  </style>
</head>
<body>
  <div class="root">
    <div id="higher" class="layer higher" data-action="higher"></div>
    <div id="lower" class="layer lower" data-action="lower"></div>
    <div id="tie-first" class="layer tie-first" data-action="tie-first"></div>
    <div id="tie-last" class="layer tie-last" data-action="tie-last"></div>
  </div>
</body>
</html>
)html";
    {
        skui::Runtime zIndexRuntime(options);
        zIndexRuntime.resize(kWidth, kHeight, 1.0f);
        if (!zIndexRuntime.loadDocumentFromString(zIndexHtml, "")) {
            std::cerr << "z-index document load failed: "
                      << zIndexRuntime.lastError() << "\n";
            return 1;
        }

        std::string clickedAction;
        zIndexRuntime.setElementEventCallback([&](const skui::ElementEvent& event) {
            if (event.type == skui::ElementEventType::Click) {
                clickedAction = event.action;
            }
        });

        uint32_t higherPixel = 0;
        uint32_t tiePixel = 0;
        ok = renderPixel(zIndexRuntime, 20, 20, higherPixel) && ok;
        ok = renderPixel(zIndexRuntime, 80, 20, tiePixel) && ok;
        ok = expect(higherPixel == solidColor(0xFF, 0x00, 0x00),
                    "larger z-index should paint above a later sibling") && ok;
        ok = expect(tiePixel == solidColor(0xFF, 0xFF, 0x00),
                    "equal z-index should preserve DOM paint order") && ok;

        sendMouse(zIndexRuntime, skui::EventType::MouseDown, 20.0f, 20.0f);
        sendMouse(zIndexRuntime, skui::EventType::MouseUp, 20.0f, 20.0f);
        ok = expect(clickedAction == "higher",
                    "hit testing should follow z-index paint order") && ok;

        ok = zIndexRuntime.setStyleById("higher", "z-index:-2;") && ok;
        uint32_t updatedPixel = 0;
        ok = renderPixel(zIndexRuntime, 20, 20, updatedPixel) && ok;
        ok = expect(updatedPixel == solidColor(0x00, 0x00, 0xFF),
                    "runtime z-index changes should update paint order") && ok;

        clickedAction.clear();
        sendMouse(zIndexRuntime, skui::EventType::MouseDown, 20.0f, 20.0f);
        sendMouse(zIndexRuntime, skui::EventType::MouseUp, 20.0f, 20.0f);
        ok = expect(clickedAction == "lower",
                    "runtime z-index changes should update hit testing") && ok;

        ok = zIndexRuntime.setStyleById("higher", "z-index:auto;") && ok;
        uint32_t autoPixel = 0;
        ok = renderPixel(zIndexRuntime, 20, 20, autoPixel) && ok;
        ok = expect(autoPixel == solidColor(0xFF, 0x00, 0x00),
                    "z-index:auto should restore the default zero layer") && ok;
    }

    constexpr std::string_view eventMutationTransitionHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .item {
      width: 80px;
      height: 20px;
      background-color: #0044ff;
      transition: height 100ms ease-in-out;
    }
    .item.active {
      height: 60px;
    }
  </style>
</head>
<body>
  <div class="root">
    <button id="first" class="item active"></button>
    <button id="second" class="item" data-action="select-second"></button>
  </div>
</body>
</html>
)html";
    {
        bool insideClickCallback = false;
        bool requestedRedrawInsideClickCallback = false;
        skui::RuntimeOptions eventMutationOptions = options;
        eventMutationOptions.requestRedraw = [&] {
            requestedRedrawInsideClickCallback =
                requestedRedrawInsideClickCallback || insideClickCallback;
        };

        skui::Runtime eventMutationRuntime(eventMutationOptions);
        eventMutationRuntime.resize(kWidth, kHeight, 1.0f);
        if (!eventMutationRuntime.loadDocumentFromString(eventMutationTransitionHtml, "")) {
            std::cerr << "event mutation transition load failed: "
                      << eventMutationRuntime.lastError() << "\n";
            return 1;
        }
        eventMutationRuntime.setElementEventCallback(
            [&](const skui::ElementEvent& event) {
                if (event.type != skui::ElementEventType::Click ||
                    event.action != "select-second") {
                    return;
                }

                insideClickCallback = true;
                {
                    skui::RuntimeUpdateBatch update(eventMutationRuntime);
                    eventMutationRuntime.removeClassById("first", "active");
                    eventMutationRuntime.addClassById("second", "active");
                }
                insideClickCallback = false;
            });

        sendMouse(eventMutationRuntime,
                  skui::EventType::MouseDown,
                  20.0f,
                  70.0f);
        sendMouse(eventMutationRuntime,
                  skui::EventType::MouseUp,
                  20.0f,
                  70.0f);
        ok = expect(!requestedRedrawInsideClickCallback,
                    "event callback mutations should commit after pointer state updates") && ok;
        ok = expect(!eventMutationRuntime.hasClassById("first", "active") &&
                        eventMutationRuntime.hasClassById("second", "active"),
                    "event callback class mutations should remain applied") && ok;
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

    constexpr std::string_view disabledFilterHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: transparent;
    }
    .disabled-button {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 60px;
      height: 40px;
      background-color: #ff0000;
      cursor: pointer;
    }
    .disabled-button:hover {
      background-color: #00ff00;
    }
    .disabled-label {
      position: absolute;
      left: 8px;
      top: 8px;
      width: 44px;
      height: 20px;
    }
    .filter-host {
      position: absolute;
      left: 90px;
      top: 10px;
      width: 40px;
      height: 40px;
      filter: grayscale(100%) brightness(50%);
    }
    .filter-shape {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 20px;
      height: 20px;
      border-radius: 10px;
      background-color: #00ff00;
    }
  </style>
</head>
<body>
  <div class="root">
    <button id="disabled-button"
            class="disabled-button"
            data-action="disabled-click"
            disabled="disabled">
      <span class="disabled-label">Disabled</span>
    </button>
    <div class="filter-host">
      <div class="filter-shape"></div>
    </div>
  </div>
</body>
</html>
)html";
    skui::RuntimeOptions disabledFilterOptions = options;
    disabledFilterOptions.clearColor = SK_ColorTRANSPARENT;
    skui::Runtime disabledFilterRuntime(disabledFilterOptions);
    disabledFilterRuntime.resize(kWidth, kHeight, 1.0f);
    ok = expect(disabledFilterRuntime.loadDocumentFromString(disabledFilterHtml),
                "disabled filter document should load") && ok;
    int disabledClicks = 0;
    int disabledPointerEvents = 0;
    disabledFilterRuntime.setElementEventCallback([&](const skui::ElementEvent& event) {
        if (event.action != "disabled-click") {
            return;
        }
        if (event.type == skui::ElementEventType::Click) {
            ++disabledClicks;
        } else if (event.type == skui::ElementEventType::MouseDown ||
                   event.type == skui::ElementEventType::MouseUp) {
            ++disabledPointerEvents;
        }
    });

    uint32_t disabledColor = 0;
    uint32_t disabledHoverColor = 0;
    uint32_t filteredShapeColor = 0;
    uint32_t filteredTransparentColor = 0;
    ok = renderPixel(disabledFilterRuntime, 65, 45, disabledColor) && ok;
    sendMouse(disabledFilterRuntime, skui::EventType::MouseMove, 20.0f, 20.0f);
    ok = renderPixel(disabledFilterRuntime, 65, 45, disabledHoverColor) && ok;
    ok = renderPixel(disabledFilterRuntime, 100, 20, filteredShapeColor) && ok;
    ok = renderPixel(disabledFilterRuntime, 120, 30, filteredTransparentColor) && ok;
    const auto colorChannel = [](uint32_t color, unsigned shift) {
        return (color >> shift) & 0xFFu;
    };
    const unsigned disabledRed = colorChannel(disabledColor, 16u);
    const unsigned disabledGreen = colorChannel(disabledColor, 8u);
    const unsigned disabledBlue = colorChannel(disabledColor, 0u);
    ok = expect(disabledRed > 200u && disabledGreen < 50u && disabledBlue < 50u,
                "disabled controls should not apply an implicit visual filter") && ok;
    ok = expect(disabledHoverColor == disabledColor,
                "disabled controls should not enter hover state") && ok;
    ok = expect(colorChannel(filteredShapeColor, 24u) == 255u &&
                    colorChannel(filteredShapeColor, 16u) ==
                        colorChannel(filteredShapeColor, 8u) &&
                    colorChannel(filteredShapeColor, 8u) ==
                        colorChannel(filteredShapeColor, 0u),
                "grayscale and brightness filters should compose across a subtree") && ok;
    ok = expect(colorChannel(filteredTransparentColor, 24u) == 0u,
                "color filters should preserve transparent pixels outside irregular content") && ok;

    skui::Event disabledDown;
    disabledDown.type = skui::EventType::MouseDown;
    disabledDown.button = skui::MouseButton::Left;
    disabledDown.x = 20.0f;
    disabledDown.y = 20.0f;
    skui::Event disabledUp = disabledDown;
    disabledUp.type = skui::EventType::MouseUp;
    ok = expect(disabledFilterRuntime.handleEvent(disabledDown) &&
                    disabledFilterRuntime.handleEvent(disabledUp),
                "disabled controls should consume pointer input") && ok;
    ok = expect(disabledPointerEvents == 0 && disabledClicks == 0,
                "disabled controls should not dispatch pointer or click events") && ok;

    ok = expect(disabledFilterRuntime.removeAttributeById("disabled-button", "disabled"),
                "removing disabled should re-enable a control") && ok;
    sendMouse(disabledFilterRuntime, skui::EventType::MouseDown, 20.0f, 20.0f);
    sendMouse(disabledFilterRuntime, skui::EventType::MouseUp, 20.0f, 20.0f);
    ok = expect(disabledPointerEvents == 2 && disabledClicks == 1,
                "re-enabled controls should dispatch pointer and click events") && ok;
    sendMouse(disabledFilterRuntime, skui::EventType::MouseMove, 20.0f, 20.0f);
    sendMouse(disabledFilterRuntime, skui::EventType::MouseDown, 20.0f, 20.0f);
    ok = expect(disabledFilterRuntime.setAttributeById(
                    "disabled-button",
                    "disabled",
                    "disabled"),
                "setting disabled should disable a control at runtime") && ok;
    uint32_t runtimeDisabledColor = 0;
    ok = renderPixel(disabledFilterRuntime, 65, 45, runtimeDisabledColor) && ok;
    sendMouse(disabledFilterRuntime, skui::EventType::MouseUp, 20.0f, 20.0f);
    ok = expect(runtimeDisabledColor == disabledColor,
                "runtime disabling should immediately clear hover and active styles") && ok;
    ok = expect(disabledPointerEvents == 3 && disabledClicks == 1,
                "runtime disabling during a press should suppress mouse up and click") && ok;

    constexpr std::string_view mouseDownTargetHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 220px;
      height: 120px;
    }
    .panel {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 90px;
      height: 42px;
      background-color: #dddddd;
    }
    .editor {
      position: absolute;
      left: 10px;
      top: 68px;
      width: 150px;
      height: 32px;
    }
  </style>
</head>
<body>
  <div class="root">
    <div id="panel" class="panel">Panel</div>
    <input id="editor" class="editor" value="">
  </div>
</body>
</html>
)html";
    {
        skui::Runtime mouseDownRuntime(options);
        mouseDownRuntime.resize(kWidth, kHeight, 1.0f);
        ok = expect(mouseDownRuntime.loadDocumentFromString(mouseDownTargetHtml),
                    "mouse down target document should load") && ok;

        int panelMouseDowns = 0;
        int editorMouseDowns = 0;
        std::string editorValue;
        mouseDownRuntime.setElementEventCallback([&](const skui::ElementEvent& event) {
            if (event.type == skui::ElementEventType::Input && event.id == "editor") {
                editorValue = event.value;
                return;
            }
            if (event.type != skui::ElementEventType::MouseDown) {
                return;
            }
            if (event.id == "panel") {
                ++panelMouseDowns;
            } else if (event.id == "editor") {
                ++editorMouseDowns;
            }
        });

        sendMouse(mouseDownRuntime, skui::EventType::MouseDown, 20.0f, 20.0f);
        sendMouse(mouseDownRuntime, skui::EventType::MouseUp, 20.0f, 20.0f);
        ok = expect(panelMouseDowns == 1,
                    "mouse down should be emitted for non-action hit elements") && ok;

        sendMouse(mouseDownRuntime, skui::EventType::MouseDown, 20.0f, 78.0f);
        sendMouse(mouseDownRuntime, skui::EventType::MouseUp, 20.0f, 78.0f);
        sendText(mouseDownRuntime, "a");
        ok = expect(editorMouseDowns == 1,
                    "mouse down should be emitted for input elements") && ok;
        ok = expect(editorValue == "a",
                    "input should keep focus after mouse down callback") && ok;
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
    html {
      width: 100%;
      height: 100%;
    }
    body {
      width: 100%;
      height: 100%;
      margin: 0;
    }
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

    constexpr std::string_view pointerPassThroughHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .screen {
      position: relative;
      width: 160px;
      height: 100px;
      background-color: transparent;
    }
    .button {
      position: absolute;
      left: 20px;
      top: 20px;
      width: 60px;
      height: 30px;
      background-color: #123456;
      cursor: pointer;
    }
    .decor {
      position: absolute;
      right: 10px;
      top: 10px;
      width: 30px;
      height: 30px;
      background-color: rgba(255,255,255,0);
    }
  </style>
</head>
<body>
  <div class="screen">
    <div id="button" class="button" data-action="button-click"></div>
    <div id="decor" class="decor"></div>
  </div>
</body>
</html>
)html";
    skui::Runtime pointerPassThroughRuntime(options);
    pointerPassThroughRuntime.resize(kWidth, kHeight, 1.0f);
    if (!pointerPassThroughRuntime.loadDocumentFromString(pointerPassThroughHtml, "")) {
        std::cerr << "pointer pass-through load failed: "
                  << pointerPassThroughRuntime.lastError() << "\n";
        return 1;
    }
    int passThroughClicks = 0;
    pointerPassThroughRuntime.setElementEventCallback([&](const skui::ElementEvent& event) {
        if (event.type == skui::ElementEventType::Click && event.action == "button-click") {
            ++passThroughClicks;
        }
    });
    skui::Event passThroughDown;
    passThroughDown.type = skui::EventType::MouseDown;
    passThroughDown.button = skui::MouseButton::Left;
    passThroughDown.x = 120.0f;
    passThroughDown.y = 80.0f;
    ok = expect(!pointerPassThroughRuntime.handleEvent(passThroughDown),
                "transparent non-interactive area should not consume mouse down") && ok;

    skui::Event passThroughUp = passThroughDown;
    passThroughUp.type = skui::EventType::MouseUp;
    ok = expect(!pointerPassThroughRuntime.handleEvent(passThroughUp),
                "transparent non-interactive area should not consume mouse up") && ok;

    skui::Event decorDown = passThroughDown;
    decorDown.x = 140.0f;
    decorDown.y = 20.0f;
    ok = expect(!pointerPassThroughRuntime.handleEvent(decorDown),
                "non-interactive decorative element should not consume mouse down") && ok;
    skui::Event decorUp = decorDown;
    decorUp.type = skui::EventType::MouseUp;
    ok = expect(!pointerPassThroughRuntime.handleEvent(decorUp),
                "non-interactive decorative element should not consume mouse up") && ok;

    skui::Event buttonDown = passThroughDown;
    buttonDown.x = 30.0f;
    buttonDown.y = 30.0f;
    ok = expect(pointerPassThroughRuntime.handleEvent(buttonDown),
                "data-action element should consume mouse down") && ok;
    skui::Event buttonUp = buttonDown;
    buttonUp.type = skui::EventType::MouseUp;
    ok = expect(pointerPassThroughRuntime.handleEvent(buttonUp),
                "data-action element should consume mouse up") && ok;
    ok = expect(passThroughClicks == 1,
                "data-action element should still emit click") && ok;

    skui::Event passThroughWheel;
    passThroughWheel.type = skui::EventType::MouseWheel;
    passThroughWheel.wheelDelta = -120.0f;
    passThroughWheel.x = 120.0f;
    passThroughWheel.y = 80.0f;
    ok = expect(!pointerPassThroughRuntime.handleEvent(passThroughWheel),
                "transparent non-interactive area should not consume mouse wheel") && ok;
    skui::Event decorWheel = passThroughWheel;
    decorWheel.x = 140.0f;
    decorWheel.y = 20.0f;
    ok = expect(!pointerPassThroughRuntime.handleEvent(decorWheel),
                "non-interactive decorative element should not consume mouse wheel") && ok;
    skui::Event buttonWheel = passThroughWheel;
    buttonWheel.x = 30.0f;
    buttonWheel.y = 30.0f;
    ok = expect(pointerPassThroughRuntime.handleEvent(buttonWheel),
                "data-action element should consume mouse wheel without scrolling") && ok;

    ok = expect(pointerPassThroughRuntime.setConsumesEventsById("button", false),
                "setConsumesEventsById should disable pointer events at runtime") && ok;
    ok = expect(!pointerPassThroughRuntime.handleEvent(buttonDown),
                "runtime-disabled pointer events should not consume mouse down") && ok;
    ok = expect(!pointerPassThroughRuntime.handleEvent(buttonUp),
                "runtime-disabled pointer events should not consume mouse up") && ok;
    ok = expect(!pointerPassThroughRuntime.handleEvent(buttonWheel),
                "runtime-disabled pointer events should not consume mouse wheel") && ok;
    ok = expect(passThroughClicks == 1,
                "runtime-disabled pointer events should not emit click") && ok;

    ok = expect(pointerPassThroughRuntime.setConsumesEventsById("button", true),
                "setConsumesEventsById should restore pointer events at runtime") && ok;
    ok = expect(pointerPassThroughRuntime.handleEvent(buttonDown),
                "runtime-restored pointer events should consume mouse down") && ok;
    ok = expect(pointerPassThroughRuntime.handleEvent(buttonUp),
                "runtime-restored pointer events should consume mouse up") && ok;
    ok = expect(pointerPassThroughRuntime.handleEvent(buttonWheel),
                "runtime-restored pointer events should consume mouse wheel") && ok;
    ok = expect(passThroughClicks == 2,
                "runtime-restored pointer events should emit click") && ok;

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

    const std::filesystem::path stylesheetFixtureDir =
        std::filesystem::temp_directory_path() / "skui_external_stylesheet";
    std::error_code stylesheetEc;
    std::filesystem::remove_all(stylesheetFixtureDir, stylesheetEc);
    std::filesystem::create_directories(stylesheetFixtureDir, stylesheetEc);
    ok = expect(!stylesheetEc, "external stylesheet fixture directory should be created") && ok;
    ok = expect(writeTextFixture(
                    stylesheetFixtureDir / "head.css",
                    ".external-box { background-color: #123456; }\n"
                    "@keyframes externalFade { 0% { opacity: 1; } 100% { opacity: 0.25; } }\n"),
                "head stylesheet fixture should be written") && ok;
    ok = expect(writeTextFixture(
                    stylesheetFixtureDir / "body.css",
                    ".external-box { background-color: #654321; }\n"),
                "body stylesheet fixture should be written") && ok;

    constexpr std::string_view externalStylesheetHtml = R"html(
<!doctype html>
<html>
<head>
  <link rel="stylesheet" href="head.css">
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .external-box {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 40px;
      height: 40px;
    }
  </style>
</head>
<body>
  <div class="root">
    <link rel="stylesheet" href="body.css">
    <div class="external-box"></div>
  </div>
</body>
</html>
)html";

    skui::Runtime externalStylesheetRuntime(options);
    externalStylesheetRuntime.resize(kWidth, kHeight, 1.0f);
    if (!externalStylesheetRuntime.loadDocumentFromString(
            externalStylesheetHtml,
            utf8PathText(stylesheetFixtureDir))) {
        std::cerr << "external stylesheet load failed: "
                  << externalStylesheetRuntime.lastError() << "\n";
        return 1;
    }

    uint32_t externalStylesheetPixel = 0;
    ok = renderPixel(externalStylesheetRuntime, 20, 20, externalStylesheetPixel) && ok;
    ok = expect(externalStylesheetPixel == solidColor(0x65, 0x43, 0x21),
                "external stylesheets should load relative to basePath and follow document order") && ok;
    std::filesystem::remove_all(stylesheetFixtureDir, stylesheetEc);

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

    constexpr std::string_view paddingHtml = R"html(
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
    .label {
      position: absolute;
      left: 10px;
      top: 10px;
      width: auto;
      padding: 10px;
      background-color: #ff0000;
      color: #ffffff;
      font-size: 10px;
    }
    .percent-label {
      position: absolute;
      left: 60px;
      top: 10px;
      width: 60px;
      height: 40px;
      padding-left: 10%;
      background-color: #008000;
      color: #ffffff;
      font-size: 20px;
    }
  </style>
</head>
<body>
  <div class="root">
    <label class="label">X</label>
    <label class="percent-label">XX</label>
  </div>
</body>
</html>
)html";

    skui::Runtime paddingRuntime(options);
    paddingRuntime.resize(kWidth, kHeight, 1.0f);
    if (!paddingRuntime.loadDocumentFromString(paddingHtml, "")) {
        std::cerr << "padding load failed: " << paddingRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t paddingRight = 0;
    uint32_t paddingBottom = 0;
    ok = renderPixel(paddingRuntime, 30, 15, paddingRight) && ok;
    ok = renderPixel(paddingRuntime, 15, 35, paddingBottom) && ok;
    ok = expect(paddingRight == solidColor(0xFF, 0x00, 0x00),
                "auto-sized text label should include horizontal padding in its layout box") && ok;
    ok = expect(paddingBottom == solidColor(0xFF, 0x00, 0x00),
                "auto-sized text label should include vertical padding in its layout box") && ok;
    std::vector<uint32_t> paddingPixels;
    ok = renderPixels(paddingRuntime, paddingPixels) && ok;
    ok = expect(countBrightPixels(paddingPixels, 60, 10, 72, 50) == 0,
                "percentage padding should offset text inside the label content box") && ok;

    constexpr std::string_view autoHeightSelectableHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 100px;
      background-color: #000000;
    }
    .card {
      position: absolute;
      left: 5px;
      top: 5px;
      width: 70px;
      padding: 5px;
      background-color: #ff0000;
    }
    selectable {
      font-size: 14px;
      color: #ffffff;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="card">
      <selectable>alpha beta gamma delta</selectable>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime autoHeightSelectableRuntime(options);
    autoHeightSelectableRuntime.resize(kWidth, kHeight, 1.0f);
    if (!autoHeightSelectableRuntime.loadDocumentFromString(
            autoHeightSelectableHtml,
            "")) {
        std::cerr << "auto-height selectable load failed: "
                  << autoHeightSelectableRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t wrappedCardBottom = 0;
    ok = renderPixel(
             autoHeightSelectableRuntime,
             7,
             70,
             wrappedCardBottom) &&
         ok;
    ok = expect(
             wrappedCardBottom == solidColor(0xFF, 0x00, 0x00),
             "wrapped selectable text should expand its auto-height parent") &&
         ok;

    constexpr std::string_view siblingWrappedTextHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 180px;
      height: 140px;
      background-color: #000000;
    }
    .card {
      position: absolute;
      left: 5px;
      top: 5px;
      width: 92px;
      padding: 5px;
      background-color: #112233;
    }
    .title {
      display: block;
      position: relative;
      margin-bottom: 5px;
      background-color: #ff0000;
      color: #ffffff;
      font-size: 16px;
      font-weight: bold;
    }
    .body {
      display: block;
      position: relative;
      background-color: #0000ff;
      color: #ffffff;
      font-size: 14px;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="card">
      <selectable class="title">Visible card</selectable>
      <selectable class="body">This block uses display none.</selectable>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime siblingWrappedTextRuntime(options);
    constexpr int siblingWrappedTextWidth = 180;
    constexpr int siblingWrappedTextHeight = 140;
    siblingWrappedTextRuntime.resize(
        siblingWrappedTextWidth,
        siblingWrappedTextHeight,
        1.0f);
    if (!siblingWrappedTextRuntime.loadDocumentFromString(
            siblingWrappedTextHtml,
            "")) {
        std::cerr << "sibling wrapped text load failed: "
                  << siblingWrappedTextRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t wrappedTitleSecondLine = 0;
    uint32_t wrappedBodyStart = 0;
    ok = renderPixelAt(
             siblingWrappedTextRuntime,
             siblingWrappedTextWidth,
             siblingWrappedTextHeight,
             80,
             40,
             wrappedTitleSecondLine) &&
         ok;
    ok = renderPixelAt(
             siblingWrappedTextRuntime,
             siblingWrappedTextWidth,
             siblingWrappedTextHeight,
             80,
             65,
             wrappedBodyStart) &&
         ok;
    ok = expect(
             wrappedTitleSecondLine == solidColor(0xFF, 0x00, 0x00),
             "wrapped title should reserve its second line before the next text node") &&
         ok;
    ok = expect(
             wrappedBodyStart == solidColor(0x00, 0x00, 0xFF),
             "following text node should start after the wrapped title") &&
         ok;

    constexpr std::string_view browserBoxSizingHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 340px;
      height: 220px;
      background-color: #000000;
    }
    .content-box {
      position: absolute;
      left: 5px;
      top: 5px;
      width: 260px;
      height: 20px;
      padding: 10px 18px;
      border: 1px solid #ff0000;
      background-color: #ff0000;
    }
    .measure-card {
      position: absolute;
      left: 5px;
      top: 60px;
      width: 260px;
      padding: 15px 18px 16px;
      border: 1px solid #224466;
      background-color: #224466;
    }
    .title {
      display: block;
      position: relative;
      margin-bottom: 5px;
      font-size: 16px;
      font-weight: bold;
    }
    .body {
      display: block;
      position: relative;
      font-size: 13px;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="content-box"></div>
    <div class="measure-card">
      <selectable class="title">Animated card</selectable>
      <selectable class="body">Runtime style changes drive transform and opacity transitions.</selectable>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime browserBoxSizingRuntime(options);
    constexpr int browserBoxWidth = 340;
    constexpr int browserBoxHeight = 220;
    browserBoxSizingRuntime.resize(
        browserBoxWidth,
        browserBoxHeight,
        1.0f);
    if (!browserBoxSizingRuntime.loadDocumentFromString(
            browserBoxSizingHtml,
            "")) {
        std::cerr << "browser box sizing load failed: "
                  << browserBoxSizingRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t contentBoxRight = 0;
    uint32_t measuredCardInside = 0;
    uint32_t measuredCardOutside = 0;
    ok = renderPixelAt(
             browserBoxSizingRuntime,
             browserBoxWidth,
             browserBoxHeight,
             300,
             20,
             contentBoxRight) &&
         ok;
    ok = renderPixelAt(
             browserBoxSizingRuntime,
             browserBoxWidth,
             browserBoxHeight,
             20,
             145,
             measuredCardInside) &&
         ok;
    ok = renderPixelAt(
             browserBoxSizingRuntime,
             browserBoxWidth,
             browserBoxHeight,
             20,
             162,
             measuredCardOutside) &&
         ok;
    ok = expect(
             contentBoxRight == solidColor(0xFF, 0x00, 0x00),
             "default content-box width should exclude padding and border") &&
         ok;
    ok = expect(
             measuredCardInside == solidColor(0x22, 0x44, 0x66),
             "content-box text width should keep the card body visible") &&
         ok;
    ok = expect(
             measuredCardOutside == solidColor(0x00, 0x00, 0x00),
             "browser content-box sizing should avoid a spurious third line") &&
         ok;

    constexpr std::string_view balancedCardsHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 700px;
      height: 160px;
      background-color: #000000;
    }
    .row {
      position: absolute;
      left: 5px;
      top: 5px;
      width: 650px;
      display: flex;
      flex-direction: row;
      align-items: flex-start;
    }
    .notice-card {
      position: relative;
      flex-grow: 1;
      margin-right: 24px;
      padding: 15px 18px 16px;
      border: 1px solid #228844;
      background-color: #228844;
    }
    .motion-card {
      position: relative;
      width: 260px;
      flex-shrink: 0;
      padding: 15px 18px 16px;
      border: 1px solid #224488;
      background-color: #224488;
    }
    .title {
      display: block;
      position: relative;
      margin-bottom: 5px;
      font-size: 16px;
      font-weight: bold;
    }
    .notice-text {
      display: block;
      position: relative;
      font-size: 14px;
    }
    .motion-text {
      display: block;
      position: relative;
      font-size: 13px;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="row">
      <div class="notice-card">
        <selectable class="title">Visible card</selectable>
        <selectable class="notice-text">This block uses display none, so hidden state removes it from layout and hit testing.</selectable>
      </div>
      <div class="motion-card">
        <selectable class="title">Animated card</selectable>
        <selectable class="motion-text">Runtime style changes drive transform and opacity transitions.</selectable>
      </div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime balancedCardsRuntime(options);
    constexpr int balancedCardsWidth = 700;
    constexpr int balancedCardsHeight = 160;
    balancedCardsRuntime.resize(
        balancedCardsWidth,
        balancedCardsHeight,
        1.0f);
    if (!balancedCardsRuntime.loadDocumentFromString(
            balancedCardsHtml,
            "")) {
        std::cerr << "balanced cards load failed: "
                  << balancedCardsRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t noticeCardInside = 0;
    uint32_t noticeCardOutside = 0;
    uint32_t motionCardInside = 0;
    uint32_t motionCardOutside = 0;
    ok = renderPixelAt(
             balancedCardsRuntime,
             balancedCardsWidth,
             balancedCardsHeight,
             20,
             90,
             noticeCardInside) &&
         ok;
    ok = renderPixelAt(
             balancedCardsRuntime,
             balancedCardsWidth,
             balancedCardsHeight,
             20,
             140,
             noticeCardOutside) &&
         ok;
    ok = renderPixelAt(
             balancedCardsRuntime,
             balancedCardsWidth,
             balancedCardsHeight,
             500,
             90,
             motionCardInside) &&
         ok;
    ok = renderPixelAt(
             balancedCardsRuntime,
             balancedCardsWidth,
             balancedCardsHeight,
             500,
             115,
             motionCardOutside) &&
         ok;
    ok = expect(
             noticeCardInside == solidColor(0x22, 0x88, 0x44),
             "flexible notice card should contain its wrapped text") &&
         ok;
    ok = expect(
             noticeCardOutside == solidColor(0x00, 0x00, 0x00),
             "flexible notice card should stop after its measured text lines") &&
         ok;
    ok = expect(
             motionCardInside == solidColor(0x22, 0x44, 0x88),
             "fixed motion card should contain its two-line text") &&
         ok;
    ok = expect(
             motionCardOutside == solidColor(0x00, 0x00, 0x00),
             "fixed motion card should keep browser-like auto height") &&
         ok;

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
    sendMouse(selectableRuntime, skui::EventType::MouseDoubleClick, 126.0f, 78.0f);
    sendKey(selectableRuntime, 'C', false, true);
    ok = expect(selectableClipboard == "能把最新的",
                "double-clicking selectable line-end whitespace should select all text") && ok;

    constexpr std::string_view selectableOverflowHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 180px;
      height: 80px;
      background-color: #000000;
    }
    selectable {
      position: absolute;
      top: 5px;
      width: 70px;
      height: 22px;
      color: #ffffff;
      font-size: 14px;
    }
    .visible {
      left: 10px;
    }
    .hidden {
      left: 100px;
      overflow: hidden;
    }
  </style>
</head>
<body>
  <div class="root">
    <selectable class="visible">alpha beta gamma</selectable>
    <selectable class="hidden">alpha beta gamma</selectable>
  </div>
</body>
</html>
)html";

    skui::Runtime selectableOverflowRuntime(options);
    selectableOverflowRuntime.resize(kWidth, kHeight, 1.0f);
    if (!selectableOverflowRuntime.loadDocumentFromString(
            selectableOverflowHtml,
            "")) {
        std::cerr << "selectable overflow load failed: "
                  << selectableOverflowRuntime.lastError() << "\n";
        return 1;
    }
    std::vector<uint32_t> selectableOverflowPixels;
    ok = renderPixels(selectableOverflowRuntime, selectableOverflowPixels) && ok;
    const int visibleOverflowPixels =
        countBrightPixels(selectableOverflowPixels, 10, 32, 85, 55);
    const int hiddenOverflowPixels =
        countBrightPixels(selectableOverflowPixels, 100, 32, 175, 55);
    ok = expect(visibleOverflowPixels > 5,
                "selectable text should paint wrapped lines outside its box "
                "when overflow is visible") &&
         ok;
    ok = expect(hiddenOverflowPixels == 0,
                "selectable text should clip wrapped lines when overflow is hidden") &&
         ok;

    constexpr std::string_view dynamicSelectableHtml = R"html(
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
    .host {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 120px;
      height: 70px;
    }
    .dynamic-selectable {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 120px;
      height: 24px;
      color: #ffffff;
      font-size: 16px;
    }
  </style>
</head>
<body>
  <div class="root">
    <div id="dynamic-host" class="host"></div>
  </div>
</body>
</html>
)html";

    selectableClipboard.clear();
    skui::Runtime dynamicSelectableRuntime(selectableOptions);
    dynamicSelectableRuntime.resize(kWidth, kHeight, 1.0f);
    if (!dynamicSelectableRuntime.loadDocumentFromString(dynamicSelectableHtml, "")) {
        std::cerr << "dynamic selectable load failed: "
                  << dynamicSelectableRuntime.lastError() << "\n";
        return 1;
    }
    ok = expect(dynamicSelectableRuntime.appendHtmlById(
                    "dynamic-host",
                    "<selectable id=\"dynamic-copy\" class=\"dynamic-selectable\">append ok</selectable>"),
                "dynamic DOM should append selectable text") && ok;
    sendMouse(dynamicSelectableRuntime, skui::EventType::MouseDown, 10.0f, 20.0f);
    sendMouse(dynamicSelectableRuntime, skui::EventType::MouseMove, 130.0f, 20.0f);
    sendMouse(dynamicSelectableRuntime, skui::EventType::MouseUp, 130.0f, 20.0f);
    sendKey(dynamicSelectableRuntime, 'C', false, true);
    ok = expect(selectableClipboard == "append ok",
                "appended selectable text should support drag selection and Ctrl+C") && ok;
    selectableClipboard.clear();
    ok = expect(dynamicSelectableRuntime.replaceHtmlById(
                    "dynamic-copy",
                    "<selectable id=\"dynamic-copy\" class=\"dynamic-selectable\">replace ok</selectable>"),
                "dynamic DOM should replace selectable text") && ok;
    sendMouse(dynamicSelectableRuntime, skui::EventType::MouseDown, 10.0f, 20.0f);
    sendMouse(dynamicSelectableRuntime, skui::EventType::MouseMove, 130.0f, 20.0f);
    sendMouse(dynamicSelectableRuntime, skui::EventType::MouseUp, 130.0f, 20.0f);
    sendKey(dynamicSelectableRuntime, 'C', false, true);
    ok = expect(selectableClipboard == "replace ok",
                "replaced selectable text should support drag selection and Ctrl+C") && ok;

    constexpr std::string_view multilineSelectableHtml = R"html(
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
    selectable {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 120px;
      height: 70px;
      color: #ffffff;
      font-size: 16px;
    }
  </style>
</head>
<body>
  <div class="root">
    <selectable id="multi-copy"></selectable>
  </div>
</body>
</html>
)html";

    selectableClipboard.clear();
    skui::Runtime multilineSelectableRuntime(selectableOptions);
    multilineSelectableRuntime.resize(kWidth, kHeight, 1.0f);
    if (!multilineSelectableRuntime.loadDocumentFromString(multilineSelectableHtml, "")) {
        std::cerr << "multiline selectable load failed: "
                  << multilineSelectableRuntime.lastError() << "\n";
        return 1;
    }
    ok = expect(multilineSelectableRuntime.setValueById("multi-copy", "alpha\nbeta\ngamma"),
                "setValueById should update selectable multiline text") && ok;
    sendMouse(multilineSelectableRuntime, skui::EventType::MouseDown, 10.0f, 20.0f);
    sendMouse(multilineSelectableRuntime, skui::EventType::MouseMove, 130.0f, 58.0f);
    sendMouse(multilineSelectableRuntime, skui::EventType::MouseUp, 130.0f, 58.0f);
    sendKey(multilineSelectableRuntime, 'C', false, true);
    ok = expect(selectableClipboard == "alpha\nbeta\ngamma",
                "selectable should support drag selection across explicit text lines") && ok;

    constexpr std::string_view multilineSelectableValueHtml = R"html(
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
    selectable {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 120px;
      height: 70px;
      color: #ffffff;
      font-size: 16px;
    }
  </style>
</head>
<body>
  <div class="root">
    <selectable value="one&#10;two&#10;three"></selectable>
  </div>
</body>
</html>
)html";

    selectableClipboard.clear();
    skui::Runtime multilineSelectableValueRuntime(selectableOptions);
    multilineSelectableValueRuntime.resize(kWidth, kHeight, 1.0f);
    if (!multilineSelectableValueRuntime.loadDocumentFromString(multilineSelectableValueHtml, "")) {
        std::cerr << "multiline selectable value load failed: "
                  << multilineSelectableValueRuntime.lastError() << "\n";
        return 1;
    }
    sendMouse(multilineSelectableValueRuntime, skui::EventType::MouseDown, 10.0f, 20.0f);
    sendMouse(multilineSelectableValueRuntime, skui::EventType::MouseMove, 130.0f, 58.0f);
    sendMouse(multilineSelectableValueRuntime, skui::EventType::MouseUp, 130.0f, 58.0f);
    sendKey(multilineSelectableValueRuntime, 'C', false, true);
    ok = expect(selectableClipboard == "one\ntwo\nthree",
                "selectable value attribute should preserve explicit text lines") && ok;

    constexpr std::string_view scrolledSelectableHtml = R"html(
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
    .scroller {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 150px;
      height: 80px;
      overflow-y: auto;
      background-color: #111111;
    }
    selectable {
      position: absolute;
      left: 0px;
      top: 70px;
      width: 130px;
      height: 70px;
      color: #ffffff;
      font-size: 16px;
    }
    .bottom {
      position: absolute;
      left: 0px;
      top: 180px;
      width: 1px;
      height: 20px;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="scroller">
      <selectable id="scrolled-copy"></selectable>
      <div class="bottom"></div>
    </div>
  </div>
</body>
</html>
)html";

    selectableClipboard.clear();
    skui::Runtime scrolledSelectableRuntime(selectableOptions);
    scrolledSelectableRuntime.resize(kWidth, kHeight, 1.0f);
    if (!scrolledSelectableRuntime.loadDocumentFromString(scrolledSelectableHtml, "")) {
        std::cerr << "scrolled selectable load failed: "
                  << scrolledSelectableRuntime.lastError() << "\n";
        return 1;
    }
    ok = expect(scrolledSelectableRuntime.setValueById("scrolled-copy", "red\ngreen\nblue"),
                "setValueById should update scrolled selectable multiline text") && ok;
    sendWheel(scrolledSelectableRuntime, 30.0f, 30.0f, -120.0f);
    ok = renderAfterScroll(scrolledSelectableRuntime) && ok;
    sendMouse(scrolledSelectableRuntime, skui::EventType::MouseDown, 10.0f, 40.0f);
    sendMouse(scrolledSelectableRuntime, skui::EventType::MouseMove, 130.0f, 78.0f);
    sendMouse(scrolledSelectableRuntime, skui::EventType::MouseUp, 130.0f, 78.0f);
    sendKey(scrolledSelectableRuntime, 'C', false, true);
    ok = expect(selectableClipboard == "red\ngreen\nblue",
                "selectable should support multiline selection inside a scrolled container") && ok;

    constexpr std::string_view linkedSelectableHtml = R"html(
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
    selectable {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 150px;
      height: 70px;
      color: #ffffff;
      font-size: 16px;
    }
  </style>
</head>
<body>
  <div class="root">
    <selectable value="first&#10;open link&#10;last" data-links="6:15:open-url:https://example.com"></selectable>
  </div>
</body>
</html>
)html";

    selectableClipboard.clear();
    std::string linkedAction;
    skui::RuntimeOptions linkedOptions = selectableOptions;
    linkedOptions.onElementEvent = [&](const skui::ElementEvent& event) {
        if (event.type == skui::ElementEventType::Click) {
            linkedAction = event.action;
        }
    };
    skui::Runtime linkedSelectableRuntime(linkedOptions);
    linkedSelectableRuntime.resize(kWidth, kHeight, 1.0f);
    if (!linkedSelectableRuntime.loadDocumentFromString(linkedSelectableHtml, "")) {
        std::cerr << "linked selectable load failed: "
                  << linkedSelectableRuntime.lastError() << "\n";
        return 1;
    }
    sendMouse(linkedSelectableRuntime, skui::EventType::MouseDown, 10.0f, 20.0f);
    sendMouse(linkedSelectableRuntime, skui::EventType::MouseMove, 150.0f, 58.0f);
    sendMouse(linkedSelectableRuntime, skui::EventType::MouseUp, 150.0f, 58.0f);
    sendKey(linkedSelectableRuntime, 'C', false, true);
    ok = expect(selectableClipboard == "first\nopen link\nlast",
                "selectable links should not split multiline copy selection") && ok;
    linkedAction.clear();
    sendMouse(linkedSelectableRuntime, skui::EventType::MouseDown, 18.0f, 33.0f);
    sendMouse(linkedSelectableRuntime, skui::EventType::MouseUp, 18.0f, 33.0f);
    ok = expect(linkedAction == "open-url:https://example.com",
                "clicking a selectable link range should emit its action") && ok;

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

    constexpr std::string_view highDpiRoundedBoxHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 400px;
      height: 300px;
      background-color: #000000;
    }
    .large-rounded-box {
      position: absolute;
      left: 20px;
      top: 20px;
      width: 320px;
      height: 220px;
      background-color: #ffffff;
      border-radius: 40px;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="large-rounded-box"></div>
  </div>
</body>
</html>
)html";
    skui::Runtime highDpiRoundedBoxRuntime(options);
    if (!highDpiRoundedBoxRuntime.loadDocumentFromString(highDpiRoundedBoxHtml, "")) {
        std::cerr << "high DPI rounded box load failed: "
                  << highDpiRoundedBoxRuntime.lastError() << "\n";
        return 1;
    }

    constexpr int highDpiWidth = 800;
    constexpr int highDpiHeight = 600;
    std::vector<uint32_t> highDpiRoundedBoxPixels(
        static_cast<size_t>(highDpiWidth) * highDpiHeight,
        0);
    ok = expect(highDpiRoundedBoxRuntime.renderToBgraPixels(
                    highDpiRoundedBoxPixels.data(),
                    highDpiWidth,
                    highDpiHeight,
                    static_cast<size_t>(highDpiWidth) * sizeof(uint32_t),
                    2.0f),
                "large rounded box should render at high DPI") && ok;

    int smoothEdgeRows = 0;
    for (int y = 42; y < 119; ++y) {
        int firstEdgeX = -1;
        for (int x = 40; x < 120; ++x) {
            if (pixelAt(highDpiRoundedBoxPixels, highDpiWidth, x, y) !=
                solidColor(0x00, 0x00, 0x00)) {
                firstEdgeX = x;
                break;
            }
        }
        if (firstEdgeX >= 0 && firstEdgeX + 1 < 120 &&
            pixelAt(highDpiRoundedBoxPixels, highDpiWidth, firstEdgeX, y) !=
                pixelAt(highDpiRoundedBoxPixels, highDpiWidth, firstEdgeX + 1, y)) {
            ++smoothEdgeRows;
        }
    }
    ok = expect(smoothEdgeRows > 8,
                "large rounded boxes should retain device-resolution antialiasing at high DPI") && ok;

    constexpr std::string_view highDpiGradientHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 160px;
      height: 100px;
      background-color: #000000;
    }
    .gradient-box {
      position: absolute;
      left: 20px;
      top: 20px;
      width: 100px;
      height: 40px;
      background: linear-gradient(to right, #000000, #ffffff);
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="gradient-box"></div>
  </div>
</body>
</html>
)html";
    skui::Runtime highDpiGradientRuntime(options);
    if (!highDpiGradientRuntime.loadDocumentFromString(highDpiGradientHtml, "")) {
        std::cerr << "high DPI gradient load failed: "
                  << highDpiGradientRuntime.lastError() << "\n";
        return 1;
    }

    constexpr int highDpiGradientWidth = 320;
    constexpr int highDpiGradientHeight = 200;
    std::vector<uint32_t> highDpiGradientPixels(
        static_cast<size_t>(highDpiGradientWidth) * highDpiGradientHeight,
        0);
    ok = expect(highDpiGradientRuntime.renderToBgraPixels(
                    highDpiGradientPixels.data(),
                    highDpiGradientWidth,
                    highDpiGradientHeight,
                    static_cast<size_t>(highDpiGradientWidth) * sizeof(uint32_t),
                    2.0f),
                "gradient box should render at high DPI") && ok;

    int repeatedDevicePixelPairs = 0;
    constexpr int gradientLeft = 40;
    constexpr int gradientTop = 40;
    constexpr int gradientLogicalWidth = 100;
    for (int logicalX = 4; logicalX < gradientLogicalWidth - 4; ++logicalX) {
        const int deviceX = gradientLeft + logicalX * 2;
        if (pixelAt(highDpiGradientPixels, highDpiGradientWidth, deviceX, gradientTop + 20) ==
            pixelAt(highDpiGradientPixels, highDpiGradientWidth, deviceX + 1, gradientTop + 20)) {
            ++repeatedDevicePixelPairs;
        }
    }
    ok = expect(repeatedDevicePixelPairs < 20,
                "gradients should be rasterized at device resolution instead of repeating cached pixels") && ok;

    constexpr std::string_view gradientOpacityHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: transparent;
    }
    .gradient-opacity {
      position: absolute;
      left: 20px;
      top: 20px;
      width: 100px;
      height: 40px;
      background: linear-gradient(to right, #ff0000, #0000ff);
      opacity: 0.5;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="gradient-opacity"></div>
  </div>
</body>
</html>
)html";
    skui::RuntimeOptions gradientOpacityOptions = options;
    gradientOpacityOptions.clearColor = SK_ColorTRANSPARENT;
    skui::Runtime gradientOpacityRuntime(gradientOpacityOptions);
    if (!gradientOpacityRuntime.loadDocumentFromString(gradientOpacityHtml, "")) {
        std::cerr << "gradient opacity load failed: "
                  << gradientOpacityRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t gradientOpacityStart = 0;
    uint32_t gradientOpacityEnd = 0;
    ok = renderPixel(gradientOpacityRuntime, 20, 40, gradientOpacityStart) && ok;
    ok = renderPixel(gradientOpacityRuntime, 119, 40, gradientOpacityEnd) && ok;
    const auto channelNear = [](unsigned actual, unsigned expected) {
        const unsigned difference = actual > expected ? actual - expected : expected - actual;
        return difference <= 3u;
    };
    ok = expect(channelNear((gradientOpacityStart >> 24u) & 0xFFu, 128u) &&
                    channelNear((gradientOpacityStart >> 16u) & 0xFFu, 128u) &&
                    channelNear((gradientOpacityStart >> 8u) & 0xFFu, 0u) &&
                    channelNear(gradientOpacityStart & 0xFFu, 0u),
                "gradient opacity should uniformly premultiply the first gradient color") && ok;
    ok = expect(channelNear((gradientOpacityEnd >> 24u) & 0xFFu, 128u) &&
                    channelNear((gradientOpacityEnd >> 16u) & 0xFFu, 0u) &&
                    channelNear((gradientOpacityEnd >> 8u) & 0xFFu, 0u) &&
                    channelNear(gradientOpacityEnd & 0xFFu, 128u),
                "gradient opacity should uniformly premultiply the last gradient color") && ok;

    constexpr std::string_view rgbaGradientHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: transparent;
    }
    .rgba-gradient {
      position: absolute;
      left: 20px;
      top: 10px;
      width: 100px;
      height: 60px;
      background: linear-gradient(
        to bottom,
        rgba(219, 255, 251, 0.78),
        rgba(186, 255, 253, 0.78),
        rgba(152, 255, 254, 0.78)
      );
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="rgba-gradient"></div>
  </div>
</body>
</html>
)html";
    skui::Runtime rgbaGradientRuntime(gradientOpacityOptions);
    if (!rgbaGradientRuntime.loadDocumentFromString(rgbaGradientHtml, "")) {
        std::cerr << "rgba gradient load failed: " << rgbaGradientRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t rgbaGradientTop = 0;
    uint32_t rgbaGradientMiddle = 0;
    uint32_t rgbaGradientBottom = 0;
    ok = renderPixel(rgbaGradientRuntime, 40, 10, rgbaGradientTop) && ok;
    ok = renderPixel(rgbaGradientRuntime, 40, 40, rgbaGradientMiddle) && ok;
    ok = renderPixel(rgbaGradientRuntime, 40, 69, rgbaGradientBottom) && ok;
    ok = expect(channelNear((rgbaGradientTop >> 24u) & 0xFFu, 199u) &&
                    channelNear((rgbaGradientMiddle >> 24u) & 0xFFu, 199u) &&
                    channelNear((rgbaGradientBottom >> 24u) & 0xFFu, 199u),
                "rgba gradient stops should preserve uniform alpha across the gradient") && ok;

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

    constexpr std::string_view transitionHtml = R"html(
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
    .card {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 20px;
      height: 20px;
      background-color: #0044ff;
      transition: transform 80ms linear, opacity 80ms linear;
    }
  </style>
</head>
<body>
  <div class="root">
    <div id="card" class="card"></div>
  </div>
</body>
</html>
)html";

    std::atomic_int animationRedraws{0};
    skui::RuntimeOptions transitionOptions = options;
    transitionOptions.requestRedraw = [&] {
        animationRedraws.fetch_add(1);
    };
    skui::Runtime transitionRuntime(transitionOptions);
    transitionRuntime.resize(kWidth, kHeight, 1.0f);
    if (!transitionRuntime.loadDocumentFromString(transitionHtml, "")) {
        std::cerr << "transition load failed: " << transitionRuntime.lastError() << "\n";
        return 1;
    }

    uint32_t originPixel = 0;
    uint32_t translatedPixel = 0;
    ok = renderPixel(transitionRuntime, 15, 15, originPixel) && ok;
    ok = renderPixel(transitionRuntime, 55, 15, translatedPixel) && ok;
    ok = expect(isMostlyBlue(originPixel), "transition test card should initially render at origin") && ok;
    ok = expect(translatedPixel == solidColor(0x00, 0x00, 0x00),
                "transition test translated slot should initially be empty") && ok;
    ok = expect(transitionRuntime.setStyleById("card", "transform: translateX(40px);"),
                "transform transition style update should apply") && ok;
    ok = expect(animationRedraws.load() > 0,
                "transition should request redraw frames after style update") && ok;
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    ok = renderPixel(transitionRuntime, 15, 15, originPixel) && ok;
    ok = renderPixel(transitionRuntime, 55, 15, translatedPixel) && ok;
    ok = expect(isMostlyBlue(originPixel),
                "render should not advance a transition without tick") && ok;
    ok = expect(translatedPixel == solidColor(0x00, 0x00, 0x00),
                "render should preserve the current transition frame") && ok;
    (void)transitionRuntime.tick(0.12f);
    ok = renderPixel(transitionRuntime, 15, 15, originPixel) && ok;
    ok = renderPixel(transitionRuntime, 55, 15, translatedPixel) && ok;
    ok = expect(originPixel == solidColor(0x00, 0x00, 0x00),
                "transform transition should move the card away from origin") && ok;
    ok = expect(isMostlyBlue(translatedPixel),
                "transform transition should render at translated position") && ok;
    ok = expect(transitionRuntime.setStyleById("card",
                                               "transform: translateX(40px); opacity:0.2;"),
                "opacity transition style update should apply") && ok;
    (void)transitionRuntime.tick(0.12f);
    ok = renderPixel(transitionRuntime, 55, 15, translatedPixel) && ok;
    ok = expect(isDimBlue(translatedPixel),
                "opacity transition should blend the card toward transparency") && ok;
    ok = expect(transitionRuntime.setStyleById("card",
                                               "transform: translateX(40px); opacity:1;"),
                "opacity transition should support fading back in") && ok;
    (void)transitionRuntime.tick(0.12f);
    ok = renderPixel(transitionRuntime, 55, 15, translatedPixel) && ok;
    ok = expect(isMostlyBlue(translatedPixel),
                "opacity transition should restore the fully visible card") && ok;

    constexpr std::string_view heightTransitionHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      width: 100px;
      height: 100px;
      background-color: #000000;
    }
    .stack {
      width: 20px;
      height: 100px;
      display: flex;
      flex-direction: column;
      align-items: flex-start;
    }
    .expander {
      width: 20px;
      height: 20px;
      flex-shrink: 0;
      background-color: #ff0000;
      transition: height 100ms linear;
    }
    .follower {
      width: 20px;
      height: 20px;
      flex-shrink: 0;
      background-color: #00ff00;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="stack">
      <div id="expander" class="expander"></div>
      <div class="follower"></div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime heightTransitionRuntime(options);
    heightTransitionRuntime.resize(kWidth, kHeight, 1.0f);
    if (!heightTransitionRuntime.loadDocumentFromString(heightTransitionHtml, "")) {
        std::cerr << "height transition load failed: "
                  << heightTransitionRuntime.lastError() << "\n";
        return 1;
    }

    uint32_t initialFollowerPixel = 0;
    uint32_t halfHeightPixel = 0;
    uint32_t halfFollowerPixel = 0;
    uint32_t finalHeightPixel = 0;
    uint32_t finalFollowerPixel = 0;
    ok = renderPixel(heightTransitionRuntime, 10, 25, initialFollowerPixel) && ok;
    ok = expect(initialFollowerPixel == solidColor(0x00, 0xFF, 0x00),
                "height transition follower should start below initial height") && ok;
    ok = expect(heightTransitionRuntime.setStyleById("expander", "height:60px;"),
                "height transition style update should apply") && ok;
    ok = renderPixel(heightTransitionRuntime, 10, 25, initialFollowerPixel) && ok;
    ok = expect(initialFollowerPixel == solidColor(0x00, 0xFF, 0x00),
                "height transition should not advance without tick") && ok;
    ok = expect(heightTransitionRuntime.tick(0.05f),
                "height transition should request an intermediate frame") && ok;
    ok = renderPixel(heightTransitionRuntime, 10, 25, halfHeightPixel) && ok;
    ok = renderPixel(heightTransitionRuntime, 10, 45, halfFollowerPixel) && ok;
    ok = expect(halfHeightPixel == solidColor(0xFF, 0x00, 0x00),
                "height transition should interpolate the animated element height") && ok;
    ok = expect(halfFollowerPixel == solidColor(0x00, 0xFF, 0x00),
                "height transition should relayout following siblings each frame") && ok;
    (void)heightTransitionRuntime.tick(0.06f);
    ok = renderPixel(heightTransitionRuntime, 10, 45, finalHeightPixel) && ok;
    ok = renderPixel(heightTransitionRuntime, 10, 65, finalFollowerPixel) && ok;
    ok = expect(finalHeightPixel == solidColor(0xFF, 0x00, 0x00),
                "height transition should reach the target height") && ok;
    ok = expect(finalFollowerPixel == solidColor(0x00, 0xFF, 0x00),
                "height transition should leave siblings at their final layout") && ok;

    constexpr std::string_view cssTransformHtml = R"html(
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
      width: 10px;
      height: 10px;
      transform-origin: left top;
    }
    .first {
      top: 10px;
      background-color: #ff0000;
      transform: translateX(20px) scale(2);
    }
    .second {
      top: 40px;
      background-color: #00ff00;
      transform: scale(2) translateX(20px);
    }
    .origin {
      top: 65px;
      background-color: #0044ff;
      transform: scale(2);
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="box first"></div>
    <div class="box second"></div>
    <div class="box origin"></div>
  </div>
</body>
</html>
)html";

    skui::Runtime cssTransformRuntime(options);
    cssTransformRuntime.resize(kWidth, kHeight, 1.0f);
    if (!cssTransformRuntime.loadDocumentFromString(cssTransformHtml, "")) {
        std::cerr << "css transform load failed: "
                  << cssTransformRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t orderedTranslateThenScale = 0;
    uint32_t orderedScaleThenTranslate = 0;
    uint32_t originScaledEdge = 0;
    ok = renderPixel(cssTransformRuntime, 35, 15, orderedTranslateThenScale) && ok;
    ok = renderPixel(cssTransformRuntime, 55, 45, orderedScaleThenTranslate) && ok;
    ok = renderPixel(cssTransformRuntime, 28, 70, originScaledEdge) && ok;
    ok = expect(isMostlyRed(orderedTranslateThenScale),
                "CSS transform should preserve translate-before-scale order") && ok;
    ok = expect(isMostlyGreen(orderedScaleThenTranslate),
                "CSS transform should preserve scale-before-translate order") && ok;
    ok = expect(isMostlyBlue(originScaledEdge),
                "transform-origin:left top should scale from the top-left corner") && ok;

    constexpr std::string_view easeTransitionHtml = R"html(
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
    #ease-card {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 10px;
      height: 10px;
      background-color: #ff0000;
      transition: transform 100ms ease;
    }
  </style>
</head>
<body>
  <div class="root"><div id="ease-card"></div></div>
</body>
</html>
)html";

    skui::Runtime easeRuntime(options);
    easeRuntime.resize(kWidth, kHeight, 1.0f);
    if (!easeRuntime.loadDocumentFromString(easeTransitionHtml, "")) {
        std::cerr << "ease transition load failed: "
                  << easeRuntime.lastError() << "\n";
        return 1;
    }
    ok = expect(easeRuntime.setStyleById("ease-card",
                                         "transform: translateX(80px);"),
                "ease transition style update should apply") && ok;
    (void)easeRuntime.tick(0.05f);
    uint32_t easedPixel = 0;
    ok = renderPixel(easeRuntime, 75, 15, easedPixel) && ok;
    ok = expect(isMostlyRed(easedPixel),
                "CSS ease should use the standard cubic-bezier curve") && ok;

    constexpr std::string_view keyframeTransformHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    @keyframes moveFade {
      from {
        transform: translateX(0px);
        opacity: 1;
      }
      to {
        transform: translateX(40px);
        opacity: 0.2;
      }
    }
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .card {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 20px;
      height: 20px;
      background-color: #0044ff;
      animation: moveFade 100ms linear forwards;
    }
  </style>
</head>
<body>
  <div class="root"><div class="card"></div></div>
</body>
</html>
)html";

    skui::Runtime keyframeTransformRuntime(options);
    keyframeTransformRuntime.resize(kWidth, kHeight, 1.0f);
    if (!keyframeTransformRuntime.loadDocumentFromString(keyframeTransformHtml, "")) {
        std::cerr << "keyframe transform load failed: "
                  << keyframeTransformRuntime.lastError() << "\n";
        return 1;
    }
    (void)keyframeTransformRuntime.tick(0.12f);
    uint32_t keyframeOriginPixel = 0;
    uint32_t keyframeTargetPixel = 0;
    ok = renderPixel(keyframeTransformRuntime, 15, 15, keyframeOriginPixel) && ok;
    ok = renderPixel(keyframeTransformRuntime, 55, 15, keyframeTargetPixel) && ok;
    ok = expect(keyframeOriginPixel == solidColor(0x00, 0x00, 0x00),
                "transform keyframe should move the card away from origin") && ok;
    ok = expect(isDimBlue(keyframeTargetPixel),
                "opacity keyframe should fade the transformed card") && ok;

    const auto runSpriteAnimationTest = [&] {
        const std::filesystem::path spriteFixtureDir =
            std::filesystem::temp_directory_path() /
            "skui-sprite-animation-test";
        std::error_code spriteError;
        std::filesystem::create_directories(spriteFixtureDir,
                                            spriteError);
        ok = expect(writeSpritePngFixture(spriteFixtureDir /
                                          "sprite.png"),
                    "sprite bitmap fixture should be written") && ok;

        constexpr std::string_view spriteHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    @keyframes spriteFrames {
      0% { background-position: 0% 0%; }
      25% { background-position: 100% 0%; }
      50% { background-position: 0% 100%; }
      75%, 100% { background-position: 100% 100%; }
    }
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: #000000;
    }
    .sprite {
      position: absolute;
      top: 10px;
      width: 20px;
      height: 20px;
      background-image: url("sprite.png");
      background-size: 200% 200%;
      background-repeat: no-repeat;
    }
    .static-frame {
      left: 10px;
      background-position: 100% 0%;
    }
    .paused-frame {
      left: 40px;
      animation: spriteFrames 400ms step-end -100ms infinite paused;
    }
    .running-frame {
      left: 70px;
      animation: spriteFrames 400ms step-end infinite;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="sprite static-frame"></div>
    <div class="sprite paused-frame"></div>
    <div class="sprite running-frame"></div>
  </div>
</body>
</html>
)html";

        {
            std::atomic_int spriteRedraws{0};
            skui::RuntimeOptions spriteOptions = options;
            spriteOptions.requestRedraw = [&] {
                spriteRedraws.fetch_add(1);
            };
            skui::Runtime spriteRuntime(spriteOptions);
            spriteRuntime.resize(kWidth, kHeight, 1.0f);
            if (!spriteRuntime.loadDocumentFromString(
                    spriteHtml,
                    utf8PathText(spriteFixtureDir))) {
                std::cerr << "sprite load failed: "
                          << spriteRuntime.lastError() << "\n";
                ok = false;
                return;
            }

            uint32_t staticSpritePixel = 0;
            uint32_t pausedSpritePixel = 0;
            uint32_t runningSpritePixel = 0;
            bool spriteReady = false;
            for (int retry = 0; retry < 100 && !spriteReady; ++retry) {
                ok = renderPixel(spriteRuntime,
                                 20,
                                 20,
                                 staticSpritePixel) && ok;
                ok = renderPixel(spriteRuntime,
                                 50,
                                 20,
                                 pausedSpritePixel) && ok;
                ok = renderPixel(spriteRuntime,
                                 80,
                                 20,
                                 runningSpritePixel) && ok;
                spriteReady = isMostlyGreen(staticSpritePixel) &&
                              isMostlyGreen(pausedSpritePixel) &&
                              isMostlyRed(runningSpritePixel);
                if (!spriteReady) {
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(10));
                }
            }
            ok = expect(
                     isMostlyGreen(staticSpritePixel),
                     "background-position should select the right-top sprite frame") &&
                 ok;
            ok = expect(
                     isMostlyGreen(pausedSpritePixel),
                     "paused keyframe animation should hold the delayed sprite frame") &&
                 ok;
            ok = expect(
                     spriteRedraws.load() > 0,
                     "running sprite animation should request redraw frames") &&
                 ok;
            std::this_thread::sleep_for(std::chrono::milliseconds(120));
            ok = renderPixel(spriteRuntime,
                             80,
                             20,
                             runningSpritePixel) && ok;
            ok = expect(
                     isMostlyRed(runningSpritePixel),
                     "render should not advance a running sprite without tick") &&
                 ok;
            ok = expect(
                     spriteRuntime.tick(0.1f),
                     "infinite sprite animation should keep requesting ticks") &&
                 ok;
            ok = renderPixel(spriteRuntime,
                             80,
                             20,
                             runningSpritePixel) && ok;
            ok = renderPixel(spriteRuntime,
                             50,
                             20,
                             pausedSpritePixel) && ok;
            ok = expect(
                     isMostlyGreen(runningSpritePixel),
                     "tick should advance the running sprite to the next frame") &&
                 ok;
            ok = expect(
                     isMostlyGreen(pausedSpritePixel),
                     "tick should not advance a paused sprite animation") &&
                 ok;
        }
        std::filesystem::remove_all(spriteFixtureDir);
    };

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
        ok = expect(
                 imageBefore == solidColor(0x00, 0x00, 0x00) ||
                     isMostlyRed(imageBefore),
                 std::string("async bitmap first render should be pending or ready: ") +
                     fixture.fileName) &&
             ok;
        ok = expect(imageRedraws.load() > 0,
                    std::string("async bitmap image load should request redraw: ") + fixture.fileName) && ok;
        ok = expect(isMostlyRed(imageAfter),
                    std::string("async bitmap image should render after loading: ") + fixture.fileName) && ok;
    }

    {
        constexpr std::string_view parentOpacityImageHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: transparent;
    }
    .opacity-group {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 40px;
      height: 20px;
      background-color: #0000ff;
      opacity: 0.5;
    }
    .opacity-image {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 20px;
      height: 20px;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="opacity-group">
      <img class="opacity-image" src="red.png">
    </div>
  </div>
</body>
</html>
)html";
        skui::RuntimeOptions parentOpacityOptions = options;
        parentOpacityOptions.clearColor = SK_ColorTRANSPARENT;
        skui::Runtime parentOpacityRuntime(parentOpacityOptions);
        parentOpacityRuntime.resize(kWidth, kHeight, 1.0f);
        if (!parentOpacityRuntime.loadDocumentFromString(
                parentOpacityImageHtml,
                imageFixtureDir.string())) {
            std::cerr << "parent opacity image load failed: "
                      << parentOpacityRuntime.lastError() << "\n";
            return 1;
        }

        uint32_t imagePixel = 0;
        uint32_t backgroundPixel = 0;
        ok = renderPixel(parentOpacityRuntime, 15, 15, imagePixel) && ok;
        waitForDirty(parentOpacityRuntime);
        ok = renderPixel(parentOpacityRuntime, 15, 15, imagePixel) && ok;
        ok = renderPixel(parentOpacityRuntime, 45, 15, backgroundPixel) && ok;
        ok = expect(channelNear(colorChannel(imagePixel, 24u), 128u) &&
                        channelNear(colorChannel(imagePixel, 16u), 128u) &&
                        channelNear(colorChannel(imagePixel, 8u), 0u) &&
                        channelNear(colorChannel(imagePixel, 0u), 0u),
                    "parent opacity should composite bitmap children as one group") && ok;
        ok = expect(channelNear(colorChannel(backgroundPixel, 24u), 128u) &&
                        channelNear(colorChannel(backgroundPixel, 16u), 0u) &&
                        channelNear(colorChannel(backgroundPixel, 8u), 0u) &&
                        channelNear(colorChannel(backgroundPixel, 0u), 128u),
                    "parent opacity should apply uniformly to its own background") && ok;
    }

    {
        constexpr std::string_view imageOpacityTransitionHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 140px;
      height: 90px;
      background-color: transparent;
    }
    .photo {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 20px;
      height: 20px;
      opacity: 1;
      visibility: visible;
      transition: opacity 100ms linear;
    }
    .photo.hidden {
      opacity: 0;
      visibility: hidden;
    }
  </style>
</head>
<body>
  <div class="root">
    <img id="transition-photo" class="photo hidden" src="red.png">
  </div>
</body>
</html>
)html";
        skui::RuntimeOptions imageTransitionOptions = options;
        imageTransitionOptions.clearColor = SK_ColorTRANSPARENT;
        skui::Runtime imageTransitionRuntime(imageTransitionOptions);
        imageTransitionRuntime.resize(kWidth, kHeight, 1.0f);
        if (!imageTransitionRuntime.loadDocumentFromString(
                imageOpacityTransitionHtml,
                imageFixtureDir.string())) {
            std::cerr << "image opacity transition load failed: "
                      << imageTransitionRuntime.lastError() << "\n";
            return 1;
        }

        uint32_t transitionPixel = 0;
        ok = renderPixel(imageTransitionRuntime, 15, 15, transitionPixel) && ok;
        waitForDirty(imageTransitionRuntime);
        ok = renderPixel(imageTransitionRuntime, 15, 15, transitionPixel) && ok;
        ok = expect(colorChannel(transitionPixel, 24u) == 0u,
                    "hidden transition image should remain transparent after decoding") && ok;
        ok = expect(imageTransitionRuntime.removeClassById(
                        "transition-photo",
                        "hidden"),
                    "image fade hidden class should be removed") && ok;
        ok = renderPixel(imageTransitionRuntime, 15, 15, transitionPixel) && ok;
        ok = expect(colorChannel(transitionPixel, 24u) == 0u,
                    "image opacity transition should render its old value before the first tick") && ok;
        (void)imageTransitionRuntime.tick(0.05f);
        ok = renderPixel(imageTransitionRuntime, 15, 15, transitionPixel) && ok;
        ok = expect(channelNear(colorChannel(transitionPixel, 24u), 128u) &&
                        channelNear(colorChannel(transitionPixel, 16u), 128u),
                    "image opacity transition should render half opacity at its midpoint") && ok;
        imageTransitionRuntime.beginUpdate();
        ok = expect(imageTransitionRuntime.addClassById(
                        "transition-photo",
                        "hidden"),
                    "batched refresh should apply its intermediate hidden class") && ok;
        ok = expect(imageTransitionRuntime.removeClassById(
                        "transition-photo",
                        "hidden"),
                    "batched refresh should restore its final visible class") && ok;
        imageTransitionRuntime.endUpdate();
        ok = renderPixel(imageTransitionRuntime, 15, 15, transitionPixel) && ok;
        ok = expect(channelNear(colorChannel(transitionPixel, 24u), 128u) &&
                        channelNear(colorChannel(transitionPixel, 16u), 128u),
                    "batched no-op refresh should preserve transition progress") && ok;
        (void)imageTransitionRuntime.tick(0.05f);
        ok = renderPixel(imageTransitionRuntime, 15, 15, transitionPixel) && ok;
        ok = expect(colorChannel(transitionPixel, 24u) == 255u &&
                        colorChannel(transitionPixel, 16u) == 255u,
                    "image opacity transition should finish fully visible") && ok;
    }

    {
        const std::filesystem::path fixturePath =
            imageFixtureDir / "semi-transparent-srgb.png";
        ok = expect(writeSemiTransparentSrgbPngFixture(fixturePath),
                    "semi-transparent sRGB fixture should be written") && ok;

        skui::RuntimeOptions linearImageOptions = options;
        linearImageOptions.clearColor = SK_ColorTRANSPARENT;
        skui::Runtime linearImageRuntime(linearImageOptions);
        linearImageRuntime.resize(kWidth, kHeight, 1.0f);
        const std::string imageHtml = imageHtmlForSource(
            "semi-transparent-srgb.png");
        if (!linearImageRuntime.loadDocumentFromString(
                imageHtml,
                imageFixtureDir.string())) {
            std::cerr << "linear image load failed: "
                      << linearImageRuntime.lastError() << "\n";
            return 1;
        }

        uint32_t pendingPixel = 0;
        uint32_t linearPixel = 0;
        uint32_t srgbPixel = 0;
        ok = renderLinearPixel(linearImageRuntime, 20, 20, pendingPixel) && ok;
        waitForDirty(linearImageRuntime);
        ok = renderLinearPixel(linearImageRuntime, 20, 20, linearPixel) && ok;
        ok = renderSrgbPixel(linearImageRuntime, 20, 20, srgbPixel) && ok;

        const unsigned alpha = (linearPixel >> 24u) & 0xFFu;
        const unsigned red = (linearPixel >> 16u) & 0xFFu;
        const unsigned green = (linearPixel >> 8u) & 0xFFu;
        const unsigned blue = linearPixel & 0xFFu;
        const bool colorMatched = alpha >= 253u && alpha <= 255u &&
                                  red >= 47u && red <= 52u &&
                                  green >= 126u && green <= 130u &&
                                  blue >= 124u && blue <= 129u;
        ok = expect(colorMatched,
                    std::string("sRGB bitmap should convert to linear premultiplied pixels; actual=") +
                        std::to_string(red) + "," +
                        std::to_string(green) + "," +
                        std::to_string(blue) + "," +
                        std::to_string(alpha)) && ok;

        const unsigned srgbAlpha = (srgbPixel >> 24u) & 0xFFu;
        const unsigned srgbRed = (srgbPixel >> 16u) & 0xFFu;
        const unsigned srgbGreen = (srgbPixel >> 8u) & 0xFFu;
        const unsigned srgbBlue = srgbPixel & 0xFFu;
        const bool srgbColorMatched = srgbAlpha >= 253u && srgbAlpha <= 255u &&
                                      srgbRed >= 81u && srgbRed <= 85u &&
                                      srgbGreen >= 126u && srgbGreen <= 130u &&
                                      srgbBlue >= 125u && srgbBlue <= 129u;
        ok = expect(srgbColorMatched,
                    std::string("sRGB bitmap should preserve browser-style sRGB compositing; actual=") +
                        std::to_string(srgbRed) + "," +
                        std::to_string(srgbGreen) + "," +
                        std::to_string(srgbBlue) + "," +
                        std::to_string(srgbAlpha)) && ok;
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
            const uint32_t initialPixel = pixelAt(parallelPixels, x, imageSize / 2);
            ok = expect(initialPixel == solidColor(0x00, 0x00, 0x00) || isMostlyRed(initialPixel),
                        "parallel bitmap first render should be empty or already loaded") && ok;
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
        ok = expect(
                 unicodeImageBefore == solidColor(0x00, 0x00, 0x00) ||
                     isMostlyRed(unicodeImageBefore),
                 "unicode bitmap first render should be pending or ready") &&
             ok;
        ok = expect(unicodeImageRedraws.load() > 0,
                    "unicode bitmap image load should request redraw") && ok;
        ok = expect(isMostlyRed(unicodeImageAfter),
                    "unicode bitmap image should render after loading") && ok;
    }

    {
        const std::filesystem::path swapDir = std::filesystem::temp_directory_path() / "skui_bitmap_swap";
        std::filesystem::create_directories(swapDir);
        ok = expect(writeRedPngFixture(swapDir / "normal.png"),
                    "normal bitmap fixture should be written") && ok;
        ok = expect(writeBluePngFixture(swapDir / "pressed.png"),
                    "pressed bitmap fixture should be written") && ok;

        constexpr std::string_view swapHtml = R"html(
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
    .button-image {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 20px;
      height: 20px;
    }
  </style>
</head>
<body>
  <div class="root"><img id="buttonImage" class="button-image" src="normal.png"></div>
</body>
</html>
)html";

        skui::RuntimeOptions swapOptions = options;
        swapOptions.assetRoot = utf8PathText(swapDir);
        skui::Runtime swapRuntime(swapOptions);
        swapRuntime.resize(kWidth, kHeight, 1.0f);
        ok = expect(swapRuntime.loadDocumentFromString(swapHtml, utf8PathText(swapDir)),
                    "bitmap swap document should load") && ok;

        uint32_t swapPixel = 0;
        ok = renderPixel(swapRuntime, 20, 20, swapPixel) && ok;
        ok = expect(
                 swapPixel == solidColor(0x00, 0x00, 0x00) ||
                     isMostlyRed(swapPixel),
                 "initial bitmap first render should be pending or ready") &&
             ok;
        waitForDirty(swapRuntime);
        ok = renderPixel(swapRuntime, 20, 20, swapPixel) && ok;
        ok = expect(isMostlyRed(swapPixel), "initial bitmap should render after async load") && ok;

        ok = expect(swapRuntime.setAttributeById("buttonImage", "src", "pressed.png"),
                    "bitmap src should switch to pending pressed image") && ok;
        ok = renderPixel(swapRuntime, 20, 20, swapPixel) && ok;
        ok = expect(
                 isMostlyRed(swapPixel) || isMostlyBlue(swapPixel),
                 "bitmap switch should draw the current or newly ready image") &&
             ok;
        waitForDirty(swapRuntime);
        ok = renderPixel(swapRuntime, 20, 20, swapPixel) && ok;
        ok = expect(isMostlyBlue(swapPixel),
                    "pending bitmap should replace current image after loading") && ok;

        std::filesystem::remove_all(swapDir);
    }

    {
        const std::filesystem::path hiddenSwapDir =
            std::filesystem::temp_directory_path() / "skui_hidden_bitmap_swap";
        std::filesystem::create_directories(hiddenSwapDir);
        ok = expect(writeRedPngFixture(hiddenSwapDir / "normal.png"),
                    "hidden normal bitmap fixture should be written") && ok;
        ok = expect(writeBluePngFixture(hiddenSwapDir / "active.png"),
                    "hidden active bitmap fixture should be written") && ok;

        constexpr std::string_view hiddenSwapHtml = R"html(
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
    .menu-item {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 20px;
      height: 20px;
    }
    .menu-image {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 20px;
      height: 20px;
    }
    .active-image {
      display: none;
    }
    .menu-item.active .normal {
      display: none;
    }
    .menu-item.active .active-image {
      display: flex;
    }
  </style>
</head>
<body>
  <div class="root">
    <button id="button" class="menu-item">
      <img class="menu-image normal" src="normal.png">
      <img class="menu-image active-image" src="active.png">
    </button>
  </div>
</body>
</html>
)html";

        std::atomic_int hiddenSwapRedraws{0};
        skui::RuntimeOptions hiddenSwapOptions = options;
        hiddenSwapOptions.assetRoot = utf8PathText(hiddenSwapDir);
        hiddenSwapOptions.requestRedraw = [&] {
            hiddenSwapRedraws.fetch_add(1);
        };
        skui::Runtime hiddenSwapRuntime(hiddenSwapOptions);
        hiddenSwapRuntime.resize(kWidth, kHeight, 1.0f);
        ok = expect(hiddenSwapRuntime.loadDocumentFromString(hiddenSwapHtml, utf8PathText(hiddenSwapDir)),
                    "hidden bitmap swap document should load") && ok;

        uint32_t hiddenSwapPixel = 0;
        for (int retry = 0; retry < 100 && hiddenSwapRedraws.load() < 2; ++retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ok = expect(hiddenSwapRedraws.load() >= 2,
                    "hidden and visible bitmap nodes should both request image loads") && ok;
        ok = renderPixel(hiddenSwapRuntime, 20, 20, hiddenSwapPixel) && ok;
        ok = expect(isMostlyRed(hiddenSwapPixel),
                    "visible normal bitmap should render after async load") && ok;
        ok = expect(hiddenSwapRuntime.addClassById("button", "active"),
                    "hidden bitmap swap should switch to active class") && ok;
        ok = renderPixel(hiddenSwapRuntime, 20, 20, hiddenSwapPixel) && ok;
        ok = expect(isMostlyBlue(hiddenSwapPixel),
                    "hidden active bitmap should be ready when it first becomes visible") && ok;

        std::filesystem::remove_all(hiddenSwapDir);
    }

    {
        const std::filesystem::path lazySwapDir =
            std::filesystem::temp_directory_path() / "skui_lazy_hidden_bitmap_swap";
        std::filesystem::create_directories(lazySwapDir);
        ok = expect(writeRedPngFixture(lazySwapDir / "normal.png"),
                    "lazy hidden normal bitmap fixture should be written") && ok;
        ok = expect(writeBluePngFixture(lazySwapDir / "active.png"),
                    "lazy hidden active bitmap fixture should be written") && ok;

        constexpr std::string_view lazySwapHtml = R"html(
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
    .menu-item {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 20px;
      height: 20px;
    }
    .menu-image {
      position: absolute;
      left: 0px;
      top: 0px;
      width: 20px;
      height: 20px;
    }
    .active-image {
      display: none;
    }
    .menu-item.active .normal {
      display: none;
    }
    .menu-item.active .active-image {
      display: flex;
    }
  </style>
</head>
<body>
  <div class="root">
    <button id="button" class="menu-item">
      <img class="menu-image normal" src="normal.png">
      <img class="menu-image active-image" src="active.png" loading="lazy">
    </button>
  </div>
</body>
</html>
)html";

        std::atomic_int lazySwapRedraws{0};
        skui::RuntimeOptions lazySwapOptions = options;
        lazySwapOptions.assetRoot = utf8PathText(lazySwapDir);
        lazySwapOptions.requestRedraw = [&] {
            lazySwapRedraws.fetch_add(1);
        };
        skui::Runtime lazySwapRuntime(lazySwapOptions);
        lazySwapRuntime.resize(kWidth, kHeight, 1.0f);
        ok = expect(lazySwapRuntime.loadDocumentFromString(lazySwapHtml, utf8PathText(lazySwapDir)),
                    "lazy hidden bitmap swap document should load") && ok;

        uint32_t lazySwapPixel = 0;
        for (int retry = 0; retry < 100 && lazySwapRedraws.load() < 1; ++retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ok = expect(lazySwapRedraws.load() == 1,
                    "lazy hidden bitmap should not request while display none") && ok;
        ok = renderPixel(lazySwapRuntime, 20, 20, lazySwapPixel) && ok;
        ok = expect(isMostlyRed(lazySwapPixel),
                    "lazy swap should render visible normal bitmap after async load") && ok;
        ok = expect(lazySwapRuntime.addClassById("button", "active"),
                    "lazy hidden bitmap swap should switch to active class") && ok;
        ok = renderPixel(lazySwapRuntime, 20, 20, lazySwapPixel) && ok;
        ok = expect(lazySwapPixel == solidColor(0x00, 0x00, 0x00),
                    "lazy hidden active bitmap should not be ready when first shown") && ok;
        bool lazyActiveReady = false;
        for (int retry = 0; retry < 100 && !lazyActiveReady; ++retry) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            ok = renderPixel(lazySwapRuntime, 20, 20, lazySwapPixel) && ok;
            lazyActiveReady = isMostlyBlue(lazySwapPixel);
        }
        ok = expect(isMostlyBlue(lazySwapPixel),
                    "lazy hidden active bitmap should render after first visible load") && ok;

        std::filesystem::remove_all(lazySwapDir);
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
        ok = expect(
                 budgetPixel == solidColor(0x00, 0x00, 0x00) ||
                     isMostlyRed(budgetPixel),
                 "budgeted bitmap first render should be pending or ready") &&
             ok;
        waitForDirty(budgetRuntime);
        ok = renderPixel(budgetRuntime, 20, 20, budgetPixel) && ok;
        ok = expect(isMostlyRed(budgetPixel), "red bitmap should render after async load") && ok;

        ok = expect(budgetRuntime.setAttributeById("photo", "src", "blue.png"),
                    "bitmap src should switch to blue") && ok;
        ok = renderPixel(budgetRuntime, 20, 20, budgetPixel) && ok;
        ok = expect(isMostlyRed(budgetPixel),
                    "budgeted bitmap switch should keep drawing current red image") && ok;
        waitForDirty(budgetRuntime);
        ok = renderPixel(budgetRuntime, 20, 20, budgetPixel) && ok;
        ok = expect(isMostlyBlue(budgetPixel), "blue bitmap should render after async load") && ok;

        ok = expect(budgetRuntime.setAttributeById("photo", "src", "red.png"),
                    "bitmap src should switch back to red") && ok;
        ok = renderPixel(budgetRuntime, 20, 20, budgetPixel) && ok;
        ok = expect(isMostlyBlue(budgetPixel),
                    "evicted bitmap switch should keep drawing current blue image") && ok;
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
    html {
      width: 100%;
      height: 100%;
    }
    body {
      width: 100%;
      height: 100%;
      margin: 0;
    }
    .screen {
      width: 100%;
      height: 100%;
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

    constexpr std::string_view dynamicVirtualListHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root {
      position: relative;
      width: 180px;
      height: 140px;
      background-color: #000000;
    }
    .list {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 120px;
      height: 90px;
      overflow-y: auto;
      overflow-x: hidden;
      background-color: #111111;
      scrollbar-gutter: stable;
    }
    .content {
      position: relative;
      width: 104px;
      height: 60px;
    }
    .title {
      position: absolute;
      left: 4px;
      top: 4px;
      width: 60px;
      height: 18px;
      background-color: #112233;
    }
    .row {
      position: absolute;
      left: 4px;
      top: 30px;
      width: 60px;
      height: 20px;
      background-color: #223344;
      color: #ffffff;
      font-size: 12px;
    }
  </style>
</head>
<body>
  <div class="root">
    <div id="dynamic-list" class="list" data-virtual-height="60">
      <div id="dynamic-content" class="content" style="height: 60px;">
        <div id="dynamic-title" class="title"></div>
        <div id="dynamic-row" class="row" style="display: none;"></div>
      </div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime dynamicVirtualListRuntime(options);
    dynamicVirtualListRuntime.resize(kWidth, kHeight, 1.0f);
    if (!dynamicVirtualListRuntime.loadDocumentFromString(dynamicVirtualListHtml, "")) {
        std::cerr << "dynamic virtual list load failed: "
                  << dynamicVirtualListRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t dynamicVirtualTitle = 0;
    uint32_t dynamicVirtualRow = 0;
    ok = renderPixel(dynamicVirtualListRuntime, 18, 18, dynamicVirtualTitle) && ok;
    ok = expect(dynamicVirtualTitle == solidColor(0x11, 0x22, 0x33),
                "virtual list title should render inside a stable scrollbar container") && ok;
    ok = expect(dynamicVirtualListRuntime.applyUpdates(
                    {{{"dynamic-row",
                       "display:flex;position:absolute;left:4px;top:30px;width:60px;height:20px;"
                       "background-color:#abcdef;color:#ffffff;font-size:12px;"}},
                     {{"dynamic-row", "row 1"}},
                     {{"dynamic-list", "data-virtual-height", "120"},
                      {"dynamic-content", "style", "height: 120px;"}}}),
                "dynamic virtual list should accept batched style, text and attribute updates") && ok;
    ok = renderPixel(dynamicVirtualListRuntime, 18, 46, dynamicVirtualRow) && ok;
    ok = expect(dynamicVirtualRow == solidColor(0xAB, 0xCD, 0xEF),
                "pooled row shown by applyUpdates should render inside a virtual list") && ok;

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

    constexpr std::string_view flexGapHtml = R"html(
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
      left: 0px;
      top: 10px;
      width: 140px;
      height: 20px;
      display: flex;
      justify-content: center;
      gap: 10px;
    }
    .tool {
      width: 20px;
      height: 20px;
      background-color: #111111;
    }
    .a {
      background-color: #ff0000;
    }
    .b {
      background-color: #00ff00;
    }
    .c {
      background-color: #0000ff;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="toolbar">
      <div class="tool a"></div>
      <div class="tool b"></div>
      <div class="tool c"></div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime flexGapRuntime(options);
    flexGapRuntime.resize(kWidth, kHeight, 1.0f);
    if (!flexGapRuntime.loadDocumentFromString(flexGapHtml, "")) {
        std::cerr << "flex gap load failed: " << flexGapRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t firstGapProbe = 0;
    uint32_t gapProbe = 0;
    uint32_t secondGapProbe = 0;
    uint32_t thirdGapProbe = 0;
    ok = renderPixel(flexGapRuntime, 40, 18, firstGapProbe) && ok;
    ok = renderPixel(flexGapRuntime, 55, 18, gapProbe) && ok;
    ok = renderPixel(flexGapRuntime, 70, 18, secondGapProbe) && ok;
    ok = renderPixel(flexGapRuntime, 100, 18, thirdGapProbe) && ok;
    ok = expect(firstGapProbe == solidColor(0xFF, 0x00, 0x00),
                "display:flex should default to row and center children") && ok;
    ok = expect(gapProbe == solidColor(0x00, 0x00, 0x00),
                "gap should leave space between row flex children") && ok;
    ok = expect(secondGapProbe == solidColor(0x00, 0xFF, 0x00),
                "second flex child should appear after gap") && ok;
    ok = expect(thirdGapProbe == solidColor(0x00, 0x00, 0xFF),
                "third flex child should appear after second gap") && ok;

    constexpr std::string_view gridCardLayoutHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    * {
      box-sizing: border-box;
    }
    .root {
      width: 140px;
      height: 90px;
      display: flex;
      flex-direction: column;
      background-color: #000000;
    }
    .header {
      height: 20px;
      display: inline-flex;
      align-items: center;
      gap: 4px;
      flex: 0 0 auto;
    }
    .header-item {
      width: 10px;
      height: 10px;
      flex: 0 0 auto;
    }
    .header-a {
      background-color: #ff0000;
    }
    .header-b {
      background-color: #00ff00;
    }
    .grid {
      flex: 1;
      display: grid;
      grid-template-columns: repeat(3, 1fr);
      grid-auto-rows: 1fr;
    }
    .red { background-color: #ff0000; }
    .green { background-color: #00ff00; }
    .blue { background-color: #0000ff; }
    .cyan { background-color: #00ffff; }
    .magenta { background-color: #ff00ff; }
    .yellow { background-color: #ffff00; }
  </style>
</head>
<body>
  <div class="root">
    <div class="header">
      <div class="header-item header-a"></div>
      <div class="header-item header-b"></div>
    </div>
    <div class="grid">
      <div class="red"></div>
      <div class="green"></div>
      <div class="blue"></div>
      <div class="cyan"></div>
      <div class="magenta"></div>
      <div class="yellow"></div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime gridCardLayoutRuntime(options);
    gridCardLayoutRuntime.resize(kWidth, kHeight, 1.0f);
    if (!gridCardLayoutRuntime.loadDocumentFromString(
            gridCardLayoutHtml,
            "")) {
        std::cerr << "grid card layout load failed: "
                  << gridCardLayoutRuntime.lastError() << "\n";
        return 1;
    }
    std::vector<uint32_t> gridCardLayoutPixels;
    ok = renderPixels(gridCardLayoutRuntime, gridCardLayoutPixels) && ok;
    ok = expect(
        pixelAt(gridCardLayoutPixels, 5, 10) == solidColor(0xFF, 0x00, 0x00) &&
            pixelAt(gridCardLayoutPixels, 19, 10) == solidColor(0x00, 0xFF, 0x00),
        "inline-flex and flex shorthand should keep header items in one row") && ok;
    ok = expect(
        pixelAt(gridCardLayoutPixels, 20, 30) == solidColor(0xFF, 0x00, 0x00) &&
            pixelAt(gridCardLayoutPixels, 70, 30) == solidColor(0x00, 0xFF, 0x00) &&
            pixelAt(gridCardLayoutPixels, 120, 30) == solidColor(0x00, 0x00, 0xFF),
        "repeat(3, 1fr) should create three equal grid columns") && ok;
    ok = expect(
        pixelAt(gridCardLayoutPixels, 20, 72) == solidColor(0x00, 0xFF, 0xFF) &&
            pixelAt(gridCardLayoutPixels, 70, 72) == solidColor(0xFF, 0x00, 0xFF) &&
            pixelAt(gridCardLayoutPixels, 120, 72) == solidColor(0xFF, 0xFF, 0x00),
        "grid-auto-rows:1fr should distribute remaining height equally") && ok;

    constexpr std::string_view mixedGridTracksHtml = R"html(
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
    .metric {
      position: absolute;
      inset: 10px 10px 40px 10px;
      display: grid;
      grid-template-columns: 30px minmax(0, 1fr) auto;
      column-gap: 5px;
    }
    .icon { background-color: #ff0000; }
    .label { background-color: #00ff00; }
    .value {
      width: 20px;
      background-color: #0000ff;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="metric">
      <div class="icon"></div>
      <div class="label"></div>
      <div class="value"></div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime mixedGridTracksRuntime(options);
    mixedGridTracksRuntime.resize(kWidth, kHeight, 1.0f);
    if (!mixedGridTracksRuntime.loadDocumentFromString(
            mixedGridTracksHtml,
            "")) {
        std::cerr << "mixed grid tracks load failed: "
                  << mixedGridTracksRuntime.lastError() << "\n";
        return 1;
    }
    std::vector<uint32_t> mixedGridTracksPixels;
    ok = renderPixels(mixedGridTracksRuntime, mixedGridTracksPixels) && ok;
    ok = expect(
        pixelAt(mixedGridTracksPixels, 20, 20) == solidColor(0xFF, 0x00, 0x00) &&
            pixelAt(mixedGridTracksPixels, 50, 20) == solidColor(0x00, 0xFF, 0x00) &&
            pixelAt(mixedGridTracksPixels, 115, 20) == solidColor(0x00, 0x00, 0xFF),
        "fixed, minmax fraction, auto tracks and inset shorthand should share a row") && ok;

    constexpr std::string_view shadowHtml = R"html(
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
    .card {
      position: absolute;
      left: 20px;
      top: 20px;
      width: 40px;
      height: 30px;
      background-color: #223344;
      box-shadow:
        8px 0 0 #ff0000,
        inset 0 0 0 4px #00ff00;
    }
    .filtered {
      position: absolute;
      left: 90px;
      top: 20px;
      width: 10px;
      height: 10px;
      background-color: #0000ff;
      filter: drop-shadow(12px 0 0 #ff0000);
    }
    .shadow-text {
      position: absolute;
      left: 10px;
      top: 60px;
      width: 30px;
      height: 24px;
      color: #ffffff;
      font-size: 20px;
      text-shadow: 5px 0 0 #ff0000;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="card"></div>
    <div class="filtered"></div>
    <span class="shadow-text">I</span>
  </div>
</body>
</html>
)html";

    skui::Runtime shadowRuntime(options);
    shadowRuntime.resize(kWidth, kHeight, 1.0f);
    if (!shadowRuntime.loadDocumentFromString(shadowHtml, "")) {
        std::cerr << "shadow CSS load failed: "
                  << shadowRuntime.lastError() << "\n";
        return 1;
    }
    std::vector<uint32_t> shadowPixels;
    ok = renderPixels(shadowRuntime, shadowPixels) && ok;
    ok = expect(
        pixelAt(shadowPixels, 21, 35) == solidColor(0x00, 0xFF, 0x00) &&
            pixelAt(shadowPixels, 40, 35) == solidColor(0x22, 0x33, 0x44) &&
            pixelAt(shadowPixels, 64, 35) == solidColor(0xFF, 0x00, 0x00),
        "box-shadow should paint multiple outer and inset layers") && ok;
    ok = expect(
        pixelAt(shadowPixels, 94, 24) == solidColor(0x00, 0x00, 0xFF) &&
            pixelAt(shadowPixels, 106, 24) == solidColor(0xFF, 0x00, 0x00),
        "drop-shadow filter should preserve content and paint its shadow") && ok;
    int redTextShadowPixels = 0;
    for (int y = 60; y < 84; ++y) {
        for (int x = 10; x < 40; ++x) {
            const uint32_t color = pixelAt(shadowPixels, x, y);
            const unsigned red = (color >> 16u) & 0xFFu;
            const unsigned green = (color >> 8u) & 0xFFu;
            const unsigned blue = color & 0xFFu;
            if (red > 160u && green < 80u && blue < 80u) {
                ++redTextShadowPixels;
            }
        }
    }
    ok = expect(redTextShadowPixels > 0,
                "text-shadow should paint a colored offset glyph") && ok;

    constexpr std::string_view layeredGradientHtml = R"html(
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
    .layered-gradient {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 60px;
      height: 40px;
      background:
        linear-gradient(135deg, transparent 35%, #ff0000 65%),
        radial-gradient(circle at 0% 0%, #0000ff 0%, transparent 45%);
    }
    .tiled-gradient {
      position: absolute;
      left: 80px;
      top: 10px;
      width: 50px;
      height: 50px;
      background-image:
        linear-gradient(#ff0000 1px, transparent 1px),
        linear-gradient(90deg, #00ff00 1px, transparent 1px);
      background-size: 10px 10px;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="layered-gradient"></div>
    <div class="tiled-gradient"></div>
  </div>
</body>
</html>
)html";

    skui::Runtime layeredGradientRuntime(options);
    layeredGradientRuntime.resize(kWidth, kHeight, 1.0f);
    if (!layeredGradientRuntime.loadDocumentFromString(
            layeredGradientHtml,
            "")) {
        std::cerr << "layered gradient load failed: "
                  << layeredGradientRuntime.lastError() << "\n";
        return 1;
    }
    uint32_t radialOriginPixel = 0;
    uint32_t angledEndPixel = 0;
    std::vector<uint32_t> layeredGradientPixels;
    ok = renderPixels(layeredGradientRuntime, layeredGradientPixels) && ok;
    ok = renderPixel(
        layeredGradientRuntime,
        11,
        11,
        radialOriginPixel) && ok;
    ok = renderPixel(
        layeredGradientRuntime,
        68,
        48,
        angledEndPixel) && ok;
    ok = expect(
        isMostlyBlue(radialOriginPixel),
        "positioned radial gradient should paint below a transparent top layer") && ok;
    ok = expect(
        isMostlyRed(angledEndPixel),
        "angled top gradient and color stops should paint the opposite corner") && ok;
    ok = expect(
        isMostlyRed(pixelAt(layeredGradientPixels, 85, 10)) &&
            isMostlyRed(pixelAt(layeredGradientPixels, 85, 20)) &&
            pixelAt(layeredGradientPixels, 85, 15) == solidColor(0x00, 0x00, 0x00),
        "pixel gradient stops and background-size should repeat axis-aligned grid lines") && ok;

    constexpr std::string_view pseudoElementHtml = R"html(
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
    .items {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 120px;
      height: 30px;
      display: grid;
      grid-template-columns: repeat(3, 1fr);
    }
    .item {
      position: relative;
      background-color: #112233;
    }
    .item:nth-child(3n+1) {
      background-color: #00ff00;
    }
    .item:not(:nth-child(3n))::after {
      content: "";
      position: absolute;
      right: 0px;
      top: 5px;
      width: 2px;
      height: 20px;
      background-color: #ffffff;
    }
    .card {
      position: absolute;
      left: 10px;
      top: 50px;
      width: 30px;
      height: 30px;
      background-color: #00ff00;
    }
    .card::before {
      content: "";
      position: absolute;
      inset: 4px;
      background-color: #ff0000;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="items">
      <div class="item"></div>
      <div class="item"></div>
      <div class="item"></div>
    </div>
    <div class="card"></div>
  </div>
</body>
</html>
)html";

    skui::Runtime pseudoElementRuntime(options);
    pseudoElementRuntime.resize(kWidth, kHeight, 1.0f);
    if (!pseudoElementRuntime.loadDocumentFromString(
            pseudoElementHtml,
            "")) {
        std::cerr << "pseudo-element CSS load failed: "
                  << pseudoElementRuntime.lastError() << "\n";
        return 1;
    }
    std::vector<uint32_t> pseudoElementPixels;
    ok = renderPixels(pseudoElementRuntime, pseudoElementPixels) && ok;
    ok = expect(
        pixelAt(pseudoElementPixels, 20, 20) == solidColor(0x00, 0xFF, 0x00) &&
            pixelAt(pseudoElementPixels, 60, 20) == solidColor(0x11, 0x22, 0x33),
        "nth-child An+B selectors should style the matching grid item") && ok;
    ok = expect(
        pixelAt(pseudoElementPixels, 48, 20) == solidColor(0xFF, 0xFF, 0xFF) &&
            pixelAt(pseudoElementPixels, 88, 20) == solidColor(0xFF, 0xFF, 0xFF) &&
            pixelAt(pseudoElementPixels, 128, 20) == solidColor(0x11, 0x22, 0x33),
        ":not(), nth-child(), and ::after should paint only the first two separators") && ok;
    ok = expect(
        pixelAt(pseudoElementPixels, 11, 51) == solidColor(0x00, 0xFF, 0x00) &&
            pixelAt(pseudoElementPixels, 15, 55) == solidColor(0xFF, 0x00, 0x00),
        "absolutely positioned ::before should honor inset shorthand") && ok;

    constexpr std::string_view textAlignmentHtml = R"html(
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
    selectable {
      position: absolute;
      left: 10px;
      width: 100px;
      height: 32px;
      color: #ffffff;
      font-size: 16px;
      font-weight: bold;
      align-items: center;
      justify-content: center;
    }
    .normal {
      top: 8px;
    }
    .flex {
      top: 48px;
      display: flex;
    }
  </style>
</head>
<body>
  <div class="root">
    <selectable class="normal">Hi</selectable>
    <selectable class="flex">Hi</selectable>
  </div>
</body>
</html>
)html";

    skui::Runtime textAlignmentRuntime(options);
    textAlignmentRuntime.resize(kWidth, kHeight, 1.0f);
    if (!textAlignmentRuntime.loadDocumentFromString(textAlignmentHtml, "")) {
        std::cerr << "text alignment load failed: " << textAlignmentRuntime.lastError() << "\n";
        return 1;
    }
    std::vector<uint32_t> textAlignmentPixels;
    ok = renderPixels(textAlignmentRuntime, textAlignmentPixels) && ok;
    const int normalLeftText =
        countBrightPixels(textAlignmentPixels, 10, 8, 38, 24);
    const int normalCenteredText =
        countBrightPixels(textAlignmentPixels, 50, 20, 82, 36);
    const int flexLeftText =
        countBrightPixels(textAlignmentPixels, 10, 48, 38, 64);
    const int flexCenteredText =
        countBrightPixels(textAlignmentPixels, 50, 60, 82, 84);
    ok = expect(normalLeftText > 8 && normalCenteredText == 0,
                "align-items and justify-content should not align ordinary text without display:flex") && ok;
    ok = expect(flexCenteredText > 8 && flexLeftText == 0,
                "display:flex should align own text content") && ok;

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
    <div id="vertical" class="vertical" data-action="vertical-scroll">
      <div id="top" class="top"></div>
      <div id="bottom" class="bottom"></div>
    </div>
    <div id="horizontal" class="horizontal" data-action="horizontal-scroll">
      <div class="wide"></div>
    </div>
  </div>
</body>
</html>
)html";

    std::atomic_int scrollRedrawRequests{0};
    skui::RuntimeOptions scrollOptions = options;
    scrollOptions.requestRedraw = [&] {
        scrollRedrawRequests.fetch_add(1);
    };
    skui::Runtime scrollRuntime(scrollOptions);
    scrollRuntime.resize(kWidth, kHeight, 1.0f);
    if (!scrollRuntime.loadDocumentFromString(scrollHtml, "")) {
        std::cerr << "scroll load failed: " << scrollRuntime.lastError() << "\n";
        return 1;
    }

    uint32_t verticalInitial = 0;
    uint32_t verticalClipped = 0;
    uint32_t verticalWheelFirstFrame = 0;
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
    ok = renderPixel(scrollRuntime, 20, 20, verticalWheelFirstFrame) && ok;
    ok = expect(verticalWheelFirstFrame == solidColor(0xAB, 0xCD, 0xEF),
                "mouse wheel should immediately map its delta to scroll offset") &&
         ok;
    ok = expect(scrollRedrawRequests.load() == 0,
                "direct wheel scrolling should not request animation frames") &&
         ok;
    ok = renderAfterScroll(scrollRuntime) && ok;
    ok = renderPixel(scrollRuntime, 20, 20, verticalScrolled) && ok;
    ok = renderPixel(scrollRuntime, 90, 20, horizontalInitial) && ok;
    sendWheel(scrollRuntime, 90.0f, 20.0f, -240.0f, true);
    ok = renderAfterScroll(scrollRuntime) && ok;
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

    std::vector<skui::ElementEvent> programmaticScrollEvents;
    skui::RuntimeOptions programmaticScrollOptions = options;
    programmaticScrollOptions.onElementEvent = [&](const skui::ElementEvent& event) {
        if (event.type == skui::ElementEventType::Scroll) {
            programmaticScrollEvents.push_back(event);
        }
    };
    skui::Runtime programmaticScrollRuntime(programmaticScrollOptions);
    programmaticScrollRuntime.resize(kWidth, kHeight, 1.0f);
    if (!programmaticScrollRuntime.loadDocumentFromString(scrollHtml, "")) {
        std::cerr << "programmatic scroll load failed: "
                  << programmaticScrollRuntime.lastError() << "\n";
        return 1;
    }

    const std::optional<skui::ScrollState> initialVerticalState =
        programmaticScrollRuntime.scrollStateById("vertical");
    ok = expect(initialVerticalState.has_value(),
                "scrollStateById should expose scroll containers") && ok;
    if (initialVerticalState) {
        ok = expect(initialVerticalState->scrollY == 0.0f &&
                        initialVerticalState->maxScrollY == 60.0f &&
                        initialVerticalState->viewportHeight == 40.0f &&
                        initialVerticalState->contentHeight == 100.0f,
                    "scroll state should include offsets, range, viewport, and content") && ok;
    }
    ok = expect(!programmaticScrollRuntime.scrollStateById("top").has_value(),
                "scrollStateById should reject non-scroll containers") && ok;
    ok = expect(!programmaticScrollRuntime.scrollStateById("missing").has_value(),
                "scrollStateById should reject missing ids") && ok;

    ok = expect(programmaticScrollRuntime.setScrollOffsetById("vertical", 0.0f, 1000.0f),
                "setScrollOffsetById should accept a scroll container") && ok;
    const std::optional<skui::ScrollState> clampedVerticalState =
        programmaticScrollRuntime.scrollStateById("vertical");
    ok = expect(clampedVerticalState && clampedVerticalState->scrollY == 60.0f,
                "setScrollOffsetById should clamp to the maximum range") && ok;
    ok = expect(programmaticScrollEvents.size() == 1 &&
                    programmaticScrollEvents.back().id == "vertical" &&
                    programmaticScrollEvents.back().scrollY == 60.0f,
                "programmatic offset changes should emit a Scroll event") && ok;

    ok = expect(programmaticScrollRuntime.setScrollOffsetById("vertical", 0.0f, 60.0f),
                "setting the current scroll offset should still succeed") && ok;
    ok = expect(programmaticScrollEvents.size() == 1,
                "unchanged programmatic offsets should not emit Scroll events") && ok;

    ok = expect(programmaticScrollRuntime.scrollById("vertical", 0.0f, -15.0f),
                "scrollById should accept relative offsets") && ok;
    const std::optional<skui::ScrollState> relativeVerticalState =
        programmaticScrollRuntime.scrollStateById("vertical");
    ok = expect(relativeVerticalState && relativeVerticalState->scrollY == 45.0f,
                "scrollById should update the current offset") && ok;
    ok = expect(!programmaticScrollRuntime.setScrollOffsetById(
                    "vertical",
                    std::numeric_limits<float>::infinity(),
                    0.0f),
                "programmatic scrolling should reject non-finite offsets") && ok;
    ok = expect(!programmaticScrollRuntime.scrollById("top", 0.0f, 10.0f),
                "scrollById should reject non-scroll containers") && ok;

    ok = programmaticScrollRuntime.setScrollOffsetById("vertical", 0.0f, 0.0f) && ok;
    const size_t eventsBeforeIntoView = programmaticScrollEvents.size();
    ok = expect(programmaticScrollRuntime.scrollIntoViewById("bottom"),
                "scrollIntoViewById should accept an existing element") && ok;
    const std::optional<skui::ScrollState> intoViewState =
        programmaticScrollRuntime.scrollStateById("vertical");
    ok = expect(intoViewState && intoViewState->scrollY == 60.0f,
                "scrollIntoViewById should reveal an element using nearest alignment") && ok;
    ok = expect(programmaticScrollEvents.size() == eventsBeforeIntoView + 1,
                "scrollIntoViewById should emit Scroll for the changed ancestor") && ok;
    ok = expect(programmaticScrollRuntime.scrollIntoViewById("bottom"),
                "scrollIntoViewById should succeed when the element is already visible") && ok;
    ok = expect(programmaticScrollEvents.size() == eventsBeforeIntoView + 1,
                "already visible elements should not emit duplicate Scroll events") && ok;
    ok = expect(!programmaticScrollRuntime.scrollIntoViewById("missing"),
                "scrollIntoViewById should reject missing ids") && ok;

    constexpr std::string_view nestedScrollHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    .root { position:relative; width:140px; height:120px; background-color:#000000; }
    .outer { position:absolute; left:10px; top:10px; width:80px; height:60px; overflow:auto; }
    .inner { position:absolute; left:0px; top:80px; width:60px; height:40px; overflow:auto; }
    .target { position:absolute; left:0px; top:70px; width:20px; height:20px; }
  </style>
</head>
<body>
  <div class="root">
    <div id="outer" class="outer" data-action="outer-scroll">
      <div id="inner" class="inner" data-action="inner-scroll">
        <div id="nested-target" class="target"></div>
      </div>
    </div>
  </div>
</body>
</html>
)html";
    std::vector<std::string> nestedScrollEventIds;
    skui::RuntimeOptions nestedScrollOptions = options;
    nestedScrollOptions.onElementEvent = [&](const skui::ElementEvent& event) {
        if (event.type == skui::ElementEventType::Scroll) {
            nestedScrollEventIds.push_back(event.id);
        }
    };
    skui::Runtime nestedScrollRuntime(nestedScrollOptions);
    nestedScrollRuntime.resize(kWidth, 120, 1.0f);
    if (!nestedScrollRuntime.loadDocumentFromString(nestedScrollHtml, "")) {
        std::cerr << "nested programmatic scroll load failed: "
                  << nestedScrollRuntime.lastError() << "\n";
        return 1;
    }
    ok = expect(nestedScrollRuntime.scrollIntoViewById("nested-target"),
                "scrollIntoViewById should handle nested scroll containers") && ok;
    const std::optional<skui::ScrollState> innerScrollState =
        nestedScrollRuntime.scrollStateById("inner");
    const std::optional<skui::ScrollState> outerScrollState =
        nestedScrollRuntime.scrollStateById("outer");
    ok = expect(innerScrollState && innerScrollState->scrollY == 50.0f &&
                    outerScrollState && outerScrollState->scrollY == 60.0f,
                "scrollIntoViewById should adjust every scrollable ancestor") && ok;
    ok = expect(nestedScrollEventIds == std::vector<std::string>{"inner", "outer"},
                "nested scroll events should follow inner-to-outer order") && ok;

    std::vector<float> continuousScrollOffsets;
    skui::RuntimeOptions continuousScrollOptions = options;
    continuousScrollOptions.onElementEvent = [&](const skui::ElementEvent& event) {
        if (event.type == skui::ElementEventType::Scroll) {
            continuousScrollOffsets.push_back(event.scrollY);
        }
    };
    skui::Runtime continuousScrollRuntime(continuousScrollOptions);
    continuousScrollRuntime.resize(kWidth, kHeight, 1.0f);
    if (!continuousScrollRuntime.loadDocumentFromString(scrollHtml, "")) {
        std::cerr << "continuous scroll load failed: "
                  << continuousScrollRuntime.lastError() << "\n";
        return 1;
    }
    sendWheel(continuousScrollRuntime, 20.0f, 20.0f, -30.0f);
    sendWheel(continuousScrollRuntime, 20.0f, 20.0f, -30.0f);
    sendWheel(continuousScrollRuntime, 20.0f, 20.0f, -30.0f);
    ok = expect(continuousScrollOffsets.size() == 3,
                "continuous wheel input should emit each accepted scroll event") &&
         ok;
    if (continuousScrollOffsets.size() == 3) {
        ok = expect(
                 std::abs(continuousScrollOffsets[0] - 12.0f) <= 0.001f &&
                     std::abs(continuousScrollOffsets[1] - 24.0f) <= 0.001f &&
                     std::abs(continuousScrollOffsets[2] - 36.0f) <= 0.001f,
                 "each wheel input should immediately add its mapped offset") &&
             ok;
    }
    ok = renderAfterScroll(continuousScrollRuntime) && ok;
    uint32_t continuousScrollSettled = 0;
    ok = renderPixel(continuousScrollRuntime, 20, 35, continuousScrollSettled) && ok;
    ok = expect(continuousScrollSettled == solidColor(0xAB, 0xCD, 0xEF),
                "continuous wheel targets should accumulate to the final offset") &&
         ok;

    std::vector<float> directReverseOffsets;
    skui::RuntimeOptions directReverseOptions = options;
    directReverseOptions.onElementEvent = [&](const skui::ElementEvent& event) {
        if (event.type == skui::ElementEventType::Scroll) {
            directReverseOffsets.push_back(event.scrollY);
        }
    };
    skui::Runtime directReverseRuntime(directReverseOptions);
    directReverseRuntime.resize(kWidth, kHeight, 1.0f);
    if (!directReverseRuntime.loadDocumentFromString(scrollHtml, "")) {
        std::cerr << "direct reverse scroll load failed: "
                  << directReverseRuntime.lastError() << "\n";
        return 1;
    }
    sendWheel(directReverseRuntime, 20.0f, 20.0f, -120.0f);
    sendWheel(directReverseRuntime, 20.0f, 20.0f, -120.0f);
    sendWheel(directReverseRuntime, 20.0f, 20.0f, 120.0f);
    ok = expect(directReverseOffsets.size() == 3,
                "direct reverse scroll should emit each accepted wheel event") &&
         ok;
    if (directReverseOffsets.size() == 3) {
        ok = expect(
                 std::abs(directReverseOffsets[0] - 48.0f) <= 0.001f &&
                     std::abs(directReverseOffsets[1] - 60.0f) <= 0.001f &&
                     std::abs(directReverseOffsets[2] - 12.0f) <= 0.001f,
                 "reverse wheel input should immediately subtract its mapped "
                 "offset after boundary clamping") &&
             ok;
    }

    constexpr std::string_view dynamicListScrollHtml = R"html(
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
    .viewport {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 60px;
      height: 40px;
      overflow-y: auto;
      overflow-x: hidden;
      background-color: #111111;
    }
    .message-list {
      position: absolute;
      left: 0px;
      top: 0px;
      right: 0px;
      flex-direction: column;
    }
    .message {
      position: relative;
      height: 30px;
      flex-shrink: 0;
    }
    .first {
      background-color: #aa0000;
    }
    .second {
      background-color: #0000aa;
    }
    .third {
      background-color: #00aa00;
    }
  </style>
</head>
<body>
  <div class="root">
    <div class="viewport">
      <div id="message-list" class="message-list">
        <div class="message first"></div>
        <div id="second" class="message second"></div>
      </div>
    </div>
  </div>
</body>
</html>
)html";

    skui::Runtime dynamicListScrollRuntime(options);
    dynamicListScrollRuntime.resize(kWidth, kHeight, 1.0f);
    if (!dynamicListScrollRuntime.loadDocumentFromString(dynamicListScrollHtml, "")) {
        std::cerr << "dynamic list scroll load failed: "
                  << dynamicListScrollRuntime.lastError() << "\n";
        return 1;
    }

    uint32_t dynamicListInitial = 0;
    uint32_t dynamicListScrolled = 0;
    uint32_t dynamicListAppended = 0;
    uint32_t dynamicListHidden = 0;
    uint32_t dynamicListRemoved = 0;
    ok = renderPixel(dynamicListScrollRuntime, 20, 20, dynamicListInitial) && ok;
    sendWheel(dynamicListScrollRuntime, 20.0f, 20.0f, -240.0f);
    ok = renderAfterScroll(dynamicListScrollRuntime) && ok;
    ok = renderPixel(dynamicListScrollRuntime, 20, 20, dynamicListScrolled) && ok;
    ok = expect(
             dynamicListScrollRuntime.appendHtmlById(
                 "message-list",
                 R"html(<div id="third" class="message third"></div>)html"),
             "appendHtmlById should grow an absolutely positioned scroll list") &&
         ok;
    sendWheel(dynamicListScrollRuntime, 20.0f, 20.0f, -240.0f);
    ok = renderAfterScroll(dynamicListScrollRuntime) && ok;
    ok = renderPixel(dynamicListScrollRuntime, 20, 20, dynamicListAppended) && ok;
    ok = expect(dynamicListScrollRuntime.setVisibleById("second", false),
                "setVisibleById should shrink an absolutely positioned scroll list") &&
         ok;
    ok = renderPixel(dynamicListScrollRuntime, 20, 20, dynamicListHidden) && ok;
    ok = expect(dynamicListScrollRuntime.removeElementById("third"),
                "removeElementById should shrink an absolutely positioned scroll list") &&
         ok;
    ok = renderPixel(dynamicListScrollRuntime, 20, 20, dynamicListRemoved) && ok;
    ok = expect(dynamicListInitial == solidColor(0xAA, 0x00, 0x00),
                "dynamic scroll list should initially show the first item") &&
         ok;
    ok = expect(dynamicListScrolled == solidColor(0x00, 0x00, 0xAA),
                "dynamic scroll list should scroll to the second item") &&
         ok;
    ok = expect(dynamicListAppended == solidColor(0x00, 0xAA, 0x00),
                "appending an item should grow the dynamic scroll range") &&
         ok;
    ok = expect(dynamicListHidden == solidColor(0x00, 0xAA, 0x00),
                "display none should shrink and clamp the dynamic scroll range") &&
         ok;
    ok = expect(dynamicListRemoved == solidColor(0xAA, 0x00, 0x00),
                "removing the last item should reset the dynamic scroll range") &&
         ok;

    skui::Runtime scrollbarDragRuntime(options);
    scrollbarDragRuntime.resize(kWidth, kHeight, 1.0f);
    if (!scrollbarDragRuntime.loadDocumentFromString(scrollHtml, "")) {
        std::cerr << "scrollbar drag load failed: " << scrollbarDragRuntime.lastError() << "\n";
        return 1;
    }
    sendMouse(scrollbarDragRuntime, skui::EventType::MouseDown, 55.0f, 20.0f);
    sendMouse(scrollbarDragRuntime, skui::EventType::MouseMove, 55.0f, 30.0f);
    sendMouse(scrollbarDragRuntime, skui::EventType::MouseUp, 55.0f, 30.0f);
    sendMouse(scrollbarDragRuntime, skui::EventType::MouseMove, 55.0f, 14.0f);
    uint32_t scrollbarAfterRelease = 0;
    ok = renderPixel(scrollbarDragRuntime, 20, 20, scrollbarAfterRelease) && ok;
    ok = expect(scrollbarAfterRelease == solidColor(0xAB, 0xCD, 0xEF),
                "scrollbar thumb should stop following the pointer after mouse up") && ok;

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
    uint32_t textareaSelectionTop = 0;
    uint32_t textareaSelectionBoundary = 0;
    uint32_t textareaSelectionBottom = 0;
    ok = renderPixel(textareaRuntime, 20, 15, textareaSelectionTop) && ok;
    ok = renderPixel(textareaRuntime, 20, 29, textareaSelectionBoundary) && ok;
    ok = renderPixel(textareaRuntime, 20, 30, textareaSelectionBottom) && ok;
    ok = expect(textareaSelectionTop == textareaSelectionBoundary &&
                    textareaSelectionBoundary == textareaSelectionBottom,
                "textarea multiline selection should not draw darker seams between selected lines") &&
         ok;
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

    constexpr std::string_view cssRenderingParityHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    html, body {
      width: 100%;
      height: 100%;
      margin: 0;
      background: #000000;
    }
    .close-icon {
      position: absolute;
      left: 10px;
      top: 10px;
    }
    .edge-borders {
      position: absolute;
      box-sizing: border-box;
      left: 50px;
      top: 10px;
      width: 60px;
      height: 40px;
      background: #112233;
      border-top: 1px solid #ff0000;
      border-bottom: 2px solid #00ff00;
    }
    .masked {
      position: absolute;
      left: 10px;
      top: 55px;
      width: 40px;
      height: 30px;
      background: #ffffff;
      mask-image: linear-gradient(
        180deg,
        rgba(0, 0, 0, 1),
        rgba(0, 0, 0, 0)
      );
    }
    .inset-shadow {
      position: absolute;
      left: 70px;
      top: 55px;
      width: 60px;
      height: 30px;
      background: #ffffff;
      box-shadow: inset 0 0 18px rgba(0, 0, 0, 0.85);
    }
  </style>
</head>
<body>
  <svg class="close-icon" width="28" height="28" viewBox="0 0 24 24" fill="none">
    <path d="M6 6L18 18M18 6L6 18" stroke="#ffffff" stroke-width="2" />
  </svg>
  <div class="edge-borders"></div>
  <div class="masked"></div>
  <div class="inset-shadow"></div>
</body>
</html>
)html";

    skui::Runtime cssRenderingParityRuntime(options);
    cssRenderingParityRuntime.resize(kWidth, kHeight, 1.0f);
    if (!cssRenderingParityRuntime.loadDocumentFromString(
            cssRenderingParityHtml,
            "")) {
        std::cerr << "CSS rendering parity load failed: "
                  << cssRenderingParityRuntime.lastError() << "\n";
        return 1;
    }
    std::vector<uint32_t> cssRenderingParityPixels;
    ok = renderPixels(cssRenderingParityRuntime, cssRenderingParityPixels) && ok;
    ok = expect(
             countBrightPixels(cssRenderingParityPixels, 8, 8, 40, 40) > 20,
             "SVG width and height presentation attributes should size inline icons") &&
         ok;
    ok = expect(pixelAt(cssRenderingParityPixels, 70, 10) ==
                    solidColor(0xFF, 0x00, 0x00),
                "border-top shorthand should draw only the top edge") &&
         ok;
    ok = expect(pixelAt(cssRenderingParityPixels, 70, 49) ==
                    solidColor(0x00, 0xFF, 0x00),
                "border-bottom shorthand should draw the bottom edge") &&
         ok;
    ok = expect(pixelAt(cssRenderingParityPixels, 50, 25) ==
                    solidColor(0x11, 0x22, 0x33),
                "unspecified border edges should remain transparent") &&
         ok;
    const unsigned maskTop =
        (pixelAt(cssRenderingParityPixels, 30, 57) >> 16u) & 0xFFu;
    const unsigned maskBottom =
        (pixelAt(cssRenderingParityPixels, 30, 82) >> 16u) & 0xFFu;
    ok = expect(maskTop > 200u && maskBottom < 60u,
                "mask-image gradients should progressively attenuate element alpha") &&
         ok;
    const unsigned insetEdge =
        (pixelAt(cssRenderingParityPixels, 72, 70) >> 16u) & 0xFFu;
    const unsigned insetCenter =
        (pixelAt(cssRenderingParityPixels, 100, 70) >> 16u) & 0xFFu;
    ok = expect(insetCenter > insetEdge + 60u,
                "inset shadows should blur inward instead of drawing a hard stroke") &&
         ok;

    constexpr std::string_view flexAutoMarginHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    html, body { width:100%; height:100%; margin:0; background:#000000; }
    .row {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 120px;
      height: 30px;
      display: flex;
      align-items: center;
      gap: 8px;
    }
    .lead { width:20px; height:20px; background:#ff0000; }
    .close {
      width: 36px;
      height: 30px;
      margin-left: auto;
      display: flex;
      align-items: center;
      justify-content: center;
      color: #ffffff;
      border: 0;
      background: transparent;
    }
  </style>
</head>
<body>
  <div class="row">
    <div class="lead"></div>
    <button class="close">
      <svg width="28" height="28" viewBox="0 0 24 24" fill="none">
        <path d="M6 6l12 12M18 6L6 18" stroke="currentColor" stroke-width="2" />
      </svg>
    </button>
  </div>
</body>
</html>
)html";
    skui::Runtime flexAutoMarginRuntime(options);
    flexAutoMarginRuntime.resize(kWidth, kHeight, 1.0f);
    if (!flexAutoMarginRuntime.loadDocumentFromString(flexAutoMarginHtml, "")) {
        std::cerr << "flex auto margin load failed: "
                  << flexAutoMarginRuntime.lastError() << "\n";
        return 1;
    }
    std::vector<uint32_t> flexAutoMarginPixels;
    ok = renderPixels(flexAutoMarginRuntime, flexAutoMarginPixels) && ok;
    ok = expect(
             countBrightPixels(flexAutoMarginPixels, 100, 8, 132, 42) > 20,
             "flex gap should preserve a child's auto margin") &&
         ok;

    constexpr std::string_view lowContrastGradientHtml = R"html(
<!doctype html>
<html>
<head>
  <style>
    html, body { width:100%; height:100%; margin:0; background:#000000; }
    .gradient {
      position: absolute;
      left: 10px;
      top: 10px;
      width: 120px;
      height: 40px;
      background: linear-gradient(90deg, #102236, #112337);
    }
  </style>
</head>
<body><div class="gradient"></div></body>
</html>
)html";
    skui::Runtime lowContrastGradientRuntime(options);
    lowContrastGradientRuntime.resize(kWidth, kHeight, 1.0f);
    if (!lowContrastGradientRuntime.loadDocumentFromString(
            lowContrastGradientHtml,
            "")) {
        std::cerr << "low contrast gradient load failed: "
                  << lowContrastGradientRuntime.lastError() << "\n";
        return 1;
    }
    std::vector<uint32_t> lowContrastGradientPixels;
    ok = renderPixels(
             lowContrastGradientRuntime,
             lowContrastGradientPixels) &&
         ok;
    int longestGradientColorRun = 0;
    int currentGradientColorRun = 0;
    uint32_t previousGradientColor = 0;
    for (int x = 10; x < 130; ++x) {
        const uint32_t color = pixelAt(lowContrastGradientPixels, x, 30);
        if (x > 10 && color == previousGradientColor) {
            ++currentGradientColorRun;
        } else {
            currentGradientColorRun = 1;
            previousGradientColor = color;
        }
        longestGradientColorRun = std::max(
            longestGradientColorRun,
            currentGradientColorRun);
    }
    ok = expect(
             longestGradientColorRun <= 16,
             "low contrast gradients should dither instead of forming color bands") &&
         ok;

    runSpriteAnimationTest();
    return ok ? 0 : 1;
}
