#include "skui_runtime.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
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
    if (type == skui::EventType::MouseDown || type == skui::EventType::MouseUp) {
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

    bool ok = true;
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
