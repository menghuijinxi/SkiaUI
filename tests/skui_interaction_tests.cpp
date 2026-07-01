#include "skui_runtime.h"
#include "skui_virtual_table.h"

#include "include/core/SkData.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPixmap.h"
#include "include/encode/SkJpegEncoder.h"
#include "include/encode/SkPngEncoder.h"
#include "include/encode/SkWebpEncoder.h"

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

bool isMostlyRed(uint32_t color) {
    const unsigned red = (color >> 16u) & 0xFFu;
    const unsigned green = (color >> 8u) & 0xFFu;
    const unsigned blue = color & 0xFFu;
    return red > 180u && green < 80u && blue < 80u;
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
        for (int i = 0; i < 100 && !imageRuntime.dirty(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        ok = renderPixel(imageRuntime, 20, 20, imageAfter) && ok;
        ok = expect(imageBefore == solidColor(0x00, 0x00, 0x00),
                    std::string("async bitmap image should not block the first render: ") + fixture.fileName) && ok;
        ok = expect(imageRedraws.load() > 0,
                    std::string("async bitmap image load should request redraw: ") + fixture.fileName) && ok;
        ok = expect(isMostlyRed(imageAfter),
                    std::string("async bitmap image should render after loading: ") + fixture.fileName) && ok;
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
    ok = expect(virtualScrollbar == solidColor(0xB8, 0xC3, 0xD0), "virtual content height should create a scrollbar without real child height") && ok;
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
    ok = expect(verticalScrollbar == solidColor(0xB8, 0xC3, 0xD0), "overflow-y container should render a vertical scrollbar thumb") && ok;
    ok = expect(horizontalScrollbar == solidColor(0xB8, 0xC3, 0xD0), "overflow-x container should render a horizontal scrollbar thumb") && ok;
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
    ok = expect(stableGutter == solidColor(0x11, 0x11, 0x11), "scrollbar-gutter stable should reserve space outside child content") && ok;
    ok = expect(stableThumb == solidColor(0xB8, 0xC3, 0xD0), "scrollbar-gutter stable should still render a vertical thumb") && ok;

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
